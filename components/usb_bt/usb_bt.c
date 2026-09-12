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
#include "nvs_flash.h"
#include "nvs.h"
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
    if (nvs_open(USBBT_NS, NVS_READONLY, &h) != ESP_OK) return false;
    int8_t v = 0;
    esp_err_t e = nvs_get_i8(h, USBBT_KEY, &v);
    nvs_close(h);
    return (e == ESP_OK) && (v != 0);
}

void usb_bt_set_active(bool active)
{
    nvs_handle_t h;
    if (nvs_open(USBBT_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (active) {
        nvs_set_i8(h, USBBT_KEY, 1);
    } else {
        nvs_erase_key(h, USBBT_KEY);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* 启动: 写标记 + 重启 (重启后 os_boot 进 BTH). 调用方负责先提示"将重启". */
esp_err_t usb_bt_activate(void)
{
    usb_bt_set_active(true);
    vTaskDelay(pdMS_TO_TICKS(600));   /* 0.5s 提示 (正在重启) 可见后再重启 */
    esp_restart();
    return ESP_OK;   /* 不返回 */
}

/* 退出: 清标记, 先卸 TinyUSB(停止 BTH) 并装回 USB Serial/JTAG 驱动 —— 与"卸载TF卡后串口恢复"同一机制,
 * 让主机重新枚举出串口; 再延迟重启完成完整复位. 调用方负责先提示"将重启恢复". */
void usb_bt_deactivate(void)
{
    usb_bt_set_active(false);
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
        ESP_LOGE(TAG, "进入 USB-BT 设备失败: %s, 清标记回串口模式", esp_err_to_name(ret));
        usb_bt_set_active(false);
        return ret;
    }
    tusb_bth_init();
    ESP_LOGI(TAG, "USB-BT 就绪: 电脑应识别到蓝牙适配器 (BTH 运行中)");
    return ESP_OK;
}