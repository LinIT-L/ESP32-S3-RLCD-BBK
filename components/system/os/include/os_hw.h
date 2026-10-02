/**
 * os_hw.h — 硬件服务懒加载调度器.
 *
 * 统一管理系统级共享硬件 (蓝牙/WiFi/USB 外设) 的按需启停:
 *   - 默认关闭 (USB 串口 Serial/JTAG 由 ESP-IDF 默认开启, 属例外)
 *   - 应用 os_hw_request() 启动服务 (引用计数), 释放归零自动关闭
 *   - 同服务同一时刻一个状态, 避免冲突
 * 应用拿到服务后正常调用对应模块 API (bt_ctl / wifi_manager / usb_hid).
 */
#ifndef OS_HW_H
#define OS_HW_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OS_HW_BT = 0,     /* 蓝牙 (bt_ctl) */
    OS_HW_WIFI,       /* Wi-Fi (wifi_manager) */
    OS_HW_USB_HID,    /* USB HID 键鼠 (usb_hid; 会切走 USB 串口) */
    OS_HW_COUNT
} os_hw_id_t;

/* 请求服务: 引用计数++; 首次请求启动底层模块. 返回 false=启动失败(需释放). */
bool os_hw_request(os_hw_id_t id);
/* 释放服务: 引用计数--; 归零时停止底层模块. */
void os_hw_release(os_hw_id_t id);
/* 服务当前是否处于启动(活跃)状态 */
bool os_hw_is_active(os_hw_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* OS_HW_H */
