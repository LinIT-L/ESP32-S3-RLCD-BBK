#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GBC (gnuboy) 模拟器封装。
 *
 * 与 gb_emu (Peanut-GB) 一致的生命周期/接口, 供菜单系统统一接入:
 *   - 进入 GBC 二级菜单时调用 gbc_emu_background_init() 预加载核心;
 *   - 返回主菜单时调用 gbc_emu_unload() 释放内存;
 *   - 选中 .gbc 后调用 gbc_emu_start(path) 启动任务, 运行循环由菜单驱动.
 *
 * 显示: 以 GB_PIXEL_565_BE 输出 160x144 RGB565, 经 board_rlcd_draw_gbc_line_2x_rgb565_be
 *       转成 4 级灰度 + 2x 缩放画到 1bit LCD.
 * 声音: 复用 audio_player (board_speaker_write), 24000Hz 立体声 int16.
 */

/* 进入 GBC 菜单时后台预加载核心 (幂等) */
esp_err_t gbc_emu_background_init(void);
void gbc_emu_unload(void);

/* V1.0.52: 设置 ROM 加载进度回调 (0-100%), 供菜单画统一加载进度条 */
typedef void (*gbc_emu_progress_cb_t)(int percent);
void gbc_emu_set_progress_cb(gbc_emu_progress_cb_t cb);

/* 启动/停止 gnuboy 任务 (start 只接受 .gbc / .gb 路径) */
esp_err_t gbc_emu_start(const char *path);
esp_err_t gbc_emu_stop(void);

/* 从内存启动 (gb_emu 兼容层使用; owned=true 时停止后由本组件释放 data) */
esp_err_t gbc_emu_start_data(uint8_t *data, size_t size, bool owned);

/* 设置 ROM 路径, 用于派生电池存档路径 (/sdcard/dict/GB/<rom名>.sav).
 * gb_emu 兼容层在 load_rom 时调用; gbc_emu_start(path) 内部自动设置. */
void gbc_emu_set_save_path(const char *rom_path);

/* 设置电池存档目录 (默认 "/sdcard/dict/GB"). gb_emu 兼容层用它对 GB/GBC 分区存档. */
void gbc_emu_set_save_dir(const char *dir);

/* 阻塞等待模拟任务完全退出 (gb_emu 兼容层在释放外部 ROM 缓冲前调用) */
void gbc_emu_wait_stopped(void);

/* 设置手柄按键状态 (低电平有效, 同 GB joypad 掩码) */
void gbc_emu_set_joypad(uint8_t joypad);
void gbc_emu_set_volume(uint8_t volume);
uint8_t gbc_emu_get_volume(void);

/* 暂停/恢复: 暂停后冻结画面, 用于菜单叠加确认弹窗 */
void gbc_emu_pause(void);
void gbc_emu_resume(void);

/* 游戏全屏开关 (默认开=2x, 关=1x 点对点) */
void gbc_emu_set_fullscreen(int mode);

#ifdef __cplusplus
}
#endif
