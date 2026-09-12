/**
 * page_pomodoro.c — 番茄钟 页面模块 (滚轮版 UI).
 *
 * UI 参考「日期更改」滚轮样式:
 *   - 工作时间 / 休息时间 → 单列滚轮选择 (5 分钟步进, 上下拖动/UP-DOWN, 底部确认/取消)
 *   - 主菜单 5 项 (一屏铺满): 工作时间 / 休息时间 / 开始番茄钟 / 声音开关 / 震动开关
 *   - 底部固定行由"返回"改为"退出", 点它或按 BACK → 直接退回上一级 (弹窗列表菜单, 无退出确认框)
 *   - 声音与震动为两个独立开关 (不再合并成"提醒设置")
 *   - 弹窗标题留空 + 底层不再绘制"番茄钟"标题文字 (弹窗态纯白底)
 *   - 开始 → 全屏倒计时: 环形进度 + 大号 MM:SS + "按任意键返回"
 *   - 完成提醒: 电子合成和弦 (C5 E5 G5 C6) 受声音开关控制, 震动受震动开关控制
 * 设置持久化到 NVS (namespace "pomodoro").
 */
#include "os.h"
#include "input.h"
#include "ui_common.h"
#include "audio_player.h"
#include "vibrator.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TAG "POMO"

#define POMO_NVS_NS   "pomodoro"
#define POMO_NVS_WORK "work_min"
#define POMO_NVS_REST "rest_min"
#define POMO_NVS_SND  "snd"
#define POMO_NVS_VIB  "vib"
#define POMO_NVS_REM  "reminder"   /* 旧版合并开关, 仅用于首次迁移 */

/* 时间档位: 5 分钟步进 */
#define POMO_STEP_MIN  5
#define POMO_WORK_MAX  120
#define POMO_REST_MAX  60
#define POMO_WORK_N    (POMO_WORK_MAX / POMO_STEP_MIN)   /* 24 档: 5..120 */
#define POMO_REST_N    (POMO_REST_MAX / POMO_STEP_MIN)   /* 12 档: 5..60  */
#define POMO_IDX_MIN(idx) ((int)(((idx) + 1) * POMO_STEP_MIN))

/* 主菜单项 (5 项, 正好占满一屏不滚动) */
#define POMO_MAIN_N     5
#define POMO_M_WORK     0   /* 工作时间 → 滚轮 */
#define POMO_M_REST     1   /* 休息时间 → 滚轮 */
#define POMO_M_START    2   /* 开始番茄钟 */
#define POMO_M_SOUND    3   /* 声音开关 */
#define POMO_M_VIB      4   /* 震动开关 */

/* ============ 私有状态 ============ */
static bool     s_active = false;     /* 倒计时运行中 (全屏) */
static bool     s_work   = true;      /* true=工作 false=休息 */
static uint32_t s_end_ms   = 0;       /* 本阶段结束时刻 (esp_timer ms) */
static uint32_t s_total_ms = 0;
static int      s_work_idx = 4;       /* 工作档位 (默认 25 分钟) */
static int      s_rest_idx = 0;       /* 休息档位 (默认 5 分钟) */
static bool     s_sound = true;       /* 完成提醒: 声音 */
static bool     s_vib   = true;       /* 完成提醒: 震动 */

