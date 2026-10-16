#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NES (nofrendo) 模拟器封装 — 移植自 esp-box-emu components/nes.
 * 生命周期与 GB/GBC/AB 一致: 进入 FC 菜单后台预加载, 返回主菜单 unload,
 * 选中 .nes 后 nes_emu_start(path) 启动模拟任务.
 * 输入沿用 GB/peanut 掩码 (bit0=A bit1=B bit2=Select bit3=Start
 *  bit4=右 bit5=左 bit6=上 bit7=下, 低电平有效). */

esp_err_t nes_emu_background_init(void);
void nes_emu_unload(void);

typedef void (*nes_emu_progress_cb_t)(int percent);
void nes_emu_set_progress_cb(nes_emu_progress_cb_t cb);

esp_err_t nes_emu_start(const char *path);
esp_err_t nes_emu_stop(void);
void nes_emu_wait_stopped(void);

void nes_emu_set_joypad(uint8_t joypad);
void nes_emu_set_volume(uint8_t volume);
uint8_t nes_emu_get_volume(void);

void nes_emu_pause(void);
void nes_emu_resume(void);
void nes_emu_set_fullscreen(int mode);   /* 0=点对点, 1=全屏, 2=拉伸 */

#ifdef __cplusplus
}
#endif
