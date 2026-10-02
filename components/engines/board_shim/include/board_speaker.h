/**
 * @file board_speaker.h
 * @brief board_speaker 兼容层 — 适配 gb_emu 到 audio_player
 *
 * 参考项目 (esp32-s3-rlcd-gb-emulator) 的 gb_emu 组件依赖 board_speaker
 * 组件提供音频接口. 本头文件提供同名接口, 内部转发到 audio_player,
 * 使 gb_emu 源码无需修改即可编译.
 */
#ifndef BOARD_SPEAKER_H
#define BOARD_SPEAKER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 兼容类型定义 */
typedef struct {
    int sample_rate_hz;
    int channel_count;
    int bits_per_sample;
    int volume;  /* 0-100 */
} board_speaker_config_t;

/* 默认配置宏 (匹配参考项目的 BOARD_SPEAKER_DEFAULT_CONFIG) */
#define BOARD_SPEAKER_DEFAULT_CONFIG() { \
    .sample_rate_hz = 24000,             \
    .channel_count = 2,                  \
    .bits_per_sample = 16,               \
    .volume = 50,                        \
}

/* === board_speaker 兼容接口 === */
esp_err_t board_speaker_init(const board_speaker_config_t *config);
esp_err_t board_speaker_write(const void *pcm, size_t bytes, size_t *bytes_written, int timeout_ms);
esp_err_t board_speaker_set_volume(uint8_t volume);
esp_err_t board_speaker_self_test(void);

#ifdef __cplusplus
}
#endif

#endif
