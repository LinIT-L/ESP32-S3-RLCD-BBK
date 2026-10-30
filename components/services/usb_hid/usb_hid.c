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

    /* ★ 2026-10-29 新增两个"失败态"(不是正常运行态, 只用于如实记录 + 允许重试):
     *   不加这两个态的话, exit 失败会被一律写成 NORMAL, 而 exit 的第一句就是
     *   "if (s_mode == NORMAL) return", 于是重试入口自己把自己锁死 —— 只能断电重启。 */
    USB_MODE_RECOVER_FAIL, /* TinyUSB 已确认卸净(PHY pad 已 disable), 但 Serial/JTAG 没装回来
                            *   ⇒ 重试动作 = **只装 SJ**(不必再卸 TinyUSB, 否则是重复卸载) */
    USB_MODE_TINYUSB_STUCK,/* TinyUSB 卸载返回错误(task_stop / usb_del_phy 失败)
                            *   ⇒ PHY 可能仍归 OTG, 此时直接装 SJ 会两边抢 pad
                            *   ⇒ 重试动作 = **先卸 TinyUSB, 再装 SJ** */
} usb_mode_t;

static usb_mode_t s_mode = USB_MODE_NORMAL;

/* 是否处于"TinyUSB 在跑"的四个正常模式 */
static inline bool mode_is_tinyusb(usb_mode_t m)
{
    return m == USB_MODE_HID || m == USB_MODE_MSC || m == USB_MODE_BTH || m == USB_MODE_NET;
}

/* 切换期间日志接管: 只丢弃输出, 不写任何串口 (切换窗口几 ms, 丢失可忽略) */
static int usb_log_discard(const char *fmt, va_list arg)
{
    (void)fmt; (void)arg;
    return 0;
}

/* ═══ 把 Serial/JTAG 驱动(=USB PHY pad) 装回来 —— 幂等、可重试 ═══
 * 为什么必须靠 install 才能恢复: tinyusb_driver_uninstall() 内部走 usb_del_phy(),
 *   而 usb_del_phy() 对内部 PHY 做的是 "Clear pullup and pulldown loads on D+ / D-,
 *   and disable the pads" (esp-idf components/usb/usb_phy.c:463-466) —— 也就是把 D+/D- 上拉
 *   清掉、pad 关掉。唯一能重新打开的是 usb_serial_jtag_driver_install() 里的
 *   usb_serial_jtag_ll_phy_enable_pad(true) (esp_driver_usb_serial_jtag/usb_serial_jtag.c:213-216)。
 *   ⇒ install 失败 = pad 仍关着 = 主机枚举不到设备(而设备本身一切正常)。
 * 幂等: IDF 对"已安装"会返回 ESP_ERR_INVALID_STATE 并自己打一条 E 日志(看起来像真故障),
 *   所以先用官方查询 usb_serial_jtag_is_driver_installed() 短路。
 * ⚠ is_driver_installed() 只说明"驱动对象在不在", **不能**代表 PHY 状态:
 *   开机默认态也是"未安装"但 PHY 正常(ROM 配置, console 走 ROM)。所以它只用于幂等,
 *   不能拿来判断"串口是否健康"—— 健康与否由 s_mode 这段操作历史表达。 */
static bool usj_restore(void)
{
    if (usb_serial_jtag_is_driver_installed()) {
        ESP_LOGI(TAG, "Serial/JTAG 驱动已在位, 跳过重装(幂等)");
        return true;
    }
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();  /* tx/rx 各 256B */
    esp_err_t r = usb_serial_jtag_driver_install(&cfg);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "Serial/JTAG 重装失败(%s) ⇒ PHY pad 仍关闭, 主机枚举不到串口",
                 esp_err_to_name(r));
        return false;
    }
    ESP_LOGI(TAG, "Serial/JTAG 已恢复 (PHY pad 已开, 主机将重新枚举)");
    return true;
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
        ESP_LOGD(TAG, "卸载 Serial/JTAG 失败: %s", esp_err_to_name(sj_ret));
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
        /* 失败: 装回 Serial/JTAG 驱动, 恢复原控制台, 避免串口永久失效。
         * ★ 2026-10-29: 回滚也必须看返回值 —— 原来这里直接丢掉结果, 回滚失败也没人知道。
         *   TinyUSB 的 install 失败路径内部会 goto del_phy 调 usb_del_phy() 清理 PHY
         *   (esp_tinyusb/tinyusb.c:227-232), 所以此刻 PHY 是干净的 ⇒ 归 RECOVER_FAIL
         *   (重试动作 = 只装 SJ), 不是 TINYUSB_STUCK。 */
        bool ok = usj_restore();
        esp_log_set_vprintf(prev);
        if (!ok) {
            s_mode = USB_MODE_RECOVER_FAIL;
            ESP_LOGE(TAG, "进入 TinyUSB 失败(%s) 且串口回滚也失败 ⇒ RECOVER_FAIL, 下次退出会重试",
                     esp_err_to_name(ret));
        } else {
            ESP_LOGE(TAG, "进入 TinyUSB 失败(%s), 串口已回滚到正常模式", esp_err_to_name(ret));
        }
        return ret;
    }
    esp_log_set_vprintf(prev);
    ESP_LOGI(TAG, "已进入 USB 模式, Serial/JTAG 已让出 PHY");
    return ESP_OK;
}

