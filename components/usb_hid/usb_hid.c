/**
 * @file usb_hid.c
 * @brief USB 复合设备"按需切换"管理器 (esp_tinyusb v2.x).
 *
 * 背景 (彻底解决 TF 卡挂载到电脑即崩溃/失败):
 *   ESP32-S3 的 USB-OTG 与 USB-Serial/JTAG 共用同一个内部 PHY, 同一时刻只能一个工作.
 *
 * 本方案采用与"虚拟键鼠"一致的按需切换 (该模式在本机已验证正常):
 *   - 正常模式 (开机默认): USB Serial/JTAG 串口控制台, 不装 TinyUSB.
 *   - 键鼠模式: 切换到 HID 键盘 + 鼠标 + CDC 复合设备.
 *   - 挂载模式: 切换到纯 MSC (标准 U盘) 设备.
 *   进入: 压制日志 → 卸载 Serial/JTAG → 装 TinyUSB.  退出: 卸 TinyUSB → 装回 Serial/JTAG.
 *   挂载期间由 usbh_msc 额外暂停串口控制台 (只写文件日志), 彻底避免日志写向已失效串口.
 *
 * 注: v2 的 esp_tinyusb 强定义了 tud_cdc_rx_cb/tud_mount_cb 等, 故这里不自定义它们;
 *     MSC 的 tud_msc_* 由官方 storage 层提供 (usbh_msc_sdspi.c 用 storage API).
 */
#include "usb_hid.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb_config.h"
#include "driver/usb_serial_jtag.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"
#include "class/cdc/cdc_device.h"
#include "class/msc/msc_device.h"
#include "class/bth/bth_device.h"

static const char *TAG = "USB_HID";

/* ---------------- 报告描述符 ---------------- */

/* 键盘报告描述符 (实例 0):
 * 偏移0: modifier(1字节) 偏移1: reserved(1字节) 偏移2-7: keycode[6] */
static const uint8_t keyboard_report_desc[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x05, 0x07,       /*   Usage Page (Key Codes) */
    0x19, 0xE0,       /*   Usage Minimum (224) */
    0x29, 0xE7,       /*   Usage Maximum (231) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x08,       /*   Report Count (8) */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute) -> modifier */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x08,       /*   Report Size (8) */
    0x81, 0x01,       /*   Input (Constant) -> reserved */
    0x95, 0x06,       /*   Report Count (6) */
    0x75, 0x08,       /*   Report Size (8) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x65,       /*   Logical Maximum (101) */
    0x05, 0x07,       /*   Usage Page (Key Codes) */
    0x19, 0x00,       /*   Usage Minimum (0) */
    0x29, 0x65,       /*   Usage Maximum (101) */
    0x81, 0x00,       /*   Input (Data, Array) -> keycode[6] */
    0xC0              /* End Collection */
};

/* 鼠标报告描述符 (实例 1):
 * 采用 TinyUSB 官方 TUD_HID_REPORT_DESC_MOUSE 模板 (5 字节: buttons + X + Y + Wheel + Pan, 全 Relative).
 * 必须 5 字节, 与 tud_hid_n_mouse_report() 实际发出的 5 字节报告一致!
 * 旧版手写 3 字节描述符 + HID_ITF_PROTOCOL_MOUSE(boot) 在 Windows 上被严格解析为 boot 3 字节格式,
 * 与设备实际发出的 5 字节报告长度不匹配 -> Windows 不识别鼠标 (macOS 宽松容忍故正常). */
static const uint8_t mouse_report_desc[] = {
    TUD_HID_REPORT_DESC_MOUSE()
};

/* ---------------- 设备 1: HID 键鼠 + CDC (键鼠模式, 照搬旧版验证过的描述符) ---------------- */

/* 接口枚举 */
enum { ITF_KEYBOARD = 0, ITF_MOUSE = 1, ITF_CDC = 2, ITF_CDC_DATA = 3, ITF_NUM_TOTAL };
enum { EP_KEYBOARD = 0x81, EP_MOUSE = 0x82, EP_CDC_NOTIF = 0x83, EP_CDC_OUT = 0x04, EP_CDC_IN = 0x84 };

#define HID_CONFIG_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

