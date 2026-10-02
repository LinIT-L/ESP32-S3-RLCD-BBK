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

void os_notify_tone_stop(void) {
    tone_stop();
}

bool os_notify_tone_ready(void) {
    return tone_player_ready();
}
