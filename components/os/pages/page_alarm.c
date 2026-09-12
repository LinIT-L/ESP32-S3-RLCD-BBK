/**
 * page_alarm.c — 闹钟 页面模块.
 *
 * 用系统时间(time/localtime)判断到点: 设定时/分, 到点触发震动+响铃.
 * 状态: 显示当前时间 + 闹钟时间, 上下调时/分, 确认开/关闹钟, 返回退出.
 */
#include "os.h"
#include "ui_common.h"
#include "audio_player.h"
#include "vibrator.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TAG "ALM"

#define ALM_SR       22050
#define ALM_RING_LEN (22050 * 2)

/* ==== 私有状态 ==== */
static bool     s_enabled = false;
static int      s_hour = 7, s_min = 0;
static int      s_field = 0;          /* 0=小时 1=分钟 */
static bool     s_ringing = false;
static uint32_t s_ring_pos = 0;
static bool     s_prev_fired = false;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 掌机复古: 大号两位数字字段 (draw_ascii_medium 12x18). sel=黑块反色 */
static void alm_field(st7305_handle_t *lcd, int cx, int y, int val, bool sel) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02d", val);
    int total_w = 12 * 2 + 4;
    int x0 = cx - total_w / 2;
    if (sel) fill_rect(lcd, x0 - 8, y - 2, x0 - 8 + total_w + 16, y + 19, ST7305_COLOR_BLACK);
    for (int i = 0; i < 2; i++) draw_ascii_medium(lcd, x0 + i * 14, y, buf[i], sel);
}
/* 掌机复古: 顶部黑色信息条 (左标题 / 右状态) */
static void alm_bar(st7305_handle_t *lcd, const char *title, const char *status) {
    fill_rect(lcd, 0, 0, 399, 24, ST7305_COLOR_BLACK);
    int tw = text_width(title);
    draw_text(lcd, 6, 4, title, true);
    int sw = text_width(status);
    draw_text(lcd, 400 - 6 - sw, 4, status, true);
}

/* "嘀-"单音提醒 (C6, 0.3s/声, 间隔0.3s, 共2秒) */
static void ring_start(void) {
    if (s_ringing) return;
    s_ringing = true;
    s_ring_pos = 0;
    /* V1.4.x: 闹钟震动改无限循环花样 (300ms 强震 + 180ms 停), 关铃时显式停止 */
    if (vibrator_ready()) vibrator_play_pattern(&VIB_PAT_ALARM);
    ESP_LOGI(TAG, "闹钟响铃!");
}

static void ring_update(void) {
    if (!s_ringing) return;
    int16_t buf[512 * 2];
    uint32_t chunk = 512;
    if (s_ring_pos + chunk > ALM_RING_LEN) chunk = ALM_RING_LEN - s_ring_pos;
    for (uint32_t i = 0; i < chunk; i++) {
        uint32_t idx = s_ring_pos + i;
        float on = fmodf((float)idx / ALM_SR, 0.6f);
        float v = (on < 0.3f) ? sinf(6.2831853f * 1046.50f * (float)idx / ALM_SR) : 0.0f;
        int16_t iv = (int16_t)(v * 12000.0f);
        buf[i * 2] = iv; buf[i * 2 + 1] = iv;
    }
    size_t wr = audio_player_feed_pcm(buf, chunk, ALM_SR);
    s_ring_pos += (uint32_t)wr;
    if (s_ring_pos >= ALM_RING_LEN && wr >= chunk) {
        s_ringing = false;
        vibrator_stop_pattern();   /* 闹铃音自然结束: 停无限震动花样 */
        audio_player_stop();
    }
}

static void alarm_check(void) {
    if (!s_enabled || s_ringing) return;
    time_t n = time(NULL);
    struct tm *t = localtime(&n);
    if (!t) return;
    int cur = t->tm_hour * 60 + t->tm_min;
    int set = s_hour * 60 + s_min;
    if (cur == set && !s_prev_fired) ring_start();
    s_prev_fired = (cur == set);
}

