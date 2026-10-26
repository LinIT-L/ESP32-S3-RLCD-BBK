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
 * [2026-10-14 linit 定案] 0 档=静音; 1 档=旧曲线 6 档的响度(68%); 1~9 档在
 * 68%~100% 之间均分(每档 4%). => 0/68/72/76/80/84/88/92/96/100 */
#define AUDIO_VOL_STEPS 10   /* 档位范围 0..9 */
#define AUDIO_VOL_BASE_PCT 68 /* 1 档百分比; 配合"0xBF=0dB"的官方曲线: 6 档=88%=+16.5dB 已经够响 */
int  audio_player_vol_to_percent(int step);   /* 0-9 档 -> 0-100 百分比 */
int  audio_player_vol_to_step(int percent);   /* 0-100 百分比 -> 0-9 档 */

/* V1.0.68: 全局强制静音 (禁用音频). true=彻底关掉所有声音 (音乐 + 游戏引擎 PCM),
 * 停止正在播放的音乐并把编解码器音量拉到 0; false=恢复之前音量. */
void audio_player_set_muted(bool muted);
bool audio_player_is_muted(void);

/* 获取当前状态 */
audio_state_t audio_player_get_state(void);

/* === 运行时音频诊断 (供 remcon 串口排查, 2026-10-09) ===
 * 打印: 音量/静音/功放/平台判断/状态机/I2S 采样率 + ES8311 全寄存器回读。
 * 用于定位"初始化全成功但没声音" —— 见 es8311.h 的诊断 API 说明。 */
void audio_player_diag(void);

/* ★ 进深睡前必须调这个 (2026-10-09) —— 一条龙把音频链路彻底关掉:
 *   ① es8311_shutdown(): codec 静音 + power-down (0x32=0 / 0x00=0)。
 *      ⚠ 只调 audio_pa_force_off() 是**不够的** —— 那只关功放, codec 本身仍在
 *      工作态持续耗电(实测"关机还掉电"的来源之一)。而 audio_player_deinit()
 *      虽然会关 codec, 但它全工程**从未被调用** ⇒ es8311_shutdown() 一直是死代码。
 *   ② audio_pa_force_off(): 关功放 + hold 低电平 (深睡时 GPIO 变高阻, 不 hold 会被上拉拉高)。
 *   必须在 esp_deep_sleep_start() **之前**调用 (之后没有代码会执行)。 */
void audio_player_sleep_for_off(void);


/* === 录音机：WAV 录制（2026-10-27） ===
 * 录音在独立任务里跑（CPU1 / 优先级 8），stop 是同步的：返回时 WAV 头已回填。
 * path 用 TF 卡绝对路径，如 "/sdcard/Rec/rec_001.wav"。返回 0 成功。 */
int  audio_player_rec_start(const char *path, int sample_rate);
void audio_player_rec_stop(void);
bool audio_player_rec_active(void);
uint32_t audio_player_rec_ms(void);       /* 已录时长 (ms) */
uint32_t audio_player_rec_bytes(void);    /* 已写 PCM 字节数 */
const char *audio_player_rec_path(void);  /* 当前/最后一次录音路径 */

/* === WAV 播放（异步任务，只支持 16bit PCM WAV） === */
int  audio_player_play_wav(const char *path);
void audio_player_wav_stop(void);
bool audio_player_wav_active(void);
uint32_t audio_player_wav_ms(void);        /* 已播 (ms) */
uint32_t audio_player_wav_total_ms(void);  /* 总长 (ms) */

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

/* === 功放芯片使能控制 (PA_EN) ===
 *
 * 小智 ESP32-S3-AI-2E 模块: 功放 **LPA2103A** 的 SD(使能) 脚在**模块内部**接到
 * ESP32 的 **GPIO46**, 高=工作 / 低=关断。微雪官方板 AAXS2005 的 `PA_EN`、自制板
 * NS4168 的 `CTRL` 也在同一根脚上(同为高=开) —— 所以 **GPIO46 = 喇叭出声的唯一总闸**。
 *
 * 正常播放/暂停/静音由 audio_player 内部自动管理(含消爆音时序), 下面这几个 API 是
 * 给**板级初始化**和**关机/深睡**用的:
 *   - audio_pa_init()       最早期调一次: 配成输出并**立刻拉低**, 幂等
 *   - audio_pa_set(on)      手动开/关 (带消爆音等待)
 *   - audio_pa_is_on()      当前状态 (诊断/状态页)
 *   - audio_pa_force_off()  关机、进深睡前调: 关断 + hold 低电平, 睡眠期间不耗电
 */
void audio_pa_init(void);
void audio_pa_set(bool on);
bool audio_pa_is_on(void);
void audio_pa_force_off(void);

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

/* ★ 2026-10-25 [LINTOS] 音效混音（MP3 音乐 + 游戏音效）
 *   MP3 模式下音乐由解码任务独占 I2S，SDLPal 的音效（VOC 实时合成）无处输出。
 *   sdl_audio.c 把音效 PCM 推进独立环形缓冲，MP3 解码循环写 I2S 前叠加。
 *   两者都是 44100Hz 立体声交错 int16（采样率由 SDL_OpenAudio 统一），可直接相加。 */
void audio_player_fx_push(const int16_t *data, int frames);   /* 音效入队（满则丢） */
void audio_player_fx_mix(int16_t *pcm, uint32_t samples);     /* 叠加到 pcm（含防削波） */
void audio_player_fx_reset(void);                             /* 换曲时清空残留 */

#ifdef __cplusplus
}
#endif

#endif

/* ★ 2026-10-16: app_board 按板型表下发音频引脚 (drivers/services 不反向依赖上层) */
void audio_player_apply_pins(int mclk, int bclk, int ws, int dout, int din,
                             int codec_sda, int codec_scl, int pa);