/* 退出 TinyUSB 模式: 卸 TinyUSB → 装回 Serial/JTAG 驱动 (恢复串口控制台).
 * ★★ 2026-10-29 重写(核心修复): 原来返回 void, 且无论成败都无条件 s_mode = NORMAL。
 *    而本函数第一句就是 "if (s_mode == NORMAL) return" ⇒ 一旦恢复失败, 状态被写成 NORMAL,
 *    之后任何调用都在第一句早退, 永远不再重试, 只能断电重启。
 *    现在: 按状态分支 + 失败保留失败态 + 每一步都查返回值并打错误码。 */
static esp_err_t usb_mode_exit(void)
{
    if (s_mode == USB_MODE_NORMAL) return ESP_OK;   /* 正常态: 没有 TinyUSB 要卸 */

    usb_mode_t from = s_mode;
    vprintf_like_t prev = esp_log_set_vprintf(&usb_log_discard);

    /* ① 只在"TinyUSB 可能还在"的态才卸。
     *    RECOVER_FAIL 表示上次已确认卸净, 再卸一次就是重复卸载。 */
    if (mode_is_tinyusb(from) || from == USB_MODE_TINYUSB_STUCK) {
        esp_err_t ur = tinyusb_driver_uninstall();
        /* ESP_ERR_INVALID_STATE = 任务上下文不存在(即根本没装)。按要求**不能直接当成功**:
         *   必须再用 TinyUSB 官方栈状态 tusb_inited() 证明"栈确实没初始化", 才允许继续装 SJ;
         *   否则一律算"卸载状态不确定" → STUCK, 绝不在 PHY 归属不明时去抢 pad。 */
        bool proven_gone = (ur == ESP_OK) ||
                           (ur == ESP_ERR_INVALID_STATE && !tusb_inited());
        if (!proven_gone) {
            esp_log_set_vprintf(prev);
            s_mode = USB_MODE_TINYUSB_STUCK;
            ESP_LOGE(TAG, "tinyusb 卸载失败(%s, tusb_inited=%d) ⇒ PHY 可能仍归 OTG, "
                          "状态=TINYUSB_STUCK(下次先重试卸载, 不抢 PHY)",
                     esp_err_to_name(ur), (int)tusb_inited());
            return ur;
        }
        if (ur != ESP_OK)   /* 只有 INVALID_STATE + 栈确实未初始化这一种情况会走到这 */
            ESP_LOGW(TAG, "tinyusb 卸载返回 %s, 但 tusb_inited()==false 已证明栈未初始化, 继续",
                     esp_err_to_name(ur));
    }

    /* ② 装回 Serial/JTAG (= 打开 PHY pad, 主机才可能重新枚举) */
    bool ok = usj_restore();
    esp_log_set_vprintf(prev);

    if (!ok) {
        s_mode = USB_MODE_RECOVER_FAIL;      /* ← 关键: 绝不置 NORMAL */
        ESP_LOGE(TAG, "退出模式(%d)失败: 串口未恢复, 状态=RECOVER_FAIL(下次可重试)", (int)from);
        return ESP_FAIL;
    }
    s_mode = USB_MODE_NORMAL;
    ESP_LOGI(TAG, "已退出模式(%d), Serial/JTAG 已恢复", (int)from);
    return ESP_OK;
}