static uint8_t const hid_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, HID_CONFIG_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    TUD_HID_DESCRIPTOR(ITF_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(keyboard_report_desc), EP_KEYBOARD, 8, 10),
    TUD_HID_DESCRIPTOR(ITF_MOUSE, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(mouse_report_desc), EP_MOUSE, 8, 10),
    TUD_CDC_DESCRIPTOR(ITF_CDC, 0, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
};

/* ---------------- 设备 2: 纯 MSC (挂载模式, 标准 U盘) ---------------- */

/* 接口枚举: 0=MSC */
enum { ITF_MSC = 0, ITF_MSC_TOTAL };
enum { EP_MSC_OUT = 0x01, EP_MSC_IN = 0x81 };

#define MSC_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static uint8_t const msc_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_MSC_TOTAL, 0, MSC_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 0, EP_MSC_OUT, EP_MSC_IN, 64),
};

/* ---------------- 设备 3: USB-BT (蓝牙适配器, 标准 HCI BTH) ---------------- */

/* 接口: 0=BTH-primary, 1=BTH-ISO(需 ISO_ALT_COUNT≥1 供主机蓝牙驱动识别).
 * EP: evt/interrupt + acl in/out(bulk) + iso in/out. */
enum { ITF_BTH = 0, ITF_BTH_ISO = 1, ITF_BTH_TOTAL = 2 };
enum { EP_BTH_EVT = 0x81, EP_BTH_IN = 0x82, EP_BTH_OUT = 0x02, EP_BTH_ISO_IN = 0x83, EP_BTH_ISO_OUT = 0x03 };

/* BTH 配置描述符长度 by 官方宏 (ISO_ALT_COUNT=1 → 8+9+7+7+7+23 = 61) */
#define BTH_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_BTH_DESC_LEN)

static uint8_t const bth_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_BTH_TOTAL, 0, BTH_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    /* evt EP16 间隔1, acl in/out 64, 1 个 ISO alt (in/out 64) */
    TUD_BTH_DESCRIPTOR(ITF_BTH, 0, EP_BTH_EVT, 16, 1, EP_BTH_IN, EP_BTH_OUT, 64, 64),
};

static const char *bth_string_desc_arr[] = {
    (char[]){0x09, 0x04}, "Espressif Systems", "Espresso USB-BT", "123456",
};

/* ---------------- 设备 4: USB 网卡共享 (RNDIS 有线网卡, Windows 原生识别) ----------------
 * 接口: 0=CDC 通信(通知, 中断 IN), 1=CDC 数据(网络数据, 批量 OUT/IN).
 * 用官方 TUD_RNDIS_DESCRIPTOR (IAD + CDC+ACM+RNDIS 功能描述符 + 端点).
 * 网络数据通路(tud_network_* 回调 + esp_netif/lwIP)见 usb_net.c. */
enum { ITF_NET = 0, ITF_NET_DATA = 1, ITF_NET_TOTAL = 2 };
enum { EP_NET_NOTIF = 0x81, EP_NET_OUT = 0x02, EP_NET_IN = 0x82 };

#define NET_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_RNDIS_DESC_LEN)

static uint8_t const net_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NET_TOTAL, 0, NET_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    /* 接口0, 无接口字符串, notif IN(8), 数据 OUT/IN(64) */
    TUD_RNDIS_DESCRIPTOR(ITF_NET, 0, EP_NET_NOTIF, 8, EP_NET_OUT, EP_NET_IN, 64),
};

static const char *net_string_desc_arr[] = {
    (char[]){0x09, 0x04}, "Espressif Systems", "Espresso USB Net", "123456",
};

/* ---------------- 字符串描述符 ---------------- */

/* [0]=langid, [1]=manufacturer, [2]=product, [3]=serial */
static const char *hid_string_desc_arr[] = {
    (char[]){0x09, 0x04}, "Espressif Systems", "Espresso HID KeyMouse", "123456",
};
static const char *msc_string_desc_arr[] = {
    (char[]){0x09, 0x04}, "Espressif Systems", "Espresso USB Storage", "123456",
};

