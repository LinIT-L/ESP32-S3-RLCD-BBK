/**
 * @file arduboy_avr.h
 * @brief Arduboy AVR 模拟核心 (基于 simavr 的 ATmega32u4 仿真)
 *
 * 从 TF 卡加载 .hex 游戏 (Arduboy 编译产物), 直接仿真 AVR CPU 并渲染到
 * ST7305 反射屏. 不使用内置游戏, 不占用额外虚拟硬件.
 *
 * 生命周期 (同 gb_emu/gbc_emu):
 *   - 进入 arduboy 二级菜单时调用 arduboy_avr_background_init() 后台懒加载核心
 *   - 返回主菜单时调用 arduboy_avr_unload() 释放全部内存
 */
#ifndef ARDUBOY_AVR_H
#define ARDUBOY_AVR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 进入 arduboy 菜单时后台预加载核心 (幂等). lcd 为 ST7305 句柄. */
esp_err_t arduboy_avr_background_init(st7305_handle_t *lcd);

/* 返回主菜单时释放全部内存 (含 simavr CPU 实例) */
void arduboy_avr_unload(void);

/* 加载 AB 目录下的 .hex 游戏并启动模拟任务 (异步启动) */
esp_err_t arduboy_avr_start(const char *path);

/* 停止模拟任务 */
esp_err_t arduboy_avr_stop(void);

/* 设置按键状态: 复用 GB 手柄 joypad 掩码 (低电平有效)
 *   bit0=A bit1=B bit4=右 bit5=左 bit6=上 bit7=下; 1=松开 0=按下 */
void arduboy_avr_set_joypad(uint8_t joypad);

/* 暂停/恢复: 暂停后冻结画面, 用于菜单叠加确认弹窗 */
void arduboy_avr_pause(void);
void arduboy_avr_resume(void);

/* 游戏全屏开关 (默认开=2x, 关=1x 点对点) */
void arduboy_avr_set_fullscreen(int mode);

#ifdef __cplusplus
}
#endif

#endif /* ARDUBOY_AVR_H */
