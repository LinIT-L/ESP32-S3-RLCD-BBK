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
#include <stdint.h>

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
/* 底层模块的**真实运行态** (不只看调度器计数, 用于判断"用户自己开着的"设备) */
bool os_hw_is_real_on(os_hw_id_t id);

/* ==== 临时占用 (linit 2026-10-14, 全系统统一口径) ====
 * 应用进入时**强制打开**网卡/蓝牙, 退出时按**进入前的状态**还原:
 *   原本就开 → 保持开 (不参与引用计数, 退出时不动它);
 *   原本关闭 → 由本次占用 request 起来, 退出时 release 关掉。
 * 用法: 页面/服务持有一个静态 os_hw_lease_t, on_enter 逐个 acquire, on_exit 逐个 release。
 * acquire 幂等 (同一 lease 重复 acquire 不会重复计数)。⛔ 不要再用裸 request/release 配对。 */
typedef struct { uint8_t held; } os_hw_lease_t;   /* 位图: bit(i) = 本次由我们持有 */
void os_hw_lease_acquire(os_hw_lease_t *l, os_hw_id_t id);
void os_hw_lease_release(os_hw_lease_t *l, os_hw_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* OS_HW_H */
