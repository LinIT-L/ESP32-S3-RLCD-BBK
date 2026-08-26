/**
 * @file usb_hid.h
 * @brief USB HID 复合设备 (键盘 + 鼠标), 供 BBK 屏当"外接键鼠"用.
 *
 * 与 usbh_msc 相同的外设切换模式:
 *   - usb_hid_start(): 卸载 USB Serial/JTAG (释放 GPIO 19/20), 安装 TinyUSB,
 *     向电脑呈现一个复合 HID 设备 = 键盘接口 (itf 0) + 鼠标接口 (itf 1).
 *   - usb_hid_stop(): 卸载 TinyUSB, 恢复 USB Serial/JTAG (回到串口日志).
 *
 * 调用方 (菜单) 负责:
 *   - 上半屏键盘: 触摸按键 -> usb_hid_key_tap()
 *   - 下半屏触控板: 滑动 -> usb_hid_mouse_move(), 点击 -> usb_hid_mouse_click()
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 USB HID (键盘+鼠标复合设备). 返回 ESP_OK 表示 TinyUSB 已就绪. */
esp_err_t usb_hid_start(void);

/* 停止 USB HID, 恢复 USB Serial/JTAG 串口. */
void usb_hid_stop(void);

/* 当前是否处于 HID 模式 */
bool usb_hid_is_running(void);

/* 电脑是否已枚举并连接 (TinyUSB mount 就绪) */
bool usb_hid_connected(void);

/* === 键盘 === */

/* 发送一次按键 (按下+释放). modifier 为 HID 修饰键位掩码 (如 HID_KEY_NONE), 
 * keycode 为 HID 键盘码 (如 HID_KEY_A). 见 tusb_hid_key_map / hid.h. */
void usb_hid_key_tap(uint8_t modifier, uint8_t keycode);

/* 按住指定键盘码 (可同时多键, keycodes 数组, n 个). 需手动调 usb_hid_key_release(). */
void usb_hid_key_press(uint8_t modifier, const uint8_t *keycodes, uint8_t n);

/* 释放所有已按键盘键 */
void usb_hid_key_release(void);

/* === 鼠标 (触控板模式) === */

/* 相对移动: dx/dy 为 -127..127 像素 */
void usb_hid_mouse_move(int8_t dx, int8_t dy);

/* 点击一次 (按下+释放). 如 usb_hid_mouse_click(MOUSE_BUTTON_LEFT). */
void usb_hid_mouse_click(uint8_t buttons);

/* 按住/释放鼠标键 (可组合 MOUSE_BUTTON_LEFT|RIGHT) */
void usb_hid_mouse_press(uint8_t buttons);
void usb_hid_mouse_release(uint8_t buttons);

#ifdef __cplusplus
}
#endif