/* 进入任一新模式前的统一前置: 保证上一模式已退干净、PHY 归属明确。
 * ★ 2026-10-29: 原来 4 个 enter 都是 "if (s_mode != NORMAL) usb_mode_exit();" 不看结果,
 *   带着"PHY 可能仍归 OTG"的状态去装 TinyUSB 会两边抢 pad。
 *   这里失败时**当场重试一次** usj_restore(), 让"用户点一次"最多尝试两次,
 *   不必等用户下次再点(那样重试入口太被动)。返回非 OK 时调用方必须放弃本次进入。 */
static esp_err_t mode_prepare_enter(void)
{
    if (s_mode == USB_MODE_NORMAL) return ESP_OK;

    esp_err_t er = usb_mode_exit();
    if (er == ESP_OK) return ESP_OK;

    /* exit 失败必须按"卡在哪一步"分开处理, 不能一律补装 SJ:
     *   TINYUSB_STUCK —— TinyUSB 还占着 PHY, 此时装 SJ 只会两边抢 pad
     *                    ⇒ 只能**再试一次卸载**; 还不行就拒绝进入。
     *   RECOVER_FAIL  —— TinyUSB 已卸净, 只差 install ⇒ 补装一次即可。 */
    if (s_mode == USB_MODE_TINYUSB_STUCK) {
        esp_err_t er2 = usb_mode_exit();      /* 再试一次"卸载 + 装回" */
        if (er2 == ESP_OK) {
            ESP_LOGW(TAG, "TinyUSB 卸载首次失败(%s), 重试已成功", esp_err_to_name(er));
            return ESP_OK;
        }
        ESP_LOGE(TAG, "TinyUSB 仍无法卸载(%s) ⇒ 拒绝进入新模式(不在 PHY 归属不明时抢 pad)",
                 esp_err_to_name(er2));
        return er2;
    }

    if (usj_restore()) {                       /* RECOVER_FAIL: 当场补装一次 */
        s_mode = USB_MODE_NORMAL;
        ESP_LOGW(TAG, "上一模式恢复首次失败(%s), 重试已成功, 继续进入", esp_err_to_name(er));
        return ESP_OK;
    }
    s_mode = USB_MODE_RECOVER_FAIL;
    ESP_LOGE(TAG, "上一模式恢复失败(%s) 且重试无效, 拒绝进入新模式", esp_err_to_name(er));
    return er;
}

/* 切换到键鼠 (HID+CDC) 复合设备 */
esp_err_t usb_composite_enter_hid(void)
{
    if (s_mode == USB_MODE_HID) return ESP_OK;
    esp_err_t pe = mode_prepare_enter();               /* 防御: 先回正常模式(失败即拒绝进入) */
    if (pe != ESP_OK) return pe;
    esp_err_t ret = usb_mode_enter(hid_config_desc, hid_string_desc_arr, 4);
    if (ret == ESP_OK) s_mode = USB_MODE_HID;
    return ret;
}

/* 切换到挂载 (纯 MSC U盘) 设备 */
esp_err_t usb_composite_enter_msc(void)
{
    if (s_mode == USB_MODE_MSC) return ESP_OK;
    esp_err_t pe = mode_prepare_enter();               /* 防御: 先回正常模式(失败即拒绝进入) */
    if (pe != ESP_OK) return pe;
    esp_err_t ret = usb_mode_enter(msc_config_desc, msc_string_desc_arr, 4);
    if (ret == ESP_OK) s_mode = USB_MODE_MSC;
    return ret;
}

/* 切换到 USB-BT (蓝牙适配器 HCI) 设备 */
esp_err_t usb_composite_enter_bth(void)
{
    if (s_mode == USB_MODE_BTH) return ESP_OK;
    esp_err_t pe = mode_prepare_enter();               /* 防御: 先回正常模式(失败即拒绝进入) */
    if (pe != ESP_OK) return pe;
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
    esp_err_t pe = mode_prepare_enter();               /* 防御: 先回正常模式(失败即拒绝进入) */
    if (pe != ESP_OK) return pe;
    esp_err_t ret = usb_mode_enter(net_config_desc, net_string_desc_arr, 4);
    if (ret == ESP_OK) s_mode = USB_MODE_NET;
    return ret;
}

/* 退出任意 TinyUSB 模式, 回到正常 (Serial/JTAG 控制台).
 * ★ 2026-10-29: 由 void 改为返回 esp_err_t。失败时调用方(尤其 HID/MSC 的 stop)必须如实上报,
 *   不能再无条件打"已恢复控制台"; USB-BT / USB-NET 的退出路径紧接着 esp_restart(),
 *   可以忽略返回值(重启比任何软件重试都彻底)。 */
