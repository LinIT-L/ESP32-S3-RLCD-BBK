#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 播放状态 */
typedef enum {
    AUDIO_STATE_IDLE = 0,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED,
    AUDIO_STATE_STOPPED,
} audio_state_t;

/* 初始化音频硬件 (ES8311 + I2S + 功放)
 * 返回 0=成功 */
int audio_player_init(void);

/* 播放指定 MP3 文件 (完整路径)
 * 返回 0=成功 */
int audio_player_play(const char *filepath);

/* 暂停/恢复 */
void audio_player_pause(void);
void audio_player_resume(void);

/* 停止播放 */
void audio_player_stop(void);

/* 设置音量 (0-100) */
void audio_player_set_volume(int percent);
int  audio_player_get_volume(void);

/* V1.0.68: 全局强制静音 (禁用音频). true=彻底关掉所有声音 (音乐 + 游戏引擎 PCM),
 * 停止正在播放的音乐并把编解码器音量拉到 0; false=恢复之前音量. */
void audio_player_set_muted(bool muted);
bool audio_player_is_muted(void);

/* 获取当前状态 */
audio_state_t audio_player_get_state(void);

/* 获取当前播放文件名 */
const char *audio_player_get_filename(void);

/* 获取播放进度 (0-1000, 千分比) */
int audio_player_get_progress(void);

/* 音频后台任务是否正在运行 */
bool audio_player_is_running(void);

/* 释放音频资源 */
void audio_player_deinit(void);

/* 直接馈入 PCM 数据到 I2S 输出 (用于游戏模拟器等实时音频)
 * data: int16_t 立体声 PCM 数据 (L/R 交错)
 * frames: 帧数 (每帧 = L+R 两个 sample)
 * sample_rate: 采样率 (如 22050, 44100)
 * 返回实际写入的帧数 */
size_t audio_player_feed_pcm(const int16_t *data, size_t frames, int sample_rate);

/* 立即清空 PCM 环形缓冲 (释放按键时切断连续音尾音) */
void audio_player_flush_pcm(void);

/* 诊断: 当前实际配置的 I2S 采样率 */
int audio_player_get_i2s_rate(void);

/* === 麦克风输入 (ES7210 双麦克风阵列, I2S RX, GPIO10) ===
 * 电子书敲击翻页等场景使用; 启动会重配 I2S 时钟到指定采样率 (16kHz 推荐) */
int  audio_player_mic_start(int sample_rate);
void audio_player_mic_stop(void);
/* 读取立体声 PCM (每帧 = L/R 两个 int16), 返回实际帧数, 失败返回 -1 */
int  audio_player_mic_read(int16_t *buf, int frames, int timeout_ms);
bool audio_player_mic_active(void);

#ifdef __cplusplus
}
#endif

#endif
