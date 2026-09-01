#ifndef WALLPAPERS_H
#define WALLPAPERS_H

#include "st7305.h"
#include <stdbool.h>
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
/* ==== 屏保状态机 (从 os_mgr 迁移归位: 渲染+状态同属壁纸模块) ==== */
bool os_screensaver_active(void);
void os_screensaver_reset(void);
void os_screensaver_force_enter(void);
/* 每帧主循环调用: 桌面空闲>WP_SS_DELAY 进入星空屏保; 离开/弹窗/新输入退出.
 * 参数 by 值, 避免 wallpapers 依赖 os 头 (无循环依赖). */
void wp_screensaver_poll(bool on_main, uint32_t now_ms, uint32_t last_input_ms, bool in_dialog);


#ifdef __cplusplus
}
#endif

#endif