/* ==== 渲染 ==== */
static void p_alarm_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    alarm_check();
    ring_update();
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    /* 顶部黑色信息条: 左"闹钟" 右 开关状态 */
    const char *st = s_ringing ? "\xe5\x93\x8d\xe9\x93\x83\xe4\xb8\xad"  /* 响铃中 */
                    : (s_enabled ? "\xe5\xb7\xb2\xe5\xbc\x80\xe5\x90\xaf" : "\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad"); /* 已开启/已关闭 */
    alm_bar(lcd, "\xe9\x97\xb9\xe9\x92\x9f", st);   /* 闹钟 */

    /* 当前时间 (小字) */
    char buf[48];
    time_t n = time(NULL);
    struct tm *t = localtime(&n);
    snprintf(buf, sizeof(buf), "\xe7\x8e\xb0\xe5\x9c\xa8\x20\x25\x30\x32\x64\x3a\x25\x30\x32\x64\x3a\x25\x30\x32\x64",
             t ? t->tm_hour : 0, t ? t->tm_min : 0, t ? t->tm_sec : 0);
    draw_text_centered(lcd, 52, buf, false);

    /* 大号闹钟时间字段: 时 / 分 */
    draw_text_centered(lcd, 108, "\xe6\x8c\xa4\xe6\x97\xb6\xe9\x97\xb4", false);   /* 闹钟时间标签 */
    alm_field(lcd, 170, 150, s_hour, s_field == 0);
    /* 时分分隔冒号 (大号) */
    draw_ascii_medium(lcd, 190, 148, ':', false);
    alm_field(lcd, 232, 150, s_min, s_field == 1);

    /* 底部操作提示条 (掌机底栏) */
    draw_hline(lcd, 0, 399, 246, ST7305_COLOR_BLACK);
    draw_text_centered(lcd, 254, "↑↓: 调整    ←→: 切换", false);
    const char *act = s_ringing ? "确定: 关铃    BACK: 返回"
                                : "确定: 开关    BACK: 返回";
    draw_text_centered(lcd, 276, act, false);
}

/* ==== 按键 ==== */
static void p_alarm_action(ui_ctx_t *ctx, os_action_t a) {
    switch (a) {
    case OS_ACTION_UP:
        if (s_field == 0) s_hour = (s_hour + 1) % 24;
        else s_min = (s_min + 1) % 60;
        break;
    case OS_ACTION_DOWN:
        if (s_field == 0) s_hour = (s_hour + 23) % 24;
        else s_min = (s_min + 59) % 60;
        break;
    case OS_ACTION_LEFT: s_field = 0; break;
    case OS_ACTION_RIGHT: s_field = 1; break;
    case OS_ACTION_CONFIRM:
        if (s_ringing) { s_ringing = false; vibrator_stop_pattern(); audio_player_stop(); }
        else s_enabled = !s_enabled;
        break;
    case OS_ACTION_BACK:
    case OS_ACTION_HOME:
        if (s_ringing) { s_ringing = false; vibrator_stop_pattern(); audio_player_stop(); }
        os_pop(ctx);
        break;
    default: break;
    }
    ctx->needs_redraw = true;
}

static void p_alarm_poll(ui_ctx_t *ctx) {
    alarm_check();
    ring_update();
    if (s_ringing) { ctx->needs_redraw = true; return; }
    static int last_min = -1;
    time_t n = time(NULL);
    struct tm *t = localtime(&n);
    int m = t ? (t->tm_hour * 60 + t->tm_min) : -1;
    if (m != last_min) { last_min = m; ctx->needs_redraw = true; }
}

/* ==== 触控 (两者兼顾): 点 时/分 字段选字段, 上/下滑 调整数值 ==== */
static int s_alm_ty0 = -1;
static bool p_alarm_touch(ui_ctx_t *ctx, int x, int y) {
    if (y < 140 || y > 172) return false;      /* 仅时间字段区 */
    if (x < 190) { if (s_field != 0) { s_field = 0; } }
    else         { if (s_field != 1) { s_field = 1; } }
    if (s_alm_ty0 < 0) { s_alm_ty0 = y; ctx->needs_redraw = true; return true; }
    int dy = y - s_alm_ty0;
    if (dy >= 16)  { s_alm_ty0 = y; if (s_field == 0) s_hour = (s_hour + 1) % 24; else s_min = (s_min + 1) % 60; }
    else if (dy <= -16) { s_alm_ty0 = y; if (s_field == 0) s_hour = (s_hour + 23) % 24; else s_min = (s_min + 59) % 60; }
    ctx->needs_redraw = true;
    return true;
}

/* ==== 模块契约 ==== */
static const os_module_t s_mod_alarm = {
    .name      = "alarm",
    .page_id   = OS_PAGE_PLACEHOLDER_ALARM,
    .render    = p_alarm_render,
    .action    = p_alarm_action,
    .poll      = p_alarm_poll,
    .touch     = p_alarm_touch,
    .fullscreen = true,
};

void os_page_alarm_register(void) { os_register(&s_mod_alarm); }