/* ---------------- HID 回调 (TinyUSB) ---------------- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    switch (instance) {
    case 0: return keyboard_report_desc;
    case 1: return mouse_report_desc;
    default: return NULL;
    }
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
    /* 本项目无 OUT 报告, 空实现即可 */
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0; /* 不支持 GET_REPORT, 让 stack STALL */
}

/* ---------------- 设备事件回调 (v2, 不做日志避免 TinyUSB 任务栈压力) ---------------- */

static void usb_device_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    (void)event;
    /* 事件回调运行在 TinyUSB 任务, 不打日志 (日志走 CDC 会在该任务栈上递归过深) */
}

/* ---------------- 模式切换管理 ---------------- */

typedef enum {
    USB_MODE_NORMAL = 0,   /* USB Serial/JTAG 控制台, 无 TinyUSB */
    USB_MODE_HID,          /* 键鼠: HID 键盘 + 鼠标 + CDC */
    USB_MODE_MSC,          /* 挂载: MSC + CDC */
    USB_MODE_BTH,          /* USB-BT: 蓝牙适配器 HCI */
    USB_MODE_NET,          /* USB 网卡共享: USB-RNDIS 有线网卡 */
} usb_mode_t;

static usb_mode_t s_mode = USB_MODE_NORMAL;

/* 切换期间日志接管: 只丢弃输出, 不写任何串口 (切换窗口几 ms, 丢失可忽略) */
static int usb_log_discard(const char *fmt, va_list arg)
{
    (void)fmt; (void)arg;
    return 0;
}

/* 进入 TinyUSB 模式: 卸载 Serial/JTAG → 装复合/纯 MSC 设备.
 * 全程压制日志, 避免任何日志写向已卸载的 Serial/JTAG.
 * 注: 挂载模式由调用方 (usbh_msc) 额外暂停串口控制台 (只写文件), 键鼠模式与旧版一致. */
static esp_err_t usb_mode_enter(const uint8_t *config_desc, const char **strings, int str_count)
{
    vprintf_like_t prev = esp_log_set_vprintf(&usb_log_discard);

    /* 释放 Serial/JTAG (与虚拟键鼠一致的切换序列), 稍等稳定 */
    esp_err_t sj_ret = usb_serial_jtag_driver_uninstall();
    if (sj_ret != ESP_OK) {
        ESP_LOGE(TAG, "卸载 Serial/JTAG 失败: %s", esp_err_to_name(sj_ret));
        esp_log_set_vprintf(prev);
        return sj_ret;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(usb_device_event_cb);
    /* 加大 TinyUSB 任务栈, 避免任务内日志路径过深溢出 */
    cfg.task = TINYUSB_TASK_CUSTOM(8192, 5, 1);
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = str_count;
    cfg.descriptor.full_speed_config = config_desc;
    cfg.descriptor.qualifier = NULL;
    cfg.descriptor.high_speed_config = NULL;
    cfg.phy.self_powered = true;
    cfg.phy.vbus_monitor_io = -1;

    esp_err_t ret = tinyusb_driver_install(&cfg);
    if (ret != ESP_OK) {
        /* 失败: 装回 Serial/JTAG 驱动, 恢复原控制台, 避免串口永久失效 */
        ESP_LOGE(TAG, "进入 TinyUSB 模式失败: %s", esp_err_to_name(ret));
        usb_serial_jtag_driver_config_t sj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usb_serial_jtag_driver_install(&sj_cfg);
        esp_log_set_vprintf(prev);
        return ret;
    }
    esp_log_set_vprintf(prev);
    return ESP_OK;
}

/* 退出 TinyUSB 模式: 卸 TinyUSB → 装回 Serial/JTAG 驱动 (恢复串口控制台). */
static void usb_mode_exit(void)
{
    if (s_mode == USB_MODE_NORMAL) return;
    vprintf_like_t prev = esp_log_set_vprintf(&usb_log_discard);

    /* v2 的 tinyusb_driver_uninstall 会停任务/释放 OTG PHY */
    tinyusb_driver_uninstall();

    /* 装回 Serial/JTAG 驱动 (自动重挂 /dev/usbserjtag VFS) */
    usb_serial_jtag_driver_config_t sj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&sj_cfg);

    esp_log_set_vprintf(prev);
    s_mode = USB_MODE_NORMAL;
}

