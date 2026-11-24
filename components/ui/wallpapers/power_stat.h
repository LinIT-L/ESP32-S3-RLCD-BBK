#ifndef WALLPAPERS_POWER_STAT_H
#define WALLPAPERS_POWER_STAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 电源统计 (linit 2026-10-14) —— 「设置 → 休眠与电源 → 电池信息」的三个时长数字
 *
 * 统计区间 = **上次 USB 插入(充电) 到现在**。区间内分别累计:
 *   运行时间 = 设备醒着的累计时长 (不含深睡)
 *   睡眠时间 = 深睡的累计时长
 *   充电至今 = 上面两者之和 (= 区间总长), 三者自洽
 *
 * 为什么能算出真实的深睡时长:
 *   用 RTC 计时器 `esp_rtc_get_time_us()` —— 它在深睡期间**继续走**, 醒来接着数;
 *   而 `esp_timer_get_time()` 每次深睡唤醒都从 0 重来, 测不了睡眠。
 *   状态本身放在 RTC 内存 (RTC_NOINIT_ATTR), 跨深睡保留, 掉电才清。
 *
 * ⚠ 当前板没把 CHRG/VBUS 接给 MCU (见 docs/硬件与IO.md), 所以"充电"只能按
 *   `usb_hid_host_mounted()` (USB 被主机枚举) 判定 —— 纯充电器检测不到。
 *   linit 明确: 不接充电检测, 就按 USB 插入时间算。
 * ==========================================================================*/

/* 开机最早期调用一次 (结算上次睡眠, 起新的"醒着段")。 */
void power_stat_init(void);

/* 低频周期调用 (建议 ≥1s 一次, 由屏保监视任务驱动): USB 插入边沿 → 三个统计清零。 */
void power_stat_tick(void);

/* ⚠ 进深睡前必须调用 (两条关机路径: 手动 wp_power_off 与 误触回睡 都要)。 */
void power_stat_on_deep_sleep(void);

typedef struct {
    uint32_t awake_s;       /* 上次充电以来 醒着累计秒 */
    uint32_t sleep_s;       /* 上次充电以来 深睡累计秒 */
    uint32_t since_usb_s;   /* 上次充电到现在 (= awake_s + sleep_s) */
    bool     usb_now;       /* 当前 USB 是否连着 (充电/接电脑) */
} power_stat_t;

void power_stat_get(power_stat_t *out);

/* ==== 唤醒/入睡事件日志 (★ 2026-10-26 linit: "存储更详细的信息辅助判断") ====
 * 只存**累计值**没法回答"是哪一次唤醒、几点醒的、醒了多久"。所以另存一份环形日志:
 *   每条 = 时刻(epoch 秒) + 类型(入睡/唤醒) + 时长 + 唤醒源, 24 条 ≈ 12 个睡/醒周期。
 *   放在 RTC 内存 (RTC_NOINIT_ATTR), **跨深睡保留**, 掉电才清。
 *   读出方式: esptool dump_mem <s_log 地址> (nm 查符号) 或串口 `pwr` 命令。
 * ⚠ 时刻用的是系统时间: 已联网校时 = 真实北京时间; 未校时 = 默认 2026-09-01 起算,
 *   **只有相对间隔可信**。 */

#define PS_LOG_N 24
typedef struct {
    uint32_t t;        /* 事件时刻: epoch 秒 */
    uint32_t dur_s;    /* 入睡事件 = 这次醒着持续多久; 唤醒事件 = 刚结束的深睡时长 */
    uint8_t  kind;     /* 1=入睡 2=唤醒 3=上电/掉过电 */
    uint8_t  cause;    /* kind=唤醒: esp_sleep_wakeup_cause_t */
    uint16_t rsv;
} power_stat_ev_t;

/* 打印最近的事件日志 (串口诊断用; `pwr` 命令会调用) */
void power_stat_log_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* WALLPAPERS_POWER_STAT_H */