/* === 完成提醒音: 电子合成和弦 (C5 E5 G5 C6), 逐帧喂 PCM === */
#define POMO_SR        22050
#define POMO_RING_LEN  22050
#define POMO_NOTE_N    4
static bool     s_ring = false;
static uint32_t s_ring_pos = 0;
static const float POMO_NOTES[POMO_NOTE_N] = { 523.25f, 659.25f, 783.99f, 1046.50f };
static const float POMO_NOTE_LEN[POMO_NOTE_N] = { 0.16f, 0.16f, 0.16f, 0.55f };

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- NVS 持久化 ---- */
static void pomo_cfg_load(void) {
    nvs_handle_t h;
    if (nvs_open(POMO_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint32_t v;
    if (nvs_get_u32(h, POMO_NVS_WORK, &v) == ESP_OK) {
        int idx = ((int)v / POMO_STEP_MIN) - 1;
        if (idx >= 0 && idx < POMO_WORK_N) s_work_idx = idx;
    }
    if (nvs_get_u32(h, POMO_NVS_REST, &v) == ESP_OK) {
        int idx = ((int)v / POMO_STEP_MIN) - 1;
        if (idx >= 0 && idx < POMO_REST_N) s_rest_idx = idx;
    }
    if (nvs_get_u32(h, POMO_NVS_SND, &v) == ESP_OK) s_sound = (v != 0);
    else if (nvs_get_u32(h, POMO_NVS_REM, &v) == ESP_OK) s_sound = (v != 0);  /* 旧值迁移 */
    if (nvs_get_u32(h, POMO_NVS_VIB, &v) == ESP_OK) s_vib = (v != 0);
    else if (nvs_get_u32(h, POMO_NVS_REM, &v) == ESP_OK) s_vib = (v != 0);    /* 旧值迁移 */
    nvs_close(h);
}
static void pomo_cfg_save(void) {
    nvs_handle_t h;
    if (nvs_open(POMO_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, POMO_NVS_WORK, (uint32_t)POMO_IDX_MIN(s_work_idx));
        nvs_set_u32(h, POMO_NVS_REST, (uint32_t)POMO_IDX_MIN(s_rest_idx));
        nvs_set_u32(h, POMO_NVS_SND,  s_sound ? 1 : 0);
        nvs_set_u32(h, POMO_NVS_VIB,  s_vib   ? 1 : 0);
        nvs_set_u32(h, POMO_NVS_REM,  (s_sound || s_vib) ? 1 : 0);   /* 兼容旧固件 */
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 阶段完成提醒: 声音与震动各自独立受控 */
static void ring_start(void) {
    if (s_sound) {
        s_ring = true;
        s_ring_pos = 0;
    }
    /* V1.4.x: 完成提醒改"成功三连"花样 (80+70+80+70+90ms 渐强) */
    if (s_vib && vibrator_ready()) vibrator_play_pattern(&VIB_PAT_SUCCESS);
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
    s_work = true;
    s_total_ms = (uint32_t)POMO_IDX_MIN(s_work_idx) * 60 * 1000;
    s_end_ms = now_ms() + s_total_ms;
    s_active = true;
    ESP_LOGI(TAG, "番茄钟开始: 工作 %d 分钟", POMO_IDX_MIN(s_work_idx));
}
static void pomo_stop(void) {
    s_active = false;
    s_ring = false;
    audio_player_flush_pcm();
}
/* 阶段切换 (工作↔休息 循环) */
static void pomo_tick(void) {
    if (!s_active) return;
    uint32_t now = now_ms();
    if ((int32_t)(s_end_ms - now) > 0) return;
    s_work = !s_work;
    s_total_ms = (uint32_t)(s_work ? POMO_IDX_MIN(s_work_idx) : POMO_IDX_MIN(s_rest_idx)) * 60 * 1000;
    s_end_ms = now + s_total_ms;
    ESP_LOGI(TAG, "番茄钟切换到 %s", s_work ? "工作" : "休息");
    ring_start();
}

/* ============ 大号 ASCII 数字 (8x12 点阵整数倍放大) ============ */
static void big_ascii(st7305_handle_t *lcd, int x, int y, char c, int scale, bool inv) {
    int idx = (c >= 0x20 && c <= 0x7E) ? (c - 0x20) : ('?' - 0x20);
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    fill_rect(lcd, x, y, x + 8 * scale - 1, y + 16 * scale - 1, bg);
    const uint8_t *bmp = FONT8X12[idx];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1 << (7 - col)))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    st7305_draw_pixel(lcd, x + col * scale + dx, y + row * scale + dy, fg);
        }
    }
}
/* 居中大号字符串 (仅 ASCII) */
static void big_text_centered(st7305_handle_t *lcd, int cx, int y, const char *s, int scale) {
    int n = (int)strlen(s);
    int w = n * 8 * scale;
    int x = cx - w / 2;
    for (int i = 0; i < n; i++) big_ascii(lcd, x + i * 8 * scale, y, s[i], scale, false);
}