/* 切换到键鼠 (HID+CDC) 复合设备 */
esp_err_t usb_composite_enter_hid(void)
{
    if (s_mode == USB_MODE_HID) return ESP_OK;
    if (s_mode != USB_MODE_NORMAL) usb_mode_exit();   /* 防御: 先回正常模式 */
    esp_err_t ret = usb_mode_enter(hid_config_desc, hid_string_desc_arr, 4);
    if (ret == ESP_OK) s_mode = USB_MODE_HID;
    return ret;
}

/* 切换到挂载 (纯 MSC U盘) 设备 */
esp_err_t usb_composite_enter_msc(void)
{
    if (s_mode == USB_MODE_MSC) return ESP_OK;
    if (s_mode != USB_MODE_NORMAL) usb_mode_exit();   /* 防御: 先回正常模式 */
    esp_err_t ret = usb_mode_enter(msc_config_desc, msc_string_desc_arr, 4);
    if (ret == ESP_OK) s_mode = USB_MODE_MSC;
    return ret;
}

/* 切换到 USB-BT (蓝牙适配器 HCI) 设备 */
esp_err_t usb_composite_enter_bth(void)
{
    if (s_mode == USB_MODE_BTH) return ESP_OK;
    if (s_mode != USB_MODE_NORMAL) usb_mode_exit();   /* 防御: 先回正常模式 */
    esp_err_t ret = usb_mode_enter(bth_config_desc, bth_string_desc_arr, 4);
    if (ret == ESP_OK) s_mode = USB_MODE_BTH;
    return ret;
}

/* 切换到 USB-RNDIS 网卡设备 (USB 有线网卡, Windows 原生识别).
 * 依赖 sdkconfig 使能 CONFIG_TINYUSB_NET_MODE_ECM_RNDIS (编译 net 类).
 * 数据通路 (tud_network_mac_address + tud_network_* 回调 + esp_netif/lwIP) 由 usb_net.c 提供. */
esp_err_t usb_composite_enter_net(void)
{
    if (s_mode == USB_MODE_NET) return ESP_OK;
    if (s_mode != USB_MODE_NORMAL) usb_mode_exit();   /* 防御: 先回正常模式 */
    esp_err_t ret = usb_mode_enter(net_config_desc, net_string_desc_arr, 4);
    if (ret == ESP_OK) s_mode = USB_MODE_NET;
    return ret;
}

/* 退出任意 TinyUSB 模式, 回到正常 (Serial/JTAG 控制台) */
void usb_composite_exit(void)
{
    usb_mode_exit();
}

/* 当前是否处于某个 TinyUSB 模式且已被主机枚举 */
bool usb_composite_is_connected(void)
{
    return s_mode != USB_MODE_NORMAL && tud_mounted();
}

/* ---------------- 对外 API (键鼠) ---------------- */

/* HID 实例: 0=键盘, 1=鼠标 */
#define HID_ITF_KB 0
#define HID_ITF_MS 1

static bool s_running = false;       /* 键鼠模式是否启用 */
static uint8_t s_mouse_buttons = 0;

