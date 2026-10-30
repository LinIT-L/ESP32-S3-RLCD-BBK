/**
 * morse_engine.c — 摩斯播放引擎实现 (自合成 PCM + 通知式等待)
 */
#include "morse_engine.h"
#include "morse_codec.h"
#include "os_notify.h"
#include "audio_player.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

#define TAG "MORSE"

#define MR_SR            44100
#define MR_SLICE_MS      40
#define MR_SLICE_FRAMES  (MR_SR / 1000 * MR_SLICE_MS)   /* 1764 帧 = 7056B */
#define MR_AMP           6000                            /* 与 tone_player 的 TONE_AMP 一致 */

#define MR_NOTIFY_STOP   0x1u

static mr_plan_t  s_plan;
static mr_el_t    s_els[MR_MAX_EL];
static int16_t    s_pcm[MR_SLICE_FRAMES * 2];            /* 立体声 L/R */

static volatile int  s_state = MRE_IDLE;
static volatile bool s_run;
static TaskHandle_t  s_task;
static int           s_el, s_n_el, s_total_ch, s_done_ch;
static uint32_t      s_el_off, s_el_len, s_phase, s_half;
static uint16_t      s_freq;
static volatile bool s_audio_ok = true;
static volatile bool s_muted;
static int           s_fail;
static volatile bool s_prime;

/* 生成一片 PCM. 返回实际生成的帧数 (0 = 序列已结束) */
static int gen_slice(int16_t *buf, int frames)
{
    int i = 0;
    while (i < frames) {
        if (s_el >= s_n_el) break;
        mr_el_t *e = &s_els[s_el];
        if (e->kind == MR_EL_END) break;

        if (s_el_len == 0) {                      /* 进入新元素 */
            s_el_len = (uint32_t)e->ms * MR_SR / 1000u;
            if (s_el_len == 0) s_el_len = 1;
            s_el_off = 0;
            if (e->kind == MR_EL_MARK) {
                s_half = (uint32_t)(MR_SR / (2u * s_freq));
                if (s_half == 0) s_half = 1;
                s_phase = 0;
            }
        }

        uint32_t remain = s_el_len - s_el_off;
        uint32_t n = (uint32_t)(frames - i);
        if (n > remain) n = remain;

        if (e->kind == MR_EL_MARK) {
            for (uint32_t k = 0; k < n; k++) {
                int16_t v = ((s_phase / s_half) & 1u) ? (int16_t)(-MR_AMP) : (int16_t)MR_AMP;
                if (++s_phase >= s_half * 2u) s_phase = 0;
                buf[(i + k) * 2]     = v;
                buf[(i + k) * 2 + 1] = v;
            }
        } else {
            memset(&buf[i * 2], 0, (size_t)n * 2 * sizeof(int16_t));
        }

        i += (int)n;
        s_el_off += n;
        if (s_el_off >= s_el_len) {
            if (e->kind == MR_EL_GAP_C && s_done_ch < s_total_ch) s_done_ch++;
            s_el++;
            s_el_len = 0;
            s_el_off = 0;
        }
    }
    /* 片段没填满且序列已结束: 余下补静音, 保证喂给 I2S 的每片都是完整时长 */
    if (i > 0 && i < frames)
        memset(&buf[i * 2], 0, (size_t)(frames - i) * 2 * sizeof(int16_t));
    return i;
}

static void mre_task(void *arg)
{
    (void)arg;
    while (s_run) {
        if (s_state != MRE_PLAY) {
            uint32_t bits = 0;
            xTaskNotifyWait(0, 0xFFFFFFFFu, &bits, pdMS_TO_TICKS(100));
            if (bits & MR_NOTIFY_STOP) break;
            continue;
        }

        if (s_prime) {                    /* 第一片前: 打开预填充闸门 */
            s_prime = false;
            os_notify_sound_flush();
            vTaskDelay(pdMS_TO_TICKS(25));
            if (!s_run || s_state != MRE_PLAY) continue;
        }

        int n = gen_slice(s_pcm, MR_SLICE_FRAMES);
        if (n <= 0) {                     /* 播完 */
            if (s_done_ch < s_total_ch) s_done_ch = s_total_ch;
            s_state = MRE_IDLE;
            continue;
        }

        if (!s_muted) {
            size_t w = os_notify_sound_feed(s_pcm, (size_t)n, MR_SR);
            if (w == 0) {
                if (++s_fail >= 3) {      /* 连续 3 片没吃进去 = 链路不可用 */
                    ESP_LOGW(TAG, "PCM 连续 3 片未被接收 ⇒ 判定音频不可用");
                    s_audio_ok = false;
                    s_state = MRE_IDLE;
                    continue;
                }
            } else {
                s_fail = 0;
            }
        }

        uint32_t bits = 0;
        if (xTaskNotifyWait(0, 0xFFFFFFFFu, &bits, pdMS_TO_TICKS(MR_SLICE_MS))) {
            if (bits & MR_NOTIFY_STOP) break;
        }
    }
    s_state = MRE_IDLE;
    s_task  = NULL;
    vTaskDelete(NULL);
}

int mre_play(const char *text, uint8_t wpm, uint16_t freq_hz)
{
    if (!text || !text[0]) return -1;

    mre_stop();                                   /* 先收干净上一段 */

    if (mr_build_plan(text, &s_plan) != 0) return -1;
    int n = mr_expand(&s_plan, wpm, s_els, MR_MAX_EL, NULL);
    if (n <= 1) return -2;                        /* 只有 END = 没有可播元素 */
    s_n_el   = n;
    s_el     = 0;
    s_el_len = 0;
    s_el_off = 0;
    s_total_ch = mr_char_count(&s_plan);
    s_done_ch  = 0;
    s_freq     = (freq_hz < 300) ? 300 : (freq_hz > 1500 ? 1500 : freq_hz);
    s_fail     = 0;
    s_audio_ok = true;
    s_muted    = audio_player_is_muted();
    s_prime    = true;
    s_run      = true;
    s_state    = MRE_PLAY;

    if (!s_task) {
        if (xTaskCreate(mre_task, "morse", 4096, NULL, 5, &s_task) != pdPASS) {
            s_task  = NULL;
            s_run   = false;
            s_state = MRE_IDLE;
            return -3;
        }
    } else {
        xTaskNotify(s_task, 0, eSetBits);
    }
    return 0;
}

void mre_stop(void)
{
    if (s_task) {
        s_run   = false;
        s_state = MRE_IDLE;
        xTaskNotify(s_task, MR_NOTIFY_STOP, eSetBits);
        for (int i = 0; i < 40 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
        if (s_task) {                              /* 兜底: 200ms 仍未退则强杀 */
            ESP_LOGW(TAG, "worker 200ms 未退出 ⇒ 强制删除");
            vTaskDelete(s_task);
            s_task = NULL;
        }
    }
    s_state = MRE_IDLE;
    os_notify_sound_flush();                       /* 清掉环形缓冲里未播的部分 */
}

bool mre_playing(void)  { return s_state == MRE_PLAY; }
int  mre_progress(void) { return s_done_ch; }
int  mre_total(void)    { return s_total_ch; }
bool mre_audio_ok(void) { return s_audio_ok; }
bool mre_muted(void)    { return s_muted; }
