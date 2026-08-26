#ifndef WALLPAPERS_H
#define WALLPAPERS_H

#include "st7305.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 内置壁纸程序 id (0=星空由 menu_system 自带, 1..7 在此实现)
 * V3: 删除板块漂移/赫尔曼栅格/腐蚀重生, 重做其余 7 个 */
#define WP_PROG_STARS    0
#define WP_PROG_STAIRS   1
#define WP_PROG_CLOCK    2
#define WP_PROG_BINARY   3
#define WP_PROG_LIFE     4
#define WP_PROG_CTHULHU  5
#define WP_PROG_PCB      6
#define WP_PROG_WHEAT    7
#define WP_PROG_WEATHER  8   /* V1.0.67: 天气时钟 (在 menu_system.c 实现) */
#define WP_PROG_COUNT    9

const char *wp_prog_name(int id);

/* 渲染一帧到 LCD (内部 1bpp PSRAM 缓冲 + flush) */
void wp_program_render(st7305_handle_t *lcd, int prog, uint32_t now_ms);

/* 释放壁纸 PSRAM 帧缓冲 (退出壁纸时调用, 下次渲染自动重新分配) */
void wp_release_buffers(void);

#ifdef __cplusplus
}
#endif

#endif
