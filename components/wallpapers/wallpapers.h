#ifndef WALLPAPERS_H
#define WALLPAPERS_H

#include "st7305.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 内置壁纸程序 id (V4: 仅保留星空, 由 menu_system 屏保实现; 其余动态壁纸已删除) */
#define WP_PROG_STARS    0
#define WP_PROG_COUNT    1

const char *wp_prog_name(int id);

/* 渲染一帧到 LCD (内部 1bpp PSRAM 缓冲 + flush) */
void wp_program_render(st7305_handle_t *lcd, int prog, uint32_t now_ms);

/* 释放壁纸 PSRAM 帧缓冲 (退出壁纸时调用, 下次渲染自动重新分配) */
void wp_release_buffers(void);

#ifdef __cplusplus
}
#endif

#endif
