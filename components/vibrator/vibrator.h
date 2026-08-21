/**
 * @file vibrator.h
 * @brief 苹果 Taptic Engine (LRA) 震动马达驱动 (V1.0.76)
 *
 * 硬件: ESP32-S3 单极性驱动裸 Taptic Engine.
 *   GPIO6 --LEDC PWM(谐振频率方波)--> N-MOS 栅极 -> 线圈通断
 *   马达线圈两端并联续流二极管.
 *
 * ESP32 GPIO 无法直接驱动 LRA 线圈(电阻≤20Ω, 峰值电流近 1A), 必须经
 * 一颗逻辑电平 N-MOS 管切换 3.3V/5V 电源. 见 README/接线说明.
 */
#ifndef VIBRATOR_H
#define VIBRATOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 LEDC 定时器/通道 (内部调用一次即幂等). */
void vibrator_init(void);

/* 设置驱动占空比 strength=0~100. 由 LEDC 产生谐振频率方波驱动 MOS 通断.
 * 0 表示静止. */
void vibrator_set(uint8_t strength);

/* 彻底停止输出 (占空比归 0). */
void vibrator_stop(void);

/* 单次震动指定时长 (阻塞, 期间保持最强输出后停止). ms<=0 仅停止. */
void vibrator_buzz(uint16_t ms);

/* 是否已初始化. */
bool vibrator_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBRATOR_H */