/* 环形进度: 底圈虚线 + 已过部分实线 (半径 r_in..r_out) */
static void ring_progress(st7305_handle_t *lcd, int cx, int cy, int r_in, int r_out, float prog) {
    const int N = 360;
    int done = (int)(prog * N);
    if (done < 0) done = 0;
    if (done > N) done = N;
    for (int d = 0; d < N; d++) {
        bool filled = (d < done);
        if (!filled && (d % 5 != 0)) continue;          /* 未走完部分: 稀疏点圈 */
        float rad = (d - 90) * 3.14159265f / 180.0f;
        float ca = cosf(rad), sa = sinf(rad);
        for (int r = r_in; r <= r_out; r++) {
            int x = cx + (int)(ca * r + (ca >= 0 ? 0.5f : -0.5f));
            int y = cy + (int)(sa * r + (sa >= 0 ? 0.5f : -0.5f));
            st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
        }
    }
}

/* ============ 全屏倒计时渲染 ============ */
static void pomo_full_render(st7305_handle_t *lcd) {
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    uint32_t now = now_ms();
    int64_t remain_ms = (int64_t)s_end_ms - (int64_t)now;
    if (remain_ms < 0) remain_ms = 0;
    int64_t remain_s = (remain_ms + 999) / 1000;

    /* 顶部: 阶段 + 本阶段总时长 */
    char buf[32];
    snprintf(buf, sizeof(buf), "%s  %d分钟", s_work ? "工作中" : "休息中",
             (int)(s_total_ms / 60000));
    draw_text_centered(lcd, 34, buf, false);

    /* 中部: 环形进度 + 环内大号 MM:SS */
    const int cx = UI_SCREEN_W / 2, cy = 148, r_out = 86, r_in = 74;
    uint32_t done = (s_total_ms > (uint32_t)remain_ms) ? (s_total_ms - (uint32_t)remain_ms) : 0;
    float prog = s_total_ms ? (done / (float)s_total_ms) : 0.0f;
    ring_progress(lcd, cx, cy, r_in, r_out, prog);
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)(remain_s / 60), (int)(remain_s % 60));
    big_text_centered(lcd, cx, cy - 18, buf, 3);

    /* 底部提示 */
    draw_text_centered(lcd, 262, "按任意键返回", false);
    st7305_flush(lcd);
}

/* ============ 滚轮选择弹窗 (参考「日期更改」滚轮) ============ */
#define POMO_WHEEL_W      350
#define POMO_WHEEL_H      250
#define POMO_WHEEL_STEP_H 30
#define POMO_WHEEL_SIDE   2
static bool s_wheel_work = true;   /* 当前滚轮编辑对象: true=工作时间 */
static int  s_wheel_idx  = 0;      /* 草稿档位 (未确认不落盘) */

static int  wheel_n(void) { return s_wheel_work ? POMO_WORK_N : POMO_REST_N; }

/* 当前档位 +steps 步的显示文本 (越界返回空串 = 不画) */
static void wheel_text(int steps, char *out, size_t n) {
    int idx = s_wheel_idx + steps;
    if (idx < 0 || idx >= wheel_n()) { out[0] = '\0'; return; }
    snprintf(out, n, "%d分钟", POMO_IDX_MIN(idx));
}
static void wheel_adjust(int delta) {
    int nb = s_wheel_idx + delta;
    if (nb < 0) nb = 0;
    if (nb >= wheel_n()) nb = wheel_n() - 1;
    s_wheel_idx = nb;
}
/* 确认: 草稿落盘 + 返回主菜单并同步该行文本 */
static void wheel_confirm(ui_ctx_t *ctx) {
    if (s_wheel_work) s_work_idx = s_wheel_idx;
    else              s_rest_idx = s_wheel_idx;
    pomo_cfg_save();
    os_dialog_pop(ctx);   /* 回主菜单 (已接管, 不会连关) */
    os_dlg_stack_t *top = os_dialog_top_mut(ctx);
    if (top && top->count >= 6) {
        if (s_wheel_work)
            snprintf(top->items[0], sizeof(top->items[0]), "工作时间: %d分钟",
                     POMO_IDX_MIN(s_work_idx));
        else
            snprintf(top->items[1], sizeof(top->items[1]), "休息时间: %d分钟",
                     POMO_IDX_MIN(s_rest_idx));
    }
}

