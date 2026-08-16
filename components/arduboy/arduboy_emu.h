/**
 * @file arduboy_emu.h
 * @brief Arduboy 模拟器入口 - 游戏选择与主循环
 */
#ifndef ARDUBOY_EMU_H
#define ARDUBOY_EMU_H

#include "st7305.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARDUBOY_GAME_BREAKOUT = 0,
    ARDUBOY_GAME_SNAKE,
    ARDUBOY_GAME_COUNT,
} arduboy_game_id_t;

/* 单个游戏的实现接口 (由各 game_*.c 提供一个 const 实例) */
typedef struct {
    void (*init)(void);     /* 游戏开始/重开时调用 */
    void (*update)(void);   /* 每帧逻辑更新, 内部读取按键 */
    void (*render)(void);   /* 每帧渲染, 画到 128x64 framebuffer (调用前模拟器已 clear) */
    const char *name;       /* 游戏名称 (ASCII) */
} arduboy_game_impl_t;

/* 初始化 (传入 ST7305 句柄, 内部调用 arduboy_init) */
void arduboy_emu_init(st7305_handle_t *lcd);

/* 选择游戏 */
void arduboy_emu_select_game(arduboy_game_id_t game);

/* 运行游戏 (进入游戏循环, 阻塞直到 BOOT 长按退出) */
void arduboy_emu_run(void);

/* 停止游戏 */
void arduboy_emu_stop(void);

/* 获取游戏列表 */
const char *arduboy_emu_get_game_name(int idx);
int arduboy_emu_get_game_count(void);

#ifdef __cplusplus
}
#endif
#endif
