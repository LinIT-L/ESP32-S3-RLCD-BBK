/**
 * @file usb_bt.h
 * @brief USB 蓝牙适配器 (BTH) 对外接口.
 *
 * 方案: NVS 标记 + 重启. 启动写标记重启进入 USB-BT; 关闭清标记重启回串口.
 * os_boot 检测标记后在开机早期初始化 BTH (独占蓝牙控制器, 跳过手柄蓝牙).
 */
#ifndef USB_BT_H
#define USB_BT_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB-BT 是否激活 (NVS 标记): true = 开机应进 BTH 模式(独占蓝牙, 跳过手柄蓝牙) */
bool usb_bt_is_active(void);
void usb_bt_set_active(bool active);

/* 启动 (写标记 + 重启进入 USB-BT). 调用方先提示"将重启". 由 UI 点"启动/后台"调用. */
esp_err_t usb_bt_activate(void);
/* 退出 (清标记 + 重启回正常串口模式). 调用方先提示"将重启恢复". 由 UI 退出弹窗调用. */
void usb_bt_deactivate(void);

/* 当前是否处于运行状态 (= NVS 标记, 供状态栏/UI 判断) */
bool usb_bt_is_running(void);

/* os_boot 在启动早期检测到标记后调用: 初始化 BTH 设备 + 纯蓝牙控制器 HCI 桥. */
esp_err_t usb_bt_start(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_BT_H */