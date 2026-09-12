/**
 * @file usb_hid.h
 * @brief USB 复合设备"按需切换" (esp_tinyusb v2).
 *
 *   - 正常模式 (开机默认): USB Serial/JTAG 串口控制台, 不装 TinyUSB.
 *   - 键鼠模式: HID 键盘 + 鼠标 + CDC 复合设备 (控制台走 CDC).
 *   - 挂载模式: MSC + CDC 复合设备 (TF 卡 U盘 + 串口, 控制台走 CDC).
 *   切换全程无日志写向已失效的 Serial/JTAG, 挂载 TF 卡到电脑不再崩溃重启.
 *
 * 调用方 (菜单) 负责:
 *   - 上半屏键盘: 触摸按键 -> usb_hid_key_tap()
 *   - 下半屏触控板: 滑动 -> usb_hid_mouse_move(), 点击 -> usb_hid_mouse_click()
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 切换到 MSC+CDC (U盘) 复合设备, 控制台重定向到 CDC. 供挂载流程调用. */
esp_err_t usb_composite_enter_msc(void);

/* 切换到 USB-BT (蓝牙适配器 HCI) 设备. 供 USB-BT 流程调用 (需先用 NVS 标记抑制手柄蓝牙). */
esp_err_t usb_composite_enter_bth(void);

/* 切换到 USB-RNDIS 网卡设备 (USB 有线网卡). 供 USB 网卡共享流程调用. */
esp_err_t usb_composite_enter_net(void);

/* 切换到 HID 键鼠 + CDC 复合设备, 控制台重定向到 CDC. 供键鼠流程调用. */
esp_err_t usb_composite_enter_hid(void);

/* 退出任意 TinyUSB 模式, 回到正常模式 (恢复 Serial/JTAG 串口控制台). */
void usb_composite_exit(void);

/* 当前 USB 设备是否已枚举 (处于 TinyUSB 模式且被主机枚举) */
bool usb_composite_is_connected(void);

/* 启动键鼠模式 (切换到 HID 键盘+鼠标+CDC). 返回 ESP_OK 表示已就绪. */
esp_err_t usb_hid_start(void);

/* 停止键鼠模式, 切回正常模式 (恢复 Serial/JTAG 控制台). */
void usb_hid_stop(void);

/* 当前是否处于键鼠模式 */
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

/* === CDC 串口 (复合设备附加) === */

/* 发送数据到 PC CDC 串口. 返回实际写入字节数; 未运行/未连接返回 0. */
size_t usb_hid_cdc_write(const uint8_t *data, size_t len);

/* CDC 串口是否已连接 (PC 打开该串口后) */
bool usb_hid_cdc_connected(void);

/* 读取 PC 经 CDC 串口发来的数据 (信息回传). 返回实际读取字节数; 无数据/未连接返回 0. */
size_t usb_hid_cdc_read(uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif