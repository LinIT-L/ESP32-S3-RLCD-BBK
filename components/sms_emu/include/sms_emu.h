#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SMS/GG (SMSPlus) 模拟器封装 — 移植自 esp-box-emu components/sms.
 * 生命周期与 NES/GB 一致. 选中 .sms/.gg 后 sms_emu_start(path) 启动模拟任务. */

esp_err_t sms_emu_background_init(void);
void sms_emu_unload(void);

typedef void (*sms_emu_progress_cb_t)(int percent);
void sms_emu_set_progress_cb(sms_emu_progress_cb_t cb);

esp_err_t sms_emu_start(const char *path);
esp_err_t sms_emu_stop(void);
void sms_emu_wait_stopped(void);

void sms_emu_set_joypad(uint8_t joypad);
void sms_emu_set_volume(uint8_t volume);
uint8_t sms_emu_get_volume(void);

void sms_emu_pause(void);
void sms_emu_resume(void);
void sms_emu_set_fullscreen(int mode);

#ifdef __cplusplus
}
#endif
