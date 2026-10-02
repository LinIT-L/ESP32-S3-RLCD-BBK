/**
 * os_hid.h — 系统 USB HID 键鼠 API (键盘/鼠标/CDC 串口).
 *
 * 对 usb_hid 子系统的系统级封装: 进入即向管家申请 USB-HID 硬件
 * (os_hw_request(OS_HW_USB_HID)), 退出即释放 (os_hw_release),
 * 与蓝牙/WiFi 共用同一套按需启停框架. 应用只调 os_hid_*, 不直接
 * include usb_hid.h, 也不直接 start/stop, 由系统统一调度硬件生命周期.
 *
 * 这与 os_wifi 对称: 页面不再手动调 os_hw_request/release, 硬件状态
 * 由管家引用计数统一管理, 避免多应用抢 USB 复合设备状态.
 */
#ifndef OS_HID_H
#define OS_HID_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动键鼠模式 (请求硬件 + 切换 HID 键盘+鼠标+CDC). 返回 ESP_OK 表示就绪. */
esp_err_t os_hid_start(void);
/* 停止键鼠模式 (切回正常模式 + 释放硬件). */
void     os_hid_stop(void);
/* 当前是否处于键鼠模式 */
bool     os_hid_is_running(void);
/* 电脑是否已枚举并连接 (TinyUSB mount 就绪) */
bool     os_hid_connected(void);

/* === 键盘 === */
/* 发送一次按键 (按下+释放). modifier=修饰键掩码, keycode=HID 键盘码. */
void os_hid_key_tap(uint8_t modifier, uint8_t keycode);
/* 释放所有已按键盘键 */
void os_hid_key_release(void);

/* === 鼠标 (触控板) === */
/* 相对移动: dx/dy 为 -127..127 像素 */
void os_hid_mouse_move(int8_t dx, int8_t dy);
/* 点击一次 (按下+释放) */
void os_hid_mouse_click(uint8_t buttons);
/* 按住/释放鼠标键 (可组合 L|R|M) */
void os_hid_mouse_release(uint8_t buttons);

/* === CDC 串口 (复合设备附加) === */
/* CDC 串口是否已连接 (PC 打开该串口后) */
bool     os_hid_cdc_connected(void);
/* 读取 PC 经 CDC 串口发来的数据. 返回实际读取字节数; 无数据/未连接返回 0. */
size_t   os_hid_cdc_read(uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* OS_HID_H */
