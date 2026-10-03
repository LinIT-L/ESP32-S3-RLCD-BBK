/**
 * os_notify.c — 系统通知 API 实现 (包装 audio_player / vibrator).
 */
#include "os_notify.h"
#include "audio_player.h"
#include "vibrator.h"
#include "tone_player.h"   /* tone_player_init/tone_beep/tone_stop/tone_player_ready */

void os_notify_vibrate(const vib_pattern_t *pat) {
    if (!pat) return;
    if (!vibrator_ready()) return;
    vibrator_play_pattern(pat);
}

void os_notify_vibrate_tap(uint16_t ms, uint8_t strength) {
    if (!vibrator_ready()) return;
    vibrator_tap(ms, strength);
}

size_t os_notify_sound_feed(const int16_t *pcm, size_t frames, int sample_rate) {
    return audio_player_feed_pcm(pcm, frames, sample_rate);
}

void os_notify_sound_stop(void) {
    audio_player_stop();
}

void os_notify_sound_flush(void) {
    audio_player_flush_pcm();
}

/* ============ 震动 / 音调 生命周期 (摩斯等需要连续发声的应用) ============ */
void os_notify_vibrate_init(void) {
    vibrator_init();
}

void os_notify_vibrate_stop(void) {
    vibrator_stop();
}

void os_notify_tone_init(void) {
    tone_player_init();
}

void os_notify_tone(uint16_t freq, uint16_t dur_ms) {
    tone_beep((int)freq, (int)dur_ms);
}

void os_notify_tone_async(uint16_t freq, uint16_t dur_ms, uint16_t amp) {
    tone_beep_async((int)freq, (int)dur_ms, (int)amp);
}

/* os_note_t 与 tone_note_t 都是 { int freq; int ms; } —— 把这句话钉在编译期,
 * 以后任何一边改了字段就会直接编译失败, 而不是静默跑飞. */
_Static_assert(sizeof(os_note_t) == sizeof(tone_note_t), "os_note_t 布局必须与 tone_note_t 一致");

void os_notify_tone_melody(const os_note_t *notes, int count) {
    tone_play_melody((const tone_note_t *)notes, count);
}

void os_notify_tone_stop(void) {
    tone_stop();
}

bool os_notify_tone_ready(void) {
    return tone_player_ready();
}
