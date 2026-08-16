/**
 * @file arduboy.h
 * @brief Arduboy2 兼容层 - 让 Arduboy 游戏可以最小改动移植到 ST7305 反射式 LCD
 *
 * 设计:
 *  - 维护 128x64 的 1bpp framebuffer (Arduboy/SSD1306 格式: 每字节8个垂直像素, 128列 x 8页)
 *  - 游戏画到这个 buffer 上, arduboy_display() 时缩放 blit 到 ST7305 400x300 屏幕
 *  - 缩放: 3x 水平 + 4x 垂直 = 384x256, 在 400x300 屏幕上居中 (offset x=8, y=22)
 *  - 颜色: ARDUBOY_WHITE(1)=像素亮(可见), ARDUBOY_BLACK(0)=像素灭(背景)
 *    ST7305 反射屏: 黑色像素=可见, 故 ARDUBOY_WHITE 映射为 ST7305_COLOR_BLACK
 */
#ifndef ARDUBOY_H
#define ARDUBOY_H

#include <stdint.h>
#include <stdbool.h>
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARDUBOY_WIDTH  128
#define ARDUBOY_HEIGHT 64

/* Arduboy 按键 bitmask */
#define ARDUBOY_BTN_A      0x01
#define ARDUBOY_BTN_B      0x02
#define ARDUBOY_BTN_UP     0x04
#define ARDUBOY_BTN_DOWN   0x08
#define ARDUBOY_BTN_LEFT   0x10
#define ARDUBOY_BTN_RIGHT  0x20

/* 颜色 (Arduboy 约定: WHITE=像素亮, BLACK=像素灭) */
#define ARDUBOY_WHITE 1
#define ARDUBOY_BLACK 0

/* 初始化, 传入 ST7305 句柄 */
void arduboy_init(st7305_handle_t *lcd);

/* 清空 framebuffer (全部置为背景色 BLACK) */
void arduboy_clear(void);

/* 画点 (在 128x64 framebuffer 上) */
void arduboy_draw_pixel(int x, int y, uint8_t color);

/* 获取点颜色 (1=亮, 0=灭) */
uint8_t arduboy_get_pixel(int x, int y);

/* 画水平线/垂直线 */
void arduboy_draw_hline(int x, int y, int w, uint8_t color);
void arduboy_draw_vline(int x, int y, int h, uint8_t color);

/* 画矩形 (空心/实心) */
void arduboy_draw_rect(int x, int y, int w, int h, uint8_t color);
void arduboy_fill_rect(int x, int y, int w, int h, uint8_t color);

/* 画圆 */
void arduboy_draw_circle(int x0, int y0, int r, uint8_t color);
void arduboy_fill_circle(int x0, int y0, int r, uint8_t color);

/* 1bpp bitmap 绘制 (Arduboy 标准格式: 每字节8个垂直像素, 列优先)
 *  bitmap 布局: byte[col * ceil(h/8) + page], bit 0=该列顶部像素
 */
void arduboy_draw_bitmap(int x, int y, const uint8_t *bitmap, int w, int h, uint8_t color);

/* 压缩 bitmap (Arduboy 压缩格式, 暂空实现) */
void arduboy_draw_compressed(int x, int y, const uint8_t *bitmap, int w, int h, uint8_t color);

/* 文字绘制 (5x7 字体, ASCII 32-127) */
void arduboy_set_cursor(int x, int y);
void arduboy_print(const char *str);
void arduboy_print_char(char c);

/* 按键轮询 (每帧由模拟器主循环调用一次, 更新内部 held/edge 状态)
 * 必须在 arduboy_buttons_state/pressed/released 之前调用 */
void arduboy_poll_input(void);

/* 获取当前按键状态 (bitmask, 当前按住) */
uint8_t arduboy_buttons_state(void);

/* 是否刚按下 (edges: 本帧从松开->按下) */
uint8_t arduboy_buttons_pressed(void);

/* 是否刚松开 (edges: 本帧从按下->松开) */
uint8_t arduboy_buttons_released(void);

/* 帧刷新: 将 128x64 framebuffer 缩放 blit 到 ST7305 400x300 屏幕 */
void arduboy_display(void);

/* 帧循环延时 (帧率控制由模拟器主循环负责, 此处为兼容空实现) */
void arduboy_idle(void);

/* 音效 (空实现, 没有扬声器) */
void arduboy_tunes_play(uint16_t freq, uint16_t duration);

/* 获取帧缓冲直接指针 (高级用途, 128*64/8 = 1024 字节) */
uint8_t *arduboy_get_buffer(void);

/* EEPROM 模拟 (空实现, 不持久化) */
void arduboy_eeprom_write(int addr, uint8_t val);
uint8_t arduboy_eeprom_read(int addr);

/* 毫秒时间戳 (用于游戏计时, 取自 FreeRTOS tick) */
uint32_t arduboy_millis(void);

#ifdef __cplusplus
}
#endif

#endif
