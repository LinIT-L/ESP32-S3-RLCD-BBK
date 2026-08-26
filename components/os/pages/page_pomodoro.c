/**
 * page_pomodoro.c — 番茄钟 页面模块 (P3 首例完整迁移).
 *
 * 从 menu_system.c 的 pomodoro_* 迁移, 自包含全屏页:
 *   - 工作/休息倒计时 + 进度条 + 阶段切换完成提醒音(和弦)+震动
 *   - 确认 = 开始/暂停切换, 返回 = 停止并退出
 *   - 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "ui_common.h"
#include "audio_player.h"
#include "vibrator.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TAG "POMO"

/* ============ 私有状态 ============ */
static bool     s_active = false;
static bool     s_work   = true;
static uint32_t s_end_ms   = 0;
static uint32_t s_total_ms = 0;
static uint32_t s_work_min  = 25;
static uint32_t s_rest_min  = 5;
static bool     s_reminder = true;   /* 完成提醒音+震动 */
static bool     s_paused   = false;
static uint32_t s_paused_left_ms = 0;

/* === 完成提醒音: 电子合成和弦 (C5 E5 G5 C6), 逐帧喂 PCM === */
#define POMO_SR        22050
#define POMO_RING_LEN  22050
#define POMO_NOTE_N    4
static bool     s_ring = false;
static uint32_t s_ring_pos = 0;
static const float POMO_NOTES[POMO_NOTE_N] = { 523.25f, 659.25f, 783.99f, 1046.50f };
static const float POMO_NOTE_LEN[POMO_NOTE_N] = { 0.16f, 0.16f, 0.16f, 0.55f };

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void ring_start(void) {
    if (!s_reminder) return;
    s_ring = true;
    s_ring_pos = 0;
    if (vibrator_ready()) vibrator_tap(300, 100);
}

static void ring_update(void) {
    if (!s_ring) return;
    int16_t buf[512 * 2];
    int guard = 0;
    while (s_ring && guard < 8) {
        uint32_t chunk = 512;
        if (s_ring_pos + chunk > POMO_RING_LEN)
            chunk = POMO_RING_LEN - s_ring_pos;
        uint32_t t0 = s_ring_pos;
        for (uint32_t i = 0; i < chunk; i++) {
            float tt = (float)(t0 + i) / POMO_SR;
            float acc = 0.0f;
            int note = -1;
            for (int k = 0; k < POMO_NOTE_N; k++) {
                if (tt < acc + POMO_NOTE_LEN[k]) { note = k; break; }
                acc += POMO_NOTE_LEN[k];
            }
            int16_t v = 0;
            if (note >= 0) {
                float lt = tt - acc;
                float f = POMO_NOTES[note];
                float env = expf(-lt * 5.0f / POMO_NOTE_LEN[note]);
                float s = (sinf(6.2831853f * f * tt) + 0.35f * sinf(12.56637f * f * tt)) * env;
                v = (int16_t)(s * 12000.0f);
            }
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        size_t wr = audio_player_feed_pcm(buf, chunk, POMO_SR);
        s_ring_pos += (uint32_t)wr;
        if (wr < chunk) break;
        if (s_ring_pos >= POMO_RING_LEN) {
            s_ring = false;
            audio_player_stop();
        }
        guard++;
    }
}

static void pomo_start(void) {
    s_work   = true;
    s_total_ms = s_work_min * 60 * 1000;
    s_end_ms  = now_ms() + s_total_ms;
    s_paused  = false;
    s_active  = true;
    ESP_LOGI(TAG, "番茄钟开始: 工作 %u 分钟", (unsigned)s_work_min);
}

static void pomo_stop(void) {
    s_active = false;
    s_paused = false;
    s_ring = false;
    audio_player_flush_pcm();
}

static void pomo_tick(void) {
    if (!s_active || s_paused) return;
    uint32_t now = now_ms();
    if ((int32_t)(s_end_ms - now) > 0) return;
    /* 阶段切换: 工作 <-> 休息 */
    s_work = !s_work;
    s_total_ms = (s_work ? s_work_min : s_rest_min) * 60 * 1000;
    s_end_ms = now + s_total_ms;
    ESP_LOGI(TAG, "番茄钟切换到 %s", s_work ? "工作" : "休息");
    ring_start();
}

/* ============ 渲染 ============ */
static void p_pomodoro_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    pomo_tick();
    ring_update();

    st7305_clear(lcd, ST7305_COLOR_WHITE);
    uint32_t now = now_ms();
    int64_t remain_ms = (int64_t)s_end_ms - (int64_t)now;
    if (remain_ms < 0) remain_ms = 0;
    int64_t remain_s = (remain_ms + 999) / 1000;
    char buf[32];

    /* 状态行 */
    const char *phase = !s_active ? "\xe6\x9c\xaa\xe5\xbc\x80\xe5\xa7\x8b"            /* 未开始 */
                     : (s_work ? "\xe5\xb7\xa5\xe4\xbd\x9c\xe4\xb8\xad" : "\xe4\xbc\x91\xe6\x81\xaf\xe4\xb8\xad"); /* 工作中/休息中 */
    snprintf(buf, sizeof(buf), "%s %u\xe5\x88\x86\xe9\x92\x9f", phase,
             (unsigned)(s_active ? (s_work ? s_work_min : s_rest_min) : s_work_min));
    draw_text_centered(lcd, 70, buf, false);
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)(remain_s / 60), (int)(remain_s % 60));
    draw_text_centered(lcd, 130, buf, false);

    /* 进度条 */
    int bar_x = 50, bar_y = 200, bar_w = 300, bar_h = 16;
    for (int yy = 0; yy < bar_h; yy++) {
        st7305_draw_pixel(lcd, bar_x, bar_y + yy, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, bar_x + bar_w - 1, bar_y + yy, ST7305_COLOR_BLACK);
    }
    for (int xx = 0; xx < bar_w; xx++) {
        st7305_draw_pixel(lcd, bar_x + xx, bar_y, ST7305_COLOR_BLACK);
        st7305_draw_pixel(lcd, bar_x + xx, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    }
    uint32_t done = (s_total_ms > remain_ms) ? (s_total_ms - remain_ms) : 0;
    int fill = (int)((bar_w - 4) * (s_total_ms ? done / (float)s_total_ms : 0));
    for (int yy = 2; yy < bar_h - 2; yy++)
        for (int xx = 0; xx < fill; xx++)
            st7305_draw_pixel(lcd, bar_x + 2 + xx, bar_y + yy, ST7305_COLOR_BLACK);

    draw_text_centered(lcd, 240, "\xe7\xa1\xae\xe8\xae\xa4\xe5\xbc\x80\xe5\xa7\x8b/\xe6\x9a\x82\xe5\x81\x9c \xe8\xbf\x94\xe5\x9b\x9e\xe9\x80\x80\xe5\x87\xba", false); /* 确认开始/暂停 返回退出 */
}

