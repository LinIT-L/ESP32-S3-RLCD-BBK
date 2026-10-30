/**
 * morse_engine.h — 摩斯播放引擎 (页面唯一碰音频的地方)
 *
 * 为什么不用 tone_beep():
 *   tone_beep() = 合成 PCM + vTaskDelay(ms)。5 WPM 时一个"划"要阻塞 720ms，
 *   期间无法响应"停止"；且每次发声 malloc 10~32KB。本引擎改为:
 *     · 自己按采样点合成方波 —— 时长由采样数决定 (精度 1/44100s，与调度无关)
 *     · 每 40ms 喂一片 os_notify_sound_feed() (现有公共 API，不改公共模块)
 *     · 片间用 xTaskNotifyWait 等待 —— stop 立即唤醒，响应 ~0ms
 *
 * 停止延迟下限: flush 只能清环形缓冲，清不掉 DMA ⇒ 3×576 帧 = 39.2ms @44.1k
 * (audio_player.c:513-515)。实测预期 40~60ms。
 *
 * 预填充: 首片前先 flush (把 audio_out 的 primed 置 true, 跳过 8192 帧闸门)，
 *   再等 25ms 才开始写，避免 "flush 清掉刚写的数据" 的竞态。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ★ 2026-10-30: morse_engine.c 引用了这两个状态名, 但头文件原先未定义 → 编译失败。
 *   这里补上最小定义 (纯内部状态, 不影响对外 API)。 */
enum {
    MRE_IDLE = 0,   /* 空闲 */
    MRE_PLAY = 1,   /* 正在播放 */
};

#ifdef __cplusplus
extern "C" {
#endif

/* 播放文本 (内部构建计划/展开元素). 返回 0=已受理; -1=参数错/空文本;
 * -2=无可播放元素; -3=任务创建失败 */
int  mre_play(const char *text, uint8_t wpm, uint16_t freq_hz);
void mre_stop(void);        /* 停播 + flush 环形缓冲 (≤200ms 内收干净) */
bool mre_playing(void);
int  mre_progress(void);    /* 已完成字符数 */
int  mre_total(void);       /* 总发声字符数 */
bool mre_audio_ok(void);    /* 音频链路是否真的吃进了数据 (false = 应提示用户) */
bool mre_muted(void);       /* 播放时系统处于静音 (只走时间轴) */

#ifdef __cplusplus
}
#endif
