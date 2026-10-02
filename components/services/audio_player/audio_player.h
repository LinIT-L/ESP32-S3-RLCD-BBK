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

/* === 统一音量档位 (0-9) ===
 * 系统设置页与各引擎"引擎优化"共用同一 0-9 档位, 底层换算成 0-100 百分比.
 * 0 档=静音; 9 档=100%; 每档约 11%. */
#define AUDIO_VOL_STEPS 10   /* 档位范围 0..9 */
int  audio_player_vol_to_percent(int step);   /* 0-9 档 -> 0-100 百分比 */
int  audio_player_vol_to_step(int percent);   /* 0-100 百分比 -> 0-9 档 */

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

/* 跳转播放进度 (0..1000 千分比). 仅在播放/暂停中有效; 任务循环内安全执行, 自动帧同步.
 * CBR 精确, VBR 近似. */
void audio_player_seek_to(int progress_0_1000);

/* 上一曲是否"自然播放结束" (非用户停止). 一次性消费: 读取即清零.
 * 供播放器页面实现单曲/列表/随机循环自动切歌. */
bool audio_player_track_ended(void);

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

/* V1.0.71: 音频振幅表 (峰值 0..1000 千分比, attack 立即/decay 10% 平滑).
 * MP3 解码任务与 PCM 环形缓冲两条 I2S 写入路径均更新.
 * 供"随音乐震动"等实时响应功能按响度采样. */
int32_t audio_player_get_meter(void);

/* 诊断: 当前实际配置的 I2S 采样率 */
int audio_player_get_i2s_rate(void);

/* === 麦克风输入 (I2S RX, GPIO10): ES7210(GPIO10 ADC) 或 INMP441 直读 ===
 * 启动会重配 I2S 时钟到指定采样率 (16kHz 推荐) */
int  audio_player_mic_start(int sample_rate);
void audio_player_mic_stop(void);
/* 读取立体声 PCM (每帧 = L/R 两个 int16), 返回实际帧数, 失败返回 -1 */
int  audio_player_mic_read(int16_t *buf, int frames, int timeout_ms);
bool audio_player_mic_active(void);

#ifdef __cplusplus
}
#endif

#endif
