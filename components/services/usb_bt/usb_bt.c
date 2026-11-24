/**
 * @file usb_bt.c
 * @brief USB 蓝牙适配器 (BTH) 模块.
 *
 * 使 ESP32-S3 模拟成 USB 蓝牙 HCI 适配器 (参考 esp-iot-solution usb_dongle).
 *
 * 方案: "开机进入 + 重启切换 + 二次重启退出" (最稳, 对齐官方 usb_dongle):
 *   - 启动: 提示"将重启进入蓝牙模式" -> 写 NVS 标记 -> esp_restart()
 *   - 重启后: os_boot 检测到标记 -> 开机早期初始化 BTH (独占蓝牙控制器, 不初始化手柄蓝牙)
 *   - 关闭: 提示"将重启恢复" -> 清标记 -> esp_restart() 回正常串口模式
 *   - 标记保持 true 期间 = USB-BT 运行中 (状态栏显示图标)
 *
 * 不用热切换: ESP32-S3 运行中切换 USB-Serial/JTAG <-> OTG 会导致 USB 枚举失败(设备消失),
 * 官方 usb_dongle 也是在开机早期就初始化 BTH, 不运行中切换.
 */
#include "usb_bt.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"

#include "svc_nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_hid.h"

/* usb_bt_bth.c 提供的 HCI 桥初始化: 开机早期初始化纯蓝牙控制器 + vhci 桥 */
extern void tusb_bth_init(void);

#define TAG "USB_BT"
#define USBBT_NS   "usb_bt"
#define USBBT_KEY  "active"

/* 当前是否处于 USB-BT 模式 (由 NVS 标记决定, 进入后保持 true 直到退出) */
bool usb_bt_is_active(void)
{
    nvs_handle_t h;
    if (svc_nvs_open(USBBT_NS, NVS_READONLY, &h) != ESP_OK) return false;
    int8_t v = 0;
    esp_err_t e = svc_nvs_get_i8(h, USBBT_KEY, &v);
    svc_nvs_close(h);
    return (e == ESP_OK) && (v != 0);
}

/* ★ 2026-11-22: 标记写入改为"带返回值的落盘", 供 activate/deactivate 判断能否安全重启。
 *   原来 commit 返回值被丢掉就直接 esp_restart() —— 标记没写进去时重启后仍是普通模式,
 *   用户看到的就是"点了进入蓝牙模式, 重启完还是老样子", 且没有任何错误提示。 */
static esp_err_t usb_bt_store_active(bool active)
{
    nvs_handle_t h;
    if (svc_nvs_open(USBBT_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_ERR_NVS_NOT_INITIALIZED;
    esp_err_t e = active ? svc_nvs_set_i8(h, USBBT_KEY, 1)
                         : svc_nvs_erase_key(h, USBBT_KEY);
    if (e == ESP_OK) e = svc_nvs_commit(h);
    svc_nvs_close(h);
    return e;
}

void usb_bt_set_active(bool active)
{
    (void)usb_bt_store_active(active);
}

/* 启动: 写标记 + 重启 (重启后 os_boot 进 BTH). 调用方负责先提示"将重启". */
esp_err_t usb_bt_activate(void)
{
    esp_err_t e = usb_bt_store_active(true);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "进入蓝牙模式的标记写入失败(%s) ⇒ 不重启, 否则重启后仍在普通模式",
                 esp_err_to_name(e));
        return e;
    }
    vTaskDelay(pdMS_TO_TICKS(600));   /* 0.5s 提示 (正在重启) 可见后再重启 */
    esp_restart();
    return ESP_OK;   /* 不返回 */
}

/* 退出: 清标记, 先卸 TinyUSB(停止 BTH) 并装回 USB Serial/JTAG 驱动 —— 与"卸载TF卡后串口恢复"同一机制,
 * 让主机重新枚举出串口; 再延迟重启完成完整复位. 调用方负责先提示"将重启恢复". */
void usb_bt_deactivate(void)
{
    /* ★ 2026-11-22: 清标记失败必须重试 —— 标记没清掉时重启会**又回到 BTH 模式**,
     *   而此时代 USB 复合设备已经被卸载, 用户看到的是"退出蓝牙后重启, 还是蓝牙模式,
     *   而且串口也没了"。重试 3 次仍失败只能重启(没有别的退路), 但留下明确日志。 */
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        e = usb_bt_store_active(false);
        if (e == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (e != ESP_OK)
        ESP_LOGE(TAG, "清蓝牙标记失败(%s): 重启后可能仍进 BTH 模式", esp_err_to_name(e));
    /* 先强制 USB 断开让主机感知设备脱离 → 重启后重新枚举出串口.
     * (macOS 在运行时 TinyUSB↔串口切换后常不自动重枚举, 不强制断开则串口不恢复) */
    if (tud_mounted()) tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    usb_composite_exit();            /* 卸 BTH TinyUSB + 装回 USB-Serial/JTAG → 串口恢复(同卸载TF) */
    vTaskDelay(pdMS_TO_TICKS(600));  /* 0.5s 提示 (正在重启) 可见后再重启 */
    esp_restart();   /* 不返回 */
}

/* 当前是否视为"运行中"(= NVS 标记): 供状态栏/UI 判断 */
bool usb_bt_is_running(void) { return usb_bt_is_active(); }

/* 系统已处于 USB-BT 激活时调用 (由 os_boot 在启动早期检测标记).
 * 开机早期初始化 BTH: 独占蓝牙控制器 (此时手柄蓝牙未初始化), 不重启.
 * 保持标记 true: 本次 USB-BT 运行期间状态栏显示图标, 用户点退出才清标记重启. */
esp_err_t usb_bt_start(void)
{
    esp_err_t ret = usb_composite_enter_bth();
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "进入 USB-BT 设备失败: %s, 清标记回串口模式", esp_err_to_name(ret));
        usb_bt_set_active(false);
        return ret;
    }
    tusb_bth_init();
    ESP_LOGI(TAG, "USB-BT 就绪: 电脑应识别到蓝牙适配器 (BTH 运行中)");
    return ESP_OK;
}