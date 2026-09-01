/**
 * @file vibrator.h
 * @brief 震动马达驱动 (V1.3.x: TM6604 线性驱动, PWM 直驱 LRA)
 *
 * 硬件 (TM6604 SOT23-6):
 *  - Pin5 PWM <- GPIO42 (LEDC 190Hz 方波, 频率= LRA 谐振点)
 *  - Pin6 EN  <- GPIO48 (原 EN IO, 保持不变; 高使能)
 *  - Pin1/3 OUT+/OUT- -> LRA 线性马达; Pin2 GND; Pin4 VDD=3.3V
 *  - 原 SCL=GPIO47 空置; 原 GPIO6/LEDC 兜底路径已并入 PWM 主路径.
 *
 * 提供阻塞 vibrator_buzz 与非阻塞 vibrator_tap 两种触发; 上层可通过
 * vibrator_set_enabled 总开关在设置里统一关闭震动.
 */
#ifndef VIBRATOR_H
#define VIBRATOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化: 配置 TM6604 (EN 使能 + LEDC PWM). 幂等. */
void vibrator_init(void);

/* 总开关 (由「震动反馈」设置调用; false 立即停止震动).
 * 注意: 设置页请改用 vibrator_set_ui_enabled / vibrator_set_game_enabled. */
void vibrator_set_enabled(bool en);

/* UI 震动开关 (拖拉/菜单导航/设置调整). 任一通道开 → 硬件总开关开. */
void vibrator_set_ui_enabled(bool en);
/* 游戏震动开关 (游戏内按键). 任一通道开 → 硬件总开关开. */
void vibrator_set_game_enabled(bool en);
/* UI 震动: 仅当 UI 震动开启时触发一次 (ms 时长, strength 0~100). */
void vibrator_tap_ui(uint16_t ms, uint8_t strength);
/* 游戏震动: 仅当游戏震动开启时触发一次 (ms 时长, strength 0~100). */
void vibrator_tap_game(uint16_t ms, uint8_t strength);

/* 设置输出强度 strength=0~100 (实时). 0 静止. */
void vibrator_set(uint8_t strength);

/* 彻底停止输出. */
void vibrator_stop(void);

/* 单次震动指定时长 (阻塞, 期间最强输出后停止). ms<=0 仅停止. */
void vibrator_buzz(uint16_t ms);

/* V1.1.0: 非阻塞单次震动: 震动 ms 毫秒、strength 0~100 强度后自动停止.
 * 供按键/触摸反馈、闹钟、番茄钟/计时器调用. */
void vibrator_tap(uint16_t ms, uint8_t strength);

/* 是否已初始化 (任一种驱动就绪). */
bool vibrator_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBRATOR_H */