static bool pomo_wheel_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    const int W = POMO_WHEEL_W, H = POMO_WHEEL_H;
    int bx = (ui_screen_w() - W) / 2, by = (ui_screen_h() - H) / 2;
    const int bbt = by + H - 2 - 44;   /* 底部确认/取消按钮区顶 */
    fill_rect(lcd, bx, by, bx + W - 1, by + H - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, bx + k, bx + W - 1 - k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + W - 1 - k, by + k, by + H - 1 - k, ST7305_COLOR_BLACK);
    }
    /* 标题: 工作时间 / 休息时间 */
    draw_text_centered(lcd, by + 14, s_wheel_work ? "工作时间" : "休息时间", false);

    /* 内容区 (标题以下、按钮区以上) 垂直居中 */
    const int top = by + 40;
    const int cy = top + (bbt - top) / 2;
    const int y0 = cy - 13;   /* 当前值文字顶 (字形约 26px) */
    const int ccx = bx + W / 2;
    for (int k = -POMO_WHEEL_SIDE; k <= POMO_WHEEL_SIDE; k++) {
        char vs[24];
        wheel_text(k, vs, sizeof(vs));
        if (!vs[0]) continue;
        int vw = text_width(vs);
        int vcx = ccx - vw / 2;
        if (k == 0) {
            /* 当前值: 黑底反色 (与日期滚轮同一观感) */
            fill_rect(lcd, vcx - 5, y0 - 3, vcx + vw + 4, y0 + 25, ST7305_COLOR_BLACK);
            draw_text(lcd, vcx, y0, vs, true);
        } else {
            draw_text(lcd, vcx, y0 + k * POMO_WHEEL_STEP_H, vs, false);
        }
    }
    /* 底部 确认 / 取消 */
    draw_hline(lcd, bx + 2, bx + W - 1 - 2, bbt, ST7305_COLOR_BLACK);
    int mid = bx + W / 2;
    draw_vline(lcd, mid, bbt, by + H - 1 - 2, ST7305_COLOR_BLACK);
    int ty = bbt + 10;
    draw_text(lcd, bx + W / 4 - text_width("确认") / 2, ty, "确认", false);
    draw_text(lcd, bx + W * 3 / 4 - text_width("取消") / 2, ty, "取消", false);
    return true;
}

static bool pomo_wheel_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)d; (void)ud;
    switch (a) {
    case OS_ACTION_UP:      wheel_adjust(+1); ctx->needs_redraw = true; return true;
    case OS_ACTION_DOWN:    wheel_adjust(-1); ctx->needs_redraw = true; return true;
    case OS_ACTION_CONFIRM: wheel_confirm(ctx); return true;
    default: return false;   /* BACK → 默认取消回主菜单 */
    }
}

static void pomo_wheel_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    /* 上下拖动选值: 累积余差, 每满一行调一档 (边界夹紧, 不越界) */
    static int s_t_last = -1, s_t_acc = 0;
    int tx, ty;
    if (input_get_touch_pos(&tx, &ty)) {
        if (s_t_last < 0) {
            s_t_last = ty;
            s_t_acc = 0;
        } else {
            s_t_acc += ty - s_t_last;
            s_t_last = ty;
            bool changed = false;
            while (s_t_acc <= -POMO_WHEEL_STEP_H) { wheel_adjust(+1); s_t_acc += POMO_WHEEL_STEP_H; changed = true; }
            while (s_t_acc >= +POMO_WHEEL_STEP_H) { wheel_adjust(-1); s_t_acc -= POMO_WHEEL_STEP_H; changed = true; }
            if (changed) ctx->needs_redraw = true;
        }
    } else {
        s_t_last = -1;
        s_t_acc = 0;
    }
}

