/**
 * @file usb_hid.c
 * @brief USB HID 复合设备 (键盘 itf 0 + 鼠标 itf 1).
 *
 * 与 usbh_msc 相同的外设切换模式:
 *   - usb_hid_start(): 卸载 USB Serial/JTAG (释放 GPIO 19/20), 安装 TinyUSB,
 *     向电脑呈现一个复合 HID 设备 = 键盘 + 鼠标.
 *   - usb_hid_stop(): 卸载 TinyUSB, 恢复 USB Serial/JTAG (回到串口日志).
 *
 * esp_tinyusb 默认描述符不包含 HID 接口, 因此这里通过 tinyusb_driver_install
 * 传入自定义的 configuration descriptor, 并使用 TUD_HID_DESCRIPTOR 拼装
 * 键盘 + 鼠标两个接口.
 *
 * HID 报告发送走原生 tinyusb 的 tud_hid_n_keyboard_report / tud_hid_n_mouse_report:
 *   实例 0 = 键盘 (itf 0), 实例 1 = 鼠标 (itf 1).
 */
#include "usb_hid.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb_tasks.h"
#include "tusb_config.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"
#include "esp_private/usb_phy.h"

static const char *TAG = "USB_HID";

/* ---------------- 描述符 ---------------- */

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
 * 偏移0: buttons(1字节) 偏移1: x(int8) 偏移2: y(int8) */
static const uint8_t mouse_report_desc[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x02,       /* Usage (Mouse) */
    0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x01,       /*   Usage (Pointer) */
    0xA1, 0x00,       /*   Collection (Physical) */
    0x05, 0x09,       /*     Usage Page (Buttons) */
    0x19, 0x01,       /*     Usage Minimum (1) */
    0x29, 0x03,       /*     Usage Maximum (3) */
    0x15, 0x00,       /*     Logical Minimum (0) */
    0x25, 0x01,       /*     Logical Maximum (1) */
    0x95, 0x03,       /*     Report Count (3) */
    0x75, 0x01,       /*     Report Size (1) */
    0x81, 0x02,       /*     Input (Data, Variable, Absolute) -> buttons */
    0x95, 0x01,       /*     Report Count (1) */
    0x75, 0x05,       /*     Report Size (5) */
    0x81, 0x01,       /*     Input (Constant) -> padding */
    0x05, 0x01,       /*     Usage Page (Generic Desktop) */
    0x09, 0x30,       /*     Usage (X) */
    0x09, 0x31,       /*     Usage (Y) */
    0x15, 0x81,       /*     Logical Minimum (-127) */
    0x25, 0x7F,       /*     Logical Maximum (127) */
    0x75, 0x08,       /*     Report Size (8) */
    0x95, 0x02,       /*     Report Count (2) */
    0x81, 0x06,       /*     Input (Data, Variable, Relative) -> x,y */
    0xC0,             /*   End Collection */
    0xC0              /* End Collection */
};

/* 接口枚举 */
enum { ITF_KEYBOARD = 0, ITF_MOUSE = 1, ITF_NUM_TOTAL };
enum { EP_KEYBOARD = 0x81, EP_MOUSE = 0x82 };

#define HID_CONFIG_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN)

static uint8_t const hid_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, HID_CONFIG_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    TUD_HID_DESCRIPTOR(ITF_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(keyboard_report_desc), EP_KEYBOARD, 8, 10),
    TUD_HID_DESCRIPTOR(ITF_MOUSE, 0, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(mouse_report_desc), EP_MOUSE, 8, 10),
};