/* ============ 按键 ============ */
static void p_pomodoro_action(ui_ctx_t *ctx, os_action_t a)
{
    switch (a) {
    case OS_ACTION_CONFIRM:
        if (!s_active) {
            pomo_start();
        } else if (s_paused) {
            s_paused = false;
            s_end_ms = now_ms() + s_paused_left_ms;
        } else {
            s_paused = true;
            s_paused_left_ms = (s_end_ms > now_ms()) ? (s_end_ms - now_ms()) : 0;
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_BACK:
    case OS_ACTION_HOME:
        pomo_stop();
        os_pop(ctx);
        break;
    default:
        break;
    }
}

/* ============ 每帧轮询: 倒计时/提醒音驱动 + 定时重绘 ============ */
static int s_last_second = -1;
static void p_pomodoro_poll(ui_ctx_t *ctx)
{
    pomo_tick();       /* 阶段切换检测 (即使未重绘也须推进) */
    ring_update();     /* 完成提醒音逐帧喂 PCM */
    if (s_active || s_ring) {
        uint32_t now = now_ms();
        int sec = (int)((s_end_ms > now ? s_end_ms - now : 0) / 1000);
        if (sec != s_last_second || s_ring) {
            s_last_second = sec;
            ctx->needs_redraw = true;
        }
    } else if (s_last_second != -1) {
        s_last_second = -1;
        ctx->needs_redraw = true;
    }
}

/* ============ 模块契约 ============ */
static const os_module_t s_mod_pomodoro = {
    .name      = "pomodoro",
    .page_id   = OS_PAGE_POMODORO,
    .render    = p_pomodoro_render,
    .action    = p_pomodoro_action,
    .poll      = p_pomodoro_poll,
    .fullscreen = true,
};

void os_page_pomodoro_register(void) { os_register(&s_mod_pomodoro); }