/* 触摸: 底部左=确认 右=取消, 内容区吞掉不外泄 (拖动由 poll 处理) */
static bool pomo_wheel_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)d; (void)ud;
    const int W = POMO_WHEEL_W, H = POMO_WHEEL_H;
    int bx = (ui_screen_w() - W) / 2, by = (ui_screen_h() - H) / 2;
    if (x < bx || x > bx + W - 1 || y < by || y > by + H - 1) return false;
    const int bbt = by + H - 2 - 44;
    if (y >= bbt) {
        int mid = bx + W / 2;
        if (x < mid) wheel_confirm(ctx);   /* 确认 */
        else         os_dialog_pop(ctx);   /* 取消 */
        ctx->needs_redraw = true;
    }
    return true;   /* 弹窗内其他区域吞掉, 不外泄到下层 */
}

static void pomo_wheel_open(ui_ctx_t *ctx, bool is_work) {
    s_wheel_work = is_work;
    s_wheel_idx  = is_work ? s_work_idx : s_rest_idx;
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.no_footer = true;
    dlg.on_render = pomo_wheel_render;
    dlg.on_key    = pomo_wheel_key;
    dlg.on_poll   = pomo_wheel_poll;
    dlg.on_touch  = pomo_wheel_touch;
    os_dialog_push(ctx, &dlg);
}

/* ============ 主菜单弹窗 ============ */
static void pomo_main_on_select(ui_ctx_t *ctx, int result, void *ud);   /* 前置声明 */

/* 主菜单: 5 项铺满一屏, 底部固定行文字为"退出" (点它/按 BACK → 直接退回上一级, 无确认框) */
static void pomo_open_main(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = POMO_MAIN_N;
    snprintf(dlg.items[POMO_M_WORK],  sizeof(dlg.items[0]), "工作时间: %d分钟", POMO_IDX_MIN(s_work_idx));
    snprintf(dlg.items[POMO_M_REST],  sizeof(dlg.items[0]), "休息时间: %d分钟", POMO_IDX_MIN(s_rest_idx));
    snprintf(dlg.items[POMO_M_START], sizeof(dlg.items[0]), "开始番茄钟");
    snprintf(dlg.items[POMO_M_SOUND], sizeof(dlg.items[0]), "声音: %s", s_sound ? "开" : "关");
    snprintf(dlg.items[POMO_M_VIB],   sizeof(dlg.items[0]), "震动: %s", s_vib   ? "开" : "关");
    snprintf(dlg.footer, sizeof(dlg.footer), "退出");
    dlg.sel = 0;
    dlg.cb  = pomo_main_on_select;
    os_dialog_push(ctx, &dlg);
}

