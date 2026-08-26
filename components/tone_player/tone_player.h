/**
 * @file tone_player.h
 * @brief 简易音调播放器 (V1.0.68): LEDC PWM → GPIO48 → AXS2005B 功放 → 喇叭
 *
 * 生成常见电子声音 (开机/关机/确认/错误/闹铃) 和简单旋律, 无需解码芯片.
 * 用法:
 *   tone_player_init();              // 初始化 (幂等, 仅在音频方案=方波直驱时调用)
 *   tone_play_effect(TONE_EFFECT_BOOT);
 *   tone_beep(880, 100);             // 单音 (阻塞 100ms)
 *   tone_play_melody(notes, n);      // 旋律 (后台任务非阻塞播放, notes 需为静态数组)
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 LEDC PWM (GPIO48), 幂等; 返回是否就绪 */
void tone_player_init(void);
/* 立即静音 */
void tone_stop(void);
/* 是否已就绪 (可发声) */
bool tone_player_ready(void);

/* 持续音: 立即以 freq Hz 发声, 不阻塞 (不自动停止). 由 tone_stop() 关闭.
 * 用于模拟器蜂鸣器等需要"按下持续响、松开停"的连续音效 (V1.0.9x 暴龙机). */
void tone_tone_on(int freq_hz);

/* 单音: 播放 freq Hz 方波 ms 毫秒 (阻塞调用者 ms 时间) */
void tone_beep(int freq_hz, int ms);

/* 常见电子音效 */
typedef enum {
    TONE_EFFECT_CONFIRM,   /* 短促 "嘀" (确认/按键) */
    TONE_EFFECT_CANCEL,    /* 低沉 "嘟" (取消/返回) */
    TONE_EFFECT_ERROR,     /* 急促三连 "嘀嘀嘀" (错误) */
    TONE_EFFECT_BOOT,      /* 开机 上行音 */
    TONE_EFFECT_SHUTDOWN,  /* 关机 下行音 */
    TONE_EFFECT_ALARM,     /* 闹铃提醒 (三声) */
} tone_effect_t;
void tone_play_effect(tone_effect_t e);

/* 简单旋律音符: freq=Hz, ms=时长 */
typedef struct { int freq; int ms; } tone_note_t;

/* 内置旋律主题 */
typedef enum {
    TONE_THEME_BOOT_UP,     /* 开机: C-E-G-C 上行 */
    TONE_THEME_TWINKLE,     /* 小星星 (一闪一闪亮晶晶) */
    TONE_THEME_BIRTHDAY,    /* 祝你生日快乐 */
} tone_theme_t;
void tone_play_theme(tone_theme_t t);

/* 播放自定义旋律 (非阻塞: 后台任务顺序播放). notes 必须指向静态/全局数据 */
void tone_play_melody(const tone_note_t *notes, int count);

#ifdef __cplusplus
}
#endif
