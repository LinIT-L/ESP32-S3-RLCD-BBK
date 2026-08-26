/**
 * @file tone_player.c
 * @brief 简易音调播放器: LEDC PWM 方波 → 功放 → 喇叭 (V1.0.68)
 *
 * 用 ESP32-S3 LEDC 定时器产生方波音调. 占空比控制音量 (8bit, ~10%),
 * 频率 = 音调. 功放 (AXS2005B) 输入为模拟差分口, 方波经功放后声音偏"电子音",
 * 适合提示音/按键音/简单旋律.
 */
#include "tone_player.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdlib.h>

#define TAG "TONE"

#define TONE_GPIO   48   /* V1.0.68: 方波输出 GPIO (飞线到功放 IN+) */
#define TONE_DUTY   25   /* 8bit 分辨率: ~10% 占空比 (控制音量, 避免过响削波) */

static bool s_ready = false;

void tone_player_init(void) {
    if (s_ready) return;
    ledc_timer_config_t tc = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .timer_num      = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz        = 880,
        .clk_cfg        = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tc) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC 定时器配置失败");
        return;
    }
    ledc_channel_config_t ch = {
        .gpio_num   = TONE_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&ch) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC 通道配置失败");
        return;
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    s_ready = true;
    ESP_LOGI(TAG, "PWM 音调就绪: GPIO%d", TONE_GPIO);
}

bool tone_player_ready(void) { return s_ready; }

void tone_stop(void) {
    if (!s_ready) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void tone_tone_on(int freq_hz) {
    if (!s_ready) return;
    if (freq_hz < 40) freq_hz = 40;
    if (freq_hz > 20000) freq_hz = 20000;
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, (uint32_t)freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, TONE_DUTY);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void tone_beep(int freq_hz, int ms) {
    if (!s_ready || ms <= 0) return;
    if (freq_hz < 40) freq_hz = 40;
    if (freq_hz > 20000) freq_hz = 20000;
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, (uint32_t)freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, TONE_DUTY);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS((uint32_t)ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void tone_play_effect(tone_effect_t e) {
    if (!s_ready) return;
    switch (e) {
    case TONE_EFFECT_CONFIRM:
        tone_beep(1200, 45);
        break;
    case TONE_EFFECT_CANCEL:
        tone_beep(320, 90);
        break;
    case TONE_EFFECT_ERROR:
        tone_beep(700, 70);
        vTaskDelay(pdMS_TO_TICKS(40));
        tone_beep(500, 70);
        vTaskDelay(pdMS_TO_TICKS(40));
        tone_beep(700, 120);
        break;
    case TONE_EFFECT_BOOT:
        tone_beep(523, 90);
        tone_beep(659, 90);
        tone_beep(784, 160);
        break;
    case TONE_EFFECT_SHUTDOWN:
        tone_beep(784, 110);
        tone_beep(659, 110);
        tone_beep(523, 200);
        break;
    case TONE_EFFECT_ALARM:
        for (int i = 0; i < 3; i++) {
            tone_beep(880, 150);
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        break;
    }
}

/* 内置旋律 (音符: 频率 Hz / 时长 ms) */
static const tone_note_t s_theme_boot_up[] = {
    { 523, 120 }, { 659, 120 }, { 784, 120 }, { 1046, 240 }, { 0, 0 },
};
static const tone_note_t s_theme_twinkle[] = {
    /* 一闪一闪亮晶晶: C C G G A A G - F F E E D D C */
    { 523, 160 }, { 523, 160 }, { 784, 160 }, { 784, 160 }, { 880, 160 }, { 880, 160 }, { 784, 320 },
    { 698, 160 }, { 698, 160 }, { 659, 160 }, { 659, 160 }, { 587, 160 }, { 587, 160 }, { 523, 320 },
    { 0, 0 },
};
static const tone_note_t s_theme_birthday[] = {
    /* 祝你生日快乐: G G A G C B -  G G A G D C - */
    { 784, 150 }, { 784, 150 }, { 880, 150 }, { 784, 150 }, { 1046, 150 }, { 988, 300 },
    { 784, 150 }, { 784, 150 }, { 880, 150 }, { 784, 150 }, { 1174, 150 }, { 1046, 300 },
    { 0, 0 },
};

void tone_play_theme(tone_theme_t t) {
    if (!s_ready) return;
    switch (t) {
    case TONE_THEME_BOOT_UP:
        tone_play_melody(s_theme_boot_up, (int)(sizeof(s_theme_boot_up) / sizeof(s_theme_boot_up[0])));
        break;
    case TONE_THEME_TWINKLE:
        tone_play_melody(s_theme_twinkle, (int)(sizeof(s_theme_twinkle) / sizeof(s_theme_twinkle[0])));
        break;
    case TONE_THEME_BIRTHDAY:
        tone_play_melody(s_theme_birthday, (int)(sizeof(s_theme_birthday) / sizeof(s_theme_birthday[0])));
        break;
    }
}

typedef struct {
    const tone_note_t *notes;
    int count;
} melody_arg_t;

static void melody_task(void *arg) {
    melody_arg_t a = *(melody_arg_t *)arg;
    free(arg);
    for (int i = 0; i < a.count && a.notes[i].freq > 0; i++) {
        tone_beep(a.notes[i].freq, a.notes[i].ms);
        if (a.notes[i].ms < 300) vTaskDelay(pdMS_TO_TICKS(25)); /* 音间小间隔 */
    }
    vTaskDelete(NULL);
}

void tone_play_melody(const tone_note_t *notes, int count) {
    if (!s_ready || count <= 0 || !notes) return;
    melody_arg_t *a = (melody_arg_t *)malloc(sizeof(melody_arg_t));
    if (!a) return;
    a->notes = notes;
    a->count = count;
    xTaskCreate(melody_task, "tone_mel", 2048, a, 1, NULL);
}