/* 主菜单选择分发: 底部"退出"行 / BACK (result<0) 直接退回上一级, 不再弹确认框 */
static void pomo_main_on_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    switch (result) {
    case POMO_M_WORK:  pomo_wheel_open(ctx, true);  break;   /* 工作时间 → 滚轮 */
    case POMO_M_REST:  pomo_wheel_open(ctx, false); break;   /* 休息时间 → 滚轮 */
    case POMO_M_START:   /* 开始番茄钟 → 关掉弹窗, 全屏倒计时接管 */
        os_dialog_clear_all(ctx);
        pomo_start();
        break;
    case POMO_M_SOUND:   /* 声音开关: 原位刷新本行, 不关窗 */
        s_sound = !s_sound;
        pomo_cfg_save();
        {
            os_dlg_stack_t *t = os_dialog_top_mut(ctx);
            if (t) snprintf(t->items[POMO_M_SOUND], sizeof(t->items[0]),
                            "声音: %s", s_sound ? "开" : "关");
            os_dialog_mark_handled(ctx);
        }
        break;
    case POMO_M_VIB:   /* 震动开关: 原位刷新本行, 不关窗 */
        s_vib = !s_vib;
        pomo_cfg_save();
        {
            os_dlg_stack_t *t = os_dialog_top_mut(ctx);
            if (t) snprintf(t->items[POMO_M_VIB], sizeof(t->items[0]),
                            "震动: %s", s_vib ? "开" : "关");
            os_dialog_mark_handled(ctx);
        }
        break;
    default:   /* result < 0: 底部"退出"行 / BACK → 直接退回上一级 (无确认框, 保留原位置) */
        os_dialog_pop(ctx);   /* 关主菜单弹窗, 页面 poll 检测弹窗栈空后自动 os_pop 回上一级 */
        break;
    }
}

/* ============ 页面渲染 ============ */
static void p_pomodoro_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    pomo_tick();
    ring_update();
    /* 倒计时运行中 → 全屏覆盖 (不透出弹窗) */
    if (s_active) {
        pomo_full_render(lcd);
        return;
    }
    /* 设置态: 底层纯白, 只留弹窗浮在上面 (不再绘制"番茄钟"标题) */
    st7305_clear(lcd, ST7305_COLOR_WHITE);
}

/* ============ 按键 ============ */
static void p_pomodoro_action(ui_ctx_t *ctx, os_action_t a) {
    /* 倒计时运行中: 按任意键停止并回到主菜单 */
    if (s_active) {
        pomo_stop();
        pomo_open_main(ctx);
        ctx->needs_redraw = true;
        return;
    }
    /* 设置弹窗态: BACK 退出页面 (弹窗在时交给弹窗层处理) */
    if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) {
        if (os_dialog_depth() > 0) return;
        os_pop(ctx);
        return;
    }
}

/* ============ 触摸 ============ */
/* V1.2.x: 全屏倒计时中, 触摸屏幕任意处 → 直接停止并回到主菜单列表 (与"按任意键"同行为,
 * 不退出番茄钟应用). 设置态触摸交给底层弹窗 (返回 false 交上层处理). */
static bool p_pomodoro_touch(ui_ctx_t *ctx, int x, int y) {
    (void)x; (void)y;
    if (s_active) {
        pomo_stop();
        pomo_open_main(ctx);
        ctx->needs_redraw = true;
        return true;
    }
    return false;
}

/* ============ 每帧轮询: 倒计时/提醒音驱动 ============ */
static int s_last_second = -1;
static void p_pomodoro_poll(ui_ctx_t *ctx) {
    pomo_tick();
    ring_update();
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
    /* 弹窗栈空且非倒计时态: 本页无内容可显示 → 自动退回上一级 (保留原位置, 不弹确认框) */
    if (!s_active && os_dialog_depth() <= 0) {
        os_pop(ctx);
    }
}

/* ============ 进入/退出 ============ */
static void p_pomodoro_enter(ui_ctx_t *ctx) {
    pomo_cfg_load();
    s_active = false;
    s_ring = false;
    pomo_open_main(ctx);
    ctx->needs_redraw = true;
}
static void p_pomodoro_exit(ui_ctx_t *ctx) {
    (void)ctx;
    pomo_stop();
    os_dialog_clear_all(ctx);
}

/* ============ 模块契约 ============ */
static const os_module_t s_mod_pomodoro = {
    .name      = "pomodoro",
    .page_id   = OS_PAGE_POMODORO,
    .on_enter  = p_pomodoro_enter,
    .on_exit   = p_pomodoro_exit,
    .render    = p_pomodoro_render,
    .action    = p_pomodoro_action,
    .touch     = p_pomodoro_touch,
    .poll      = p_pomodoro_poll,
    .fullscreen = true,
};

void os_page_pomodoro_register(void) { os_register(&s_mod_pomodoro); }
