#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MD/Genesis (Gwenesis) 模拟器封装 — 移植自 esp-box-emu components/genesis.
 * 生命周期与 NES/GB 一致: 进入 MD 菜单后台预加载, 返回主菜单 unload,
 * 选中 .md/.gen/.bin 后 md_emu_start(path) 启动模拟任务. */

esp_err_t md_emu_background_init(void);
void md_emu_unload(void);

typedef void (*md_emu_progress_cb_t)(int percent);
void md_emu_set_progress_cb(md_emu_progress_cb_t cb);

esp_err_t md_emu_start(const char *path);
esp_err_t md_emu_stop(void);
void md_emu_wait_stopped(void);

/* 输入: 引擎帧内自行读取 input_get_held_gb_joypad, 此接口保留以兼容页循环分发 (空实现) */
void md_emu_set_joypad(uint8_t joypad);
void md_emu_set_volume(uint8_t volume);
uint8_t md_emu_get_volume(void);

void md_emu_pause(void);
void md_emu_resume(void);
void md_emu_set_fullscreen(int mode);   /* 0=点对点, 1=全屏, 2=拉伸 */

#ifdef __cplusplus
}
#endif