esp_err_t usb_composite_exit(void)
{
    return usb_mode_exit();
}

/* 当前是否处于某个 TinyUSB 模式且已被主机枚举 */
bool usb_composite_is_connected(void)
{
    /* ★ 2026-10-29: 用 mode_is_tinyusb 而不是 != NORMAL —— 失败态(RECOVER_FAIL/STUCK)下
     *   TinyUSB 栈已卸载, 调 tud_mounted() 没有意义且不安全。 */
    return mode_is_tinyusb(s_mode) && tud_mounted();
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
        ESP_LOGD(TAG, "键鼠模式启动失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

void usb_hid_stop(void)
{
    if (!s_running) return;
    usb_hid_key_release();
    usb_hid_mouse_release(MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT | MOUSE_BUTTON_MIDDLE);
    s_running = false;      /* 先置 false: 万一下面恢复失败, 下次 start 仍会走 enter → 触发重试 */
    /* ★ 2026-10-29: 恢复失败时不能再说"已恢复控制台"(那是谎报) —— 如实报错, 并说明怎么重试 */
    esp_err_t r = usb_composite_exit();
    if (r == ESP_OK)
        ESP_LOGI(TAG, "键鼠模式已退出, 已恢复 Serial/JTAG 控制台");
    else
        ESP_LOGE(TAG, "键鼠模式已退出, 但串口恢复失败(%s) ⇒ 状态已保留可重试, 请重启或重开一次键鼠",
                 esp_err_to_name(r));
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
    /* ★ 2026-10-30: 抬起**必须无条件发一次** —— 原来这里先 `if (!kb_ready()) return;`,
     *   于是"刚按下、主机就掉线/接口变忙"这一瞬间会直接返回, 抬起报文永远不发。
     *   主机重新枚举后仍认为那个键是按下的 (按住 Shift/Enter 卡死就是这么来的)。
     *   tud_hid_n_keyboard_report() 在未就绪时自己会返回 false, 不会误发, 所以无需再判。 */
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
    if (!mode_is_tinyusb(s_mode) || !tud_cdc_connected()) return 0;   /* 失败态不碰 TinyUSB API */
    size_t n = tud_cdc_write(data, len);
    tud_cdc_write_flush();
    return n;
}

/* CDC 串口是否已连接 (PC 打开该串口后 DTR 置位) */
/* ★ 2026-10-02: USB 是否被**主机枚举**(挂载) —— 不区分当前复合模式(HID/CDC/MSC...)。
 * 插到电脑等**数据主机**上为真; 插纯充电器(不枚举)为假。
 * 用途: "USB 连接时不要自动休眠/自动关机"(linit)。见 wallpapers.c 的独立监视任务。
 * ⚠ 当前板把 CHRG/VBUS 留在模块里没接给 MCU(见 docs/硬件与IO.md), 所以**纯充电器检测不了**。 */
bool usb_hid_host_mounted(void)
{
    /* linit 明确: 不做充电检测, 只按 **USB/串口连接** 判。
     * tud_mounted()      = 主机已枚举我们(插到电脑上就算, 不要求打开串口)
     * tud_cdc_connected()= 串口已被主机打开(在跑 monitor / 传文件)
     * 两者任一为真即视为"USB 连着"。纯充电器不枚举 → 仍检测不到(板子没把 CHRG/VBUS 接给 MCU)。 */
    return tud_mounted() || tud_cdc_connected();
}

bool usb_hid_cdc_connected(void)
{
    return mode_is_tinyusb(s_mode) && tud_cdc_connected();
}

/* ---------------- CDC 接收 (信息回传: 目标电脑经串口发日志回设备) ----------------
 * 非回调实现: 调用方 (日志诊断页 poll) 周期调用 usb_hid_cdc_read 拉取 TinyUSB 接收
 * FIFO, 不依赖 tud_cdc_rx_cb (esp_tinyusb v2 内部强定义). */

/* 读取 PC 经 CDC 串口发来的数据 (回传). 返回实际读取字节数; 无数据/未连接返回 0. */
size_t usb_hid_cdc_read(uint8_t *data, size_t len)
{
    if (!data || len == 0 || !mode_is_tinyusb(s_mode) || !tud_cdc_connected()) return 0;
    size_t avail = tud_cdc_available();
    if (avail == 0) return 0;
    if (avail > len) avail = len;
    return tud_cdc_read(data, avail);
}