esp_err_t usb_hid_start(void)
{
    if (s_running) return ESP_OK;
    esp_err_t ret = usb_composite_enter_hid();
    if (ret == ESP_OK) {
        s_running = true;
        ESP_LOGI(TAG, "键鼠模式已启用 (HID 键盘+鼠标+CDC, 控制台走 CDC)");
    } else {
        ESP_LOGE(TAG, "键鼠模式启动失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

void usb_hid_stop(void)
{
    if (!s_running) return;
    usb_hid_key_release();
    usb_hid_mouse_release(MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT | MOUSE_BUTTON_MIDDLE);
    s_running = false;
    usb_composite_exit();
    ESP_LOGI(TAG, "键鼠模式已退出, 已恢复 Serial/JTAG 控制台");
}

bool usb_hid_is_running(void) { return s_running; }

bool usb_hid_connected(void) { return s_running && tud_mounted(); }

/* ---------------- 键盘操作 ---------------- */

static bool kb_ready(void)
{
    return s_running && tud_hid_n_ready(HID_ITF_KB);
}

void usb_hid_key_tap(uint8_t modifier, uint8_t keycode)
{
    uint8_t buf[6] = {0, 0, 0, 0, 0, 0};
    if (!kb_ready()) return;
    buf[0] = keycode;
    tud_hid_n_keyboard_report(HID_ITF_KB, 0, modifier, buf);
    vTaskDelay(pdMS_TO_TICKS(25));
    if (!kb_ready()) return;
    uint8_t empty[6] = {0, 0, 0, 0, 0, 0};
    tud_hid_n_keyboard_report(HID_ITF_KB, 0, 0, empty);
    vTaskDelay(pdMS_TO_TICKS(25));
}

void usb_hid_key_press(uint8_t modifier, const uint8_t *keycodes, uint8_t n)
{
    uint8_t buf[6] = {0, 0, 0, 0, 0, 0};
    if (!kb_ready()) return;
    if (n > 6) n = 6;
    for (uint8_t i = 0; i < n; i++) buf[i] = keycodes[i];
    tud_hid_n_keyboard_report(HID_ITF_KB, 0, modifier, buf);
}

void usb_hid_key_release(void)
{
    if (!s_running) return;
    uint8_t empty[6] = {0, 0, 0, 0, 0, 0};
    if (kb_ready()) {
        tud_hid_n_keyboard_report(HID_ITF_KB, 0, 0, empty);
    }
}

/* ---------------- 鼠标操作 ---------------- */

static bool ms_ready(void)
{
    return s_running && tud_hid_n_ready(HID_ITF_MS);
}

void usb_hid_mouse_move(int8_t dx, int8_t dy)
{
    if (!ms_ready()) return;
    tud_hid_n_mouse_report(HID_ITF_MS, 0, s_mouse_buttons, dx, dy, 0, 0);
}

void usb_hid_mouse_click(uint8_t buttons)
{
    if (!ms_ready()) return;
    tud_hid_n_mouse_report(HID_ITF_MS, 0, buttons, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    if (!ms_ready()) return;
    tud_hid_n_mouse_report(HID_ITF_MS, 0, 0, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
}

void usb_hid_mouse_press(uint8_t buttons)
{
    s_mouse_buttons |= buttons;
    if (!ms_ready()) return;
    tud_hid_n_mouse_report(HID_ITF_MS, 0, s_mouse_buttons, 0, 0, 0, 0);
}

void usb_hid_mouse_release(uint8_t buttons)
{
    s_mouse_buttons &= (uint8_t)~buttons;
    if (!ms_ready()) return;
    tud_hid_n_mouse_report(HID_ITF_MS, 0, s_mouse_buttons, 0, 0, 0, 0);
}

/* ---------------- CDC 串口 (复合设备附加) ---------------- */

/* 向 PC CDC 串口发送数据. 返回实际写入字节数; 未运行/未连接返回 0. */
size_t usb_hid_cdc_write(const uint8_t *data, size_t len)
{
    if (s_mode == USB_MODE_NORMAL || !tud_cdc_connected()) return 0;
    size_t n = tud_cdc_write(data, len);
    tud_cdc_write_flush();
    return n;
}

/* CDC 串口是否已连接 (PC 打开该串口后 DTR 置位) */
bool usb_hid_cdc_connected(void)
{
    return s_mode != USB_MODE_NORMAL && tud_cdc_connected();
}

/* ---------------- CDC 接收 (信息回传: 目标电脑经串口发日志回设备) ----------------
 * 非回调实现: 调用方 (日志诊断页 poll) 周期调用 usb_hid_cdc_read 拉取 TinyUSB 接收
 * FIFO, 不依赖 tud_cdc_rx_cb (esp_tinyusb v2 内部强定义). */

/* 读取 PC 经 CDC 串口发来的数据 (回传). 返回实际读取字节数; 无数据/未连接返回 0. */
size_t usb_hid_cdc_read(uint8_t *data, size_t len)
{
    if (!data || len == 0 || s_mode == USB_MODE_NORMAL || !tud_cdc_connected()) return 0;
    size_t avail = tud_cdc_available();
    if (avail == 0) return 0;
    if (avail > len) avail = len;
    return tud_cdc_read(data, avail);
}
