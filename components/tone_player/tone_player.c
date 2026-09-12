/**
 * @file tone_player.c
 * @brief 简易音调播放器 (V1.0.70): PCM 合成 → I2S → 功放 → 喇叭
 *
 * V1.0.70 起改为"两板一固件"方案:
 *   之前用 LEDC PWM 方波 → GPIO6 → 功放 IN+ 飞线 (仅官方板 AAXS2005 模拟功放可用,
 *   且 V1.0.69 因 GPIO48 与震动 EN 冲突移脚到 GPIO6, 接线受限).
 *   现在统一: 合成音调 PCM → audio_player_feed_pcm() → I2S 输出.
 *     官方板: I2S → ES8311 → AAXS2005 功放
 *     自制板: I2S → NS4168 (I2S 数字输入功放) 直接放大
 *   两板同固件零判断, GPIO6 完全闲置 (释放给其它用途).
 *
 * 生成常见电子声音 (开机/关机/确认/错误/闹铃) 和简单旋律.
 * 用法:
 *   tone_player_init();              // 初始化 (幂等, 全局调用, 不绑定任何引擎)
 *   tone_play_effect(TONE_EFFECT_BOOT);
 *   tone_beep(880, 100);             // 单音 (阻塞 100ms)
 *   tone_play_melody(notes, n);      // 旋律 (后台任务非阻塞播放, notes 需为静态数组)
 */
#include "tone_player.h"
#include "audio_player.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdlib.h>

#define TAG "TONE"

/* PCM 采样率: 与 MP3/游戏音频同用 44100, feed_pcm 内部会重配 I2S */
#define TONE_SR      44100
/* 方波幅值 (int16), 避免过响削波 */
#define TONE_AMP     6000

static bool s_ready = false;

void tone_player_init(void) {
    if (s_ready) return;
    /* 无需硬件初始化: PCM 经 audio_player_feed_pcm 输出, 由 audio_player 管理 I2S/功放 */
    s_ready = true;
    ESP_LOGI(TAG, "音调播放器就绪 (PCM→I2S, 两板通用: ES8311/NS4168)");
}

bool tone_player_ready(void) { return s_ready; }

/* 合成方波 PCM (立体声, L/R 相同) 并喂给 I2S */
static void tone_feed(int freq_hz, int ms) {
    if (!s_ready || ms <= 0) return;
    int frames = TONE_SR * ms / 1000;
    if (frames <= 0) frames = 1;
    int16_t *buf = heap_caps_malloc((size_t)frames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!buf) return;
    int half = TONE_SR / (2 * freq_hz);   /* 半周期采样数 */
    if (half < 1) half = 1;
    for (int i = 0; i < frames; i++) {
        int16_t v = ((i / half) & 1) ? -TONE_AMP : TONE_AMP;
        buf[i * 2] = v;
        buf[i * 2 + 1] = v;
    }
    audio_player_feed_pcm(buf, (size_t)frames, TONE_SR);
    free(buf);
}

/* ============ 持续音 (tone_tone_on / tone_stop) ============
 * "按下持续响、松开停"语义: 后台任务循环合成方波 PCM 喂给 I2S,
 * 直到 tone_stop() 置 0 退出并 flush 清空环形缓冲. */
static TaskHandle_t s_cont_task = NULL;
static volatile int  s_cont_freq = 0;

void tone_stop(void) {
    if (!s_ready) return;
    if (s_cont_freq != 0 || s_cont_task) {
        s_cont_freq = 0;                       /* 让持续音任务退出 */
        for (int i = 0; i < 20 && s_cont_task; i++)
            vTaskDelay(pdMS_TO_TICKS(5));      /* 最多等 100ms */
    }
    audio_player_flush_pcm();   /* 清空环形缓冲, 切断尾音 */
}

static void tone_cont_task(void *arg) {
    (void)arg;
    /* 每块 50ms 立体声 PCM (PSRAM) */
    int frames = TONE_SR / 20;   /* 50ms @44100 */
    int16_t *buf = heap_caps_malloc((size_t)frames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) { s_cont_task = NULL; vTaskDelete(NULL); return; }
    while (s_cont_freq > 0) {
        int freq = s_cont_freq;
        int half = TONE_SR / (2 * freq);
        if (half < 1) half = 1;
        for (int i = 0; i < frames; i++) {
            int16_t v = ((i / half) & 1) ? -TONE_AMP : TONE_AMP;
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        audio_player_feed_pcm(buf, (size_t)frames, TONE_SR);
        vTaskDelay(pdMS_TO_TICKS(45));   /* 略短于块时长, 保持连续不断音 */
    }
    free(buf);
    s_cont_task = NULL;
    vTaskDelete(NULL);
}

void tone_tone_on(int freq_hz) {
    if (!s_ready) return;
    if (freq_hz < 40) freq_hz = 40;
    if (freq_hz > 20000) freq_hz = 20000;
    if (s_cont_freq == freq_hz && s_cont_task) return;  /* 同频持续中, 幂等 */
    tone_stop();
    s_cont_freq = freq_hz;
    /* V1.0.72: 栈 2048→4096 words. tone_cont 走 feed_pcm → audio_out_start
     * → es8311_start/set_sample_rate 的 I2C 深链路, 8KB 会栈溢出 panic (重启循环). */
    if (xTaskCreate(tone_cont_task, "tone_cont", 4096, NULL, 5, &s_cont_task) != pdPASS) {
        s_cont_freq = 0;
    }
}

void tone_beep(int freq_hz, int ms) {
    if (!s_ready || ms <= 0) return;
    if (freq_hz < 40) freq_hz = 40;
    if (freq_hz > 20000) freq_hz = 20000;
    tone_feed(freq_hz, ms);
    vTaskDelay(pdMS_TO_TICKS((uint32_t)ms));   /* 保持阻塞语义: 播放完再返回 */
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
    /* V1.0.72: 栈 2048→4096 words. 开机旋律首次 feed_pcm 触发 audio_out_start
     * (ES8311/NS4168 配置 + I2C 深链路), 8KB 栈溢出 → 开机无限重启. */
    xTaskCreate(melody_task, "tone_mel", 4096, a, 1, NULL);
}