/* 字符串描述符: [0]=langid, [1]=manufacturer, [2]=product, [3]=serial */
static const char *string_desc_arr[] = {
    (char[]){0x09, 0x04}, "Espressif Systems", "Espresso HID KeyMouse", "123456",
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

/* ---------------- 状态 ---------------- */

static bool s_running = false;
static bool s_jtag_was_running = false;
/* V1.0.72: stop 时 usb_new_phy(SERIAL_JTAG) 占用的 fsls PHY 句柄.
 * start 时先 usb_del_phy 释放, 否则 tinyusb 内部 usb_new_phy(OTG) 会因
 * "selected PHY is in use" 报 ESP_ERR_INVALID_STATE, 导致退出后重进仿真键鼠失败. */
static usb_phy_handle_t s_serial_phy_hdl = NULL;

static uint8_t s_key_modifier = 0;
static bool   s_key_held = false;
static uint8_t s_mouse_buttons = 0;

/* 键盘实例 */
#define HID_ITF_KB 0
/* 鼠标实例 */
#define HID_ITF_MS 1

/* ---------------- 对外 API ---------------- */

/* V1.0.74: 调试 - 读取 fsls (internal) PHY 当前占用状态, 打印方便定位重进失败 */
static void dbg_dump_phy_state(void)
{
    usb_phy_status_t st;
    if (usb_phy_get_phy_status(USB_PHY_TARGET_INT, &st) != ESP_OK) {
        ESP_LOGI(TAG, "[PHY] fsls 状态读取失败(未安装?)");
        return;
    }
    ESP_LOGI(TAG, "[PHY] fsls=%s, s_serial_phy_hdl=%p", st ? "IN_USE" : "FREE", (void *)s_serial_phy_hdl);
}

esp_err_t usb_hid_start(void)
{
    if (s_running) return ESP_OK;
    ESP_LOGI(TAG, "启动 USB HID 复合设备 (键盘 + 鼠标)...");
    dbg_dump_phy_state();

    /* 对齐 usbh_msc 已验证模式: 无条件卸载 Serial/JTAG 驱动 (未安装时 IDF
     * 返回 ESP_OK 仅打 warning), 彻底释放 GPIO 19/20 给 OTG.
     * 旧版用 is_driver_installed 判断会因日志走 ROM console 误判"未安装"
     * 跳过卸载, 导致 PHY 被占; 且 s_serial_phy_hdl 跨次保存/释放会在重进时
     * usb_del_phy 误删有效句柄, 破坏 usb_phy 子系统 ("USB_PHY is not
     * initialized"), 第二次进入键盘/触控板失效. */
    s_jtag_was_running = usb_serial_jtag_is_driver_installed();
    esp_err_t jret = usb_serial_jtag_driver_uninstall();
    if (jret != ESP_OK) {
        ESP_LOGE(TAG, "卸载 Serial/JTAG 失败: %s (0x%x)", esp_err_to_name(jret), jret);
        return jret;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 不再维护 s_serial_phy_hdl 跨次句柄 (该机制实测破坏 PHY 子系统) */
    s_serial_phy_hdl = NULL;

    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .self_powered = true,
        .vbus_monitor_io = -1,
        .device_descriptor = NULL,             /* 用 esp_tinyusb 默认设备描述符 */
        .configuration_descriptor = hid_config_desc, /* 自定义: 键盘+鼠标 */
        .string_descriptor = string_desc_arr,
        .string_descriptor_count = 4,
    };
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install 失败: %s (0x%x)", esp_err_to_name(ret), ret);
        if (s_jtag_was_running) {
            usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
            usb_serial_jtag_driver_install(&cfg);
        }
        return ret;
    }
    ESP_LOGI(TAG, "[HID] tinyusb_driver_install 成功");

    s_running = true;
    ESP_LOGI(TAG, "USB HID 已启动, 请连接电脑 USB 以枚举为键盘+鼠标");
    ESP_LOGI(TAG, "[HID] start 完成: is_running=%d jtag_was=%d phy_hdl=%p",
             (int)s_running, (int)s_jtag_was_running, (void *)s_serial_phy_hdl);
    return ESP_OK;
}

void usb_hid_stop(void)
{
    if (!s_running) return;
    ESP_LOGI(TAG, "停止 USB HID...");
    usb_hid_key_release();
    usb_hid_mouse_release(MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT | MOUSE_BUTTON_MIDDLE);

    /* 先停掉 tud_task 后台任务再卸载: esp_tinyusb 的 tinyusb_driver_uninstall()
     * 只会 usb_del_phy() 释放 OTG PHY, 并不会 tusb_stop_task() 停掉后台死循环.
     * 残留 tud_task 会持续驱动/占用 USB PHY 寄存器. */
    tusb_stop_task();
    /* tusb_deinit: 彻底释放 tusb 栈内部状态 (DCD/队列/互斥锁/_usbd_rhport).
     * 不调 tusb_deinit 则 tud_inited() 仍为 true, 下次 tinyusb_driver_install 时
     * tud_rhport_init 见已初始化直接跳过 DCD 重配 → 二次打开键鼠失效. */
    tusb_deinit(0);
    tinyusb_driver_uninstall();
    s_running = false;

    /* v5.5.5 下不再显式 usb_new_phy(SERIAL_JTAG) 切回 PHY 控制器.
     *
     * 关键修复背景 (esp-idf #9826/#15912): 旧方案在 stop 里用 usb_new_phy()
     * 切回 SERIAL_JTAG, 但该方法产生的 fsls PHY 句柄一旦丢弃就成"无主占用",
     * 且注释里 s_serial_phy_hdl 跨次保存/释放又可能误删有效句柄破坏 usb_phy
     * 子系统. 实测二次 start 时报 "selected PHY is in use" (0x103) 启动失败,
     * 界面闪退回主菜单. v5.5.5 的 tinyusb_driver_uninstall() 内部已正确
     * usb_del_phy 释放 OTG PHY, 无需手动切回 (与 GitHub 参考版 usbh_msc 一致).
     *
     * 这里直接重装 USB Serial/JTAG 驱动恢复到串口日志即可. */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t sj_ret = usb_serial_jtag_driver_install(&cfg);
    if (sj_ret == ESP_OK) {
        ESP_LOGI(TAG, "USB Serial/JTAG 驱动已重装, 串口已恢复 (无需重启)");
    } else {
        ESP_LOGW(TAG, "重装 USB Serial/JTAG 驱动失败: %s", esp_err_to_name(sj_ret));
    }
    s_serial_phy_hdl = NULL;
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
    s_key_modifier = modifier;
    s_key_held = true;
    tud_hid_n_keyboard_report(HID_ITF_KB, 0, modifier, buf);
}

void usb_hid_key_release(void)
{
    if (!s_running) return;
    uint8_t empty[6] = {0, 0, 0, 0, 0, 0};
    s_key_modifier = 0;
    s_key_held = false;
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