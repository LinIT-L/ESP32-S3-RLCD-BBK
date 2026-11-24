#ifndef POWER_MGR_H
#define POWER_MGR_H

#include <stdbool.h>
#include <stdint.h>

#include "st7305.h"   /* st7305_handle_t: 关屏(sleep_in)与 deinit 需要面板句柄 */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * power_mgr — 系统电源管理 (深睡/关机的唯一实现)
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
 *
 * ★ 2026-10-28 从 components/ui/wallpapers/sleep.c 迁出 (第 5 阶段 Step 3):
 *   原来这套"关射频 → 关音频 → GPS 停靠 → 卸 SD → 关触摸 → 关屏 → 隔离引脚 →
 *   配电源域 → 深睡"的编排全在 UI 层, 连 system/input 都要 extern 它取按键初始化。
 *   现在硬件收尾归本组件; UI 只负责"刷白 + 提出关机请求"(见 wallpapers/sleep.c)。
 * ==========================================================================*/

#define POWER_OFF_MANUAL  0   /* 手动关机: 仅 PWR 键唤醒 + 须按住 0.5s */
#define POWER_OFF_AUTO    1   /* 自动关机(休眠到点): 任意键唤醒 */

/* 开机调用一次: 绑定面板句柄(关屏/deinit 用) + 按键引脚配置 + 注册深睡唤醒源 (幂等)。 */
void power_mgr_init(st7305_handle_t *lcd);

/* 同 power_mgr_init(), 保留独立入口给 input_init (只看按键唤醒源, 语义更明确)。 */
void power_mgr_pin_init(void);

/* ★ 关机(唯一入口, 正常不返回)。
 *   顺序与本组件迁出前完全一致: 停射频 → 关音频(early) → GPS 停靠 → 关音频(hold)
 *   → 卸 SD/关触摸/关屏 → 等键松开 → 设唤醒掩码并 hold → 电源域 → 降频
 *   → 屏 deinit → 二次卸 SD/触摸进复位 → SD 引脚 hold → 隔离未用脚 → 诊断汇总 → 深睡。
 *   origin 取 POWER_OFF_MANUAL / POWER_OFF_AUTO。
 *   ⚠ 调用前 UI 层应已刷出"关机画面"; 本函数不再碰画面内容。 */
void power_mgr_off(int origin);

/* 开机**最早期**调用(早于一切初始化, app_main 首行):
 * 上次是 MANUAL 关机唤醒时, 要求 PWR 键仍被按住且满 0.5s 才放行;
 * 不足 0.5s(碰一下) → 判为误触, 直接回深睡, 不返回。 */
void power_mgr_wake_gate(void);

/* 本次启动是否由"关机深睡"唤醒(EXT1)。 */
bool power_mgr_woke_from_off(void);

/* 本次唤醒的关机来源(POWER_OFF_MANUAL / POWER_OFF_AUTO); 非关机唤醒返回 -1。 */
int power_mgr_wake_origin(void);

/* 开机打印复位/唤醒原因(排障用)。 */
void power_mgr_boot_diag(void);

/* 分阶段功耗诊断掩码 (默认 0x7F = 全关, 正式关机行为不变)。
 * 位: 0x01 屏 / 0x02 音频 / 0x04 SD / 0x08 触摸 / 0x10 GPS / 0x20 震动 / 0x40 引脚隔离
 * 用法: remcon `sleepstage 0x13` ⇒ 只关屏+音频+GPS, 其余保持, 立刻深睡。 */
void power_mgr_set_stage(uint32_t mask);
uint32_t power_mgr_get_stage(void);

/* 深睡前钩子 (在"电源域配置 / 屏 deinit"之后、"esp_deep_sleep_start()"之前的原位置调用)。
 * 电源统计 (wallpapers/power_stat.c) 靠它结账"醒着段", 不必让本组件反向依赖 wallpapers。 */
void power_mgr_set_pre_sleep_hook(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif /* POWER_MGR_H */
