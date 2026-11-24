/**
 * os_notify.h — 系统通知 API (声音 + 震动).
 *
 * 应用(番茄钟/摩斯/计时器等)需要"响一下/震一下"时, 统一走本 API,
 * 不直接 include audio_player.h / vibrator.h, 也不直接调组件内部.
 * 好处:
 *   - 系统后续可整体替换音频/震动后端, 不影响任何应用;
 *   - 接入 elf_loader 动态应用时, 只需在符号表导出本 API, 应用无需重新链接底层.
 */
#ifndef OS_NOTIFY_H
#define OS_NOTIFY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vibrator.h"   /* vib_pattern_t, VIB_PAT_SUCCESS 等内置花样 */

#ifdef __cplusplus
extern "C" {
#endif

/* 震动: 播放预设花样 (内部已含 vibrator_ready 判断, 调用方无需预检). */
void os_notify_vibrate(const vib_pattern_t *pat);
/* 震动: 单次 (ms, strength 0~100). */
void os_notify_vibrate_tap(uint16_t ms, uint8_t strength);

/* 震动: 初始化 (幂等, 全局). */
void os_notify_vibrate_init(void);
/* 震动: 停止输出 (退出页时调用, 不解除总使能). */
void os_notify_vibrate_stop(void);

/* 音调: 初始化 (幂等, 全局). 用于摩斯等需要连续发声的应用. */
void os_notify_tone_init(void);
/* 单音: 播放 freq Hz 方波 dur_ms 毫秒 (阻塞调用者). */
void os_notify_tone(uint16_t freq, uint16_t dur_ms);
/* 单音(非阻塞): 喂完 PCM 立刻返回. amp 传 0 用默认音量.
 * 游戏按键音 / 连发提示音用它, 否则每次发声都会卡住一整帧. */
void os_notify_tone_async(uint16_t freq, uint16_t dur_ms, uint16_t amp);

/* 旋律音符 (布局与 tone_player 的 tone_note_t 一致, 编译期已校验). */
typedef struct { int freq; int ms; } os_note_t;
/* 旋律(非阻塞): 后台任务顺序播放. ⚠ notes 必须是**静态/全局**数据 (只存指针不拷贝);
 * 末尾放一个 {0,0} 作终止, 或按 count 播完即止。 */
void os_notify_tone_melody(const os_note_t *notes, int count);
/* 关闭音调. */
void os_notify_tone_stop(void);
/* 音调是否就绪 (可发声). */
bool os_notify_tone_ready(void);

/* 实时 PCM 提示音: 逐帧馈入 I2S (返回实际写入帧数). 用于合成和弦/提示音. */
size_t os_notify_sound_feed(const int16_t *pcm, size_t frames, int sample_rate);
/* 停止提示音. */
void os_notify_sound_stop(void);
/* 清空 PCM 缓冲 (切断连续音尾音). */
void os_notify_sound_flush(void);

#ifdef __cplusplus
}
#endif
#endif /* OS_NOTIFY_H */
