/**
 * @file vibrator.h
 * @brief 震动马达驱动 (V1.1.0: DRV2605L 驱动 LRA 首选, GPIO6/LEDC 兜底)
 *
 * 硬件:
 *  - DRV2605L (I2C 0x5A, 独立软件I2C) -> OUT+/OUT- -> LRA 线性马达.
 *    SDA=GPIO42 SCL=GPIO47 (见 vibrator.c, 避开 strapping/ADC 脚), EN 接高.
 *  - 兜底: GPIO6 --LEDC PWM--> N-MOS --> 裸 LRA (未接 DRV2605L 时自动回退).
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

/* 初始化: 优先探测 DRV2605L (软件I2C), 检测不到回退 GPIO6/LEDC. 幂等. */
void vibrator_init(void);

/* 总开关 (由「震动反馈」设置调用; false 立即停止震动). */
void vibrator_set_enabled(bool en);

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