#ifndef WALLPAPERS_SLEEP_H
#define WALLPAPERS_SLEEP_H

#include <stdbool.h>
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 电源模块 — 关机 = ESP-IDF 官方 deep sleep
 *
 * 参考: esp-idf/examples/system/deep_sleep
 *   - deep_sleep_example_main.c : 关机 = esp_deep_sleep_start(); 用 RTC 内存跨睡眠存数据
 *   - ext_wakeup.c              : 唤醒源 = gpio_config() + esp_sleep_enable_ext1_wakeup_io()
 *
 * ESP32-S3 无 SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP, 深睡 GPIO 唤醒只有 EXT1 一条路。
 *
 * 两条关机路径, 唤醒条件不同:
 *   MANUAL 手动(长按关机键 → 确认框) : 只认 PWR 键, 且必须**保持按住 ≥0.5s** 才开机
 *                                      (EXT1 是电平触发, 0.5s 保持由软件层判定, 挡误触)
 *   AUTO   自动(休眠时间到点)        : 认**任意键**(PWR/KEY/BOOT), 短按即开机
 * ==========================================================================*/

#define WP_OFF_MANUAL  0   /* 手动关机: 仅 PWR 键唤醒 + 须按住 0.5s */
#define WP_OFF_AUTO    1   /* 自动关机(休眠到点): 任意键唤醒 */

/* 开机调用一次: 绑定 LCD(关机清屏用) + 注册深睡唤醒源。 */
void wp_power_init(st7305_handle_t *lcd);

/* 按键引脚初始化 + 注册深睡唤醒源(由 input_init 调用一次)。 */
void wp_power_pin_init(void);

/* 关机(唯一入口, 正常不返回): 停射频 → 清屏 → 按来源设唤醒掩码 → 深睡。
 * origin 取 WP_OFF_MANUAL / WP_OFF_AUTO。 */
void wp_power_off(int origin);

/* 开机**最早期**调用(早于一切初始化, app_main 首行):
 * 上次是 MANUAL 关机唤醒时, 要求 PWR 键仍被按住且满 0.5s 才放行;
 * 不足 0.5s(碰一下) → 判为误触, 直接回深睡, 不返回。 */
void wp_power_wake_gate(void);

/* 本次启动是否由"关机深睡"唤醒(EXT1)。 */
bool wp_power_woke_from_off(void);

/* 本次唤醒的关机来源(WP_OFF_MANUAL / WP_OFF_AUTO); 非关机唤醒返回 -1。 */
int wp_power_wake_origin(void);

/* 开机打印复位/唤醒原因(排障用)。 */
void wp_sleep_boot_diag(void);

#ifdef __cplusplus
}
#endif

#endif /* WALLPAPERS_SLEEP_H */
