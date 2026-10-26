/**
 * applock.c — 通用「应用锁」实现 (PIN 弹窗门禁)
 *
 * 设计 (2026-10-18 linit: "单独做一个模块, 其他应用直接调用, UI 用现在成熟的这套"):
 *   · UI **完全复用密钥管理器那套 PIN 弹窗** (304px 窗口 / 标题「输入Pin」/ 单一输入框 /
 *     九宫格 1-9 / 底行 删除·0·确定), 各应用不再各画一套;
 *   · 校验/锁定/失败计数**全部走系统 keyvault**, 全设备共享一份 PIN 与冷却时间;
 *   · ★ 2026-10-18 linit: **退出即重新锁定** —— 不记忆解锁状态, 每次进入都必须重新输 PIN;
 *   · BACK = 取消: 退回上一级页面 (不进应用)。
 *
 * 页面接入 (3 处):
 *   init:   s_authed = applock_gate(ctx, "fav", "收藏夹");
 *   render: ...画自己的内容...  if (applock_active()) applock_render(lcd);
 *   key:    if (applock_active()) { applock_action(ctx, a); if (!applock_active()) s_authed = true; return; }
 *   touch:  同上, 调 applock_touch()
 */
#include <string.h>
#include <stdio.h>
#include "applock.h"
#include "ui_common.h"
#include "input.h"
#include "os_keyvault.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "APPLOCK";

#define AL_W            400
#define AL_H            300
#define AL_ID_LEN       16

/* ---- PIN 弹窗几何 (与 page_keymanager.c 完全一致) ---- */
#define KP_WIN_W    304
#define KP_WIN_X    ((AL_W - KP_WIN_W) / 2)   /* 48 */
#define KP_WIN_Y    24
#define KP_TITLE_H  26
#define KP_PIN_Y    (KP_WIN_Y + KP_TITLE_H + 8)
#define KP_PIN_H    46
#define KP_TOP      (KP_PIN_Y + KP_PIN_H + 12)
#define KP_COL_W    88
#define KP_COL_X0   (KP_WIN_X + 12)   /* 3*88+2*8=280, 左右各 12 → 真正居中 */
#define KP_COL_GAP  8
#define KP_ROW_H    36
#define KP_ROW_GAP  4
#define KP_WIN_H    (KP_TOP + 4 * (KP_ROW_H + KP_ROW_GAP) - KP_ROW_GAP + 8 - KP_WIN_Y)

/* ---- 状态 ---- */
static bool  s_active;
static bool  s_need_setup;   /* 未设 PIN: 进入设置流程 */
static int   s_step;         /* 0=第一次输入 1=再次确认 */
static char  s_pin2[20];     /* 第一次输入暂存 */
static uint8_t s_key[KEYVAULT_KEY_LEN];   /* 解锁后的数据密钥 (本次进入有效) */
static bool  s_have_key;
static char  s_app[AL_ID_LEN];
static char  s_title[24];
static char  s_pin[20];
static int   s_pin_len;
static int   s_cr, s_cc;          /* 键盘光标 */
static int   s_pr = -1, s_pc = -1;  /* 触摸按下高亮 (格子) */
static bool  s_prev_down;          /* 上一帧手指是否按着 (用于触发重绘) */
static int64_t s_pr_until_us;      /* 高亮截止时间: 按一下亮 120ms, 之后自动恢复 */

void applock_forget_all(void) { /* 退出即锁, 无会话状态可清 */ }

bool applock_gate(ui_ctx_t *ctx, const char *app_id, const char *title) {
    (void)ctx;
    os_keyvault_init();                      /* ★ 必须: keyvault_set_pin 要求 s_ready */
    if (!app_id || !app_id[0]) return true;
    /* ★★ linit: **统一 PIN + 强制先设置** —— 没有 PIN 时不许进 (数据要加密, 没密钥等于明文),
     *   显示一页提示去「密钥管理器」设置; 设过就按正常验证流程。 */
    if (!os_keyvault_has_pin()) {
        snprintf(s_app, sizeof(s_app), "%s", app_id);
        snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
        s_need_setup = true; s_active = true;
        s_step = 0; s_pin[0] = 0; s_pin_len = 0; s_pin2[0] = 0;
        s_cr = 0; s_cc = 0; s_pr = -1; s_pc = -1;
        ESP_LOGW(TAG, "未设置 PIN: 进入设置流程 (%s)", s_app);
        return false;
    }
    s_need_setup = false;

    snprintf(s_app, sizeof(s_app), "%s", app_id);
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    s_pin[0] = 0; s_pin_len = 0;
    s_cr = 0; s_cc = 0; s_pr = -1; s_pc = -1;
    s_active = true;
    ESP_LOGI(TAG, "锁定: app=%s", s_app);
    return false;
}

bool applock_active(void) { return s_active; }

/* ================= 绘制 ================= */
static void kp_rect(int r, int c, int *x, int *y, int *w, int *h) {
    *x = KP_COL_X0 + c * (KP_COL_W + KP_COL_GAP);
    *y = KP_TOP + r * (KP_ROW_H + KP_ROW_GAP);
    *w = KP_COL_W; *h = KP_ROW_H;
}

static void draw_cell(st7305_handle_t *lcd, int cx, int cy, int w, int h,
                      const char *text, bool cur, bool pressed, int fh) {
    (void)cur;   /* ★ 2026-10-18 linit: 不要常驻反色 —— 只有手指按着那一刻才黑, 松手恢复 */
    bool active = pressed;
    draw_rect_outline(lcd, cx, cy, cx + w - 1, cy + h - 1, ST7305_COLOR_BLACK);
    if (active) fill_rect(lcd, cx + 1, cy + 1, cx + w - 2, cy + h - 2, ST7305_COLOR_BLACK);
    /* ★ 2026-10-18: ASCII 字形墨迹偏下 (16px 框内只占约 12 行) ⇒ 上移 2px 才视觉居中 */
    int ty = cy + (h - fh) / 2 - (fh == 16 ? 6 : 1);   /* 实测: ASCII 墨迹偏下 4px, 多减 4 */
    draw_label_centered_at(lcd, cx + w / 2, ty, text, active);
}

void applock_render(st7305_handle_t *lcd) {
    if (!s_active || !lcd) return;
    /* 未设 PIN: 复用同一套弹窗做「设置 Pin / 再次输入」 */
    int wx0 = KP_WIN_X, wy0 = KP_WIN_Y;
    int wx1 = KP_WIN_X + KP_WIN_W - 1, wy1 = KP_WIN_Y + KP_WIN_H - 1;

    /* ★ 2026-10-19: 锁屏时把整屏刷白, 不显示后面的程序界面 (1bpp 做不了模糊, 直接空白) */
    fill_rect(lcd, 0, 0, ST7305_WIDTH - 1, ST7305_HEIGHT - 1, ST7305_COLOR_WHITE);

    fill_rect(lcd, wx0, wy0, wx1, wy1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, wx0 + k, wx1 - k, wy0 + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, wx0 + k, wx1 - k, wy1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, wx0 + k, wy0 + k, wy1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, wx1 - k, wy0 + k, wy1 - k, ST7305_COLOR_BLACK);
    }

    /* 标题: 「输入Pin」+ 右侧应用名 (区分是哪个应用要密码) */
    const char *t = s_need_setup
        ? (s_step == 0 ? "\xe8\xae\xbe\xe7\xbd\xaePIN" : "\xe5\x86\x8d\xe6\xac\xa1\xe8\xbe\x93\xe5\x85\xa5")  /* 设置Pin / 再次输入 */
        : "\xe8\xbe\x93\xe5\x85\xa5PIN";   /* 输入PIN */
    int tw = text_width(t);
    draw_text(lcd, KP_WIN_X + (KP_WIN_W - tw) / 2, KP_WIN_Y + 5, t, false);
    /* 不显示应用名 (linit: 标题只留「输入Pin」) */

    /* 输入框 */
    {
        int px0 = KP_WIN_X + 12, px1 = KP_WIN_X + KP_WIN_W - 12;
        int py0 = KP_PIN_Y, py1 = KP_PIN_Y + KP_PIN_H - 1;
        draw_rect_outline(lcd, px0, py0, px1, py1, ST7305_COLOR_BLACK);
        char dots[20];
        int n = s_pin_len > 18 ? 18 : s_pin_len;
        for (int i = 0; i < n; i++) dots[i] = '*';
        dots[n] = 0;
        if (n > 0) draw_text_centered(lcd, py0 + (KP_PIN_H - 26) / 2, dots, false);
    }

    /* 触摸反馈: 手指正按着 → 该格黑; 刚按过 120ms 内 → 仍是该格黑 (触摸是瞬时事件,
     * 渲染时手指往往已经抬起, 只查"当前按着"会完全看不到反馈) */
    int tpx = -1, tpy = -1;
    bool touch_down = input_get_touch_pos(&tpx, &tpy);   /* 实时: 按着才有坐标, 松手即失效 */

    /* 九宫格 1-9 */
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int x, y, w, h; kp_rect(r, c, &x, &y, &w, &h);
            char buf[2] = { (char)('0' + (r * 3 + c + 1)), 0 };
            bool p = touch_down && tpx >= x && tpx < x + w && tpy >= y && tpy < y + h;
            draw_cell(lcd, x, y, w, h, buf, false, p, 16);
        }
    }
    /* 底行: 删除 / 0 / 确定 */
    {
        int x, y, w, h;
        kp_rect(3, 0, &x, &y, &w, &h);
        draw_cell(lcd, x, y, w, h, "\xe5\x88\xa0\xe9\x99\xa4", false, (touch_down && tpx >= x && tpx < x + w && tpy >= y && tpy < y + h), 24);
        kp_rect(3, 1, &x, &y, &w, &h);
        draw_cell(lcd, x, y, w, h, "0", false, (touch_down && tpx >= x && tpx < x + w && tpy >= y && tpy < y + h), 16);
        kp_rect(3, 2, &x, &y, &w, &h);
        draw_cell(lcd, x, y, w, h, "\xe7\xa1\xae\xe5\xae\x9a", false, (touch_down && tpx >= x && tpx < x + w && tpy >= y && tpy < y + h), 24);
    }
}

/* ================= 输入 ================= */
static void pin_digit(char ch) {
    if (s_pin_len < 15) { s_pin[s_pin_len++] = ch; s_pin[s_pin_len] = 0; }
}

static void pin_submit(ui_ctx_t *ctx) {
    /* ★ 允许空密码: 0 位也能提交 (存的是空密码就直接过, 否则算输错) */
    if (s_need_setup) {
        if (s_step == 0) {
            memcpy(s_pin2, s_pin, sizeof(s_pin2));
            s_pin[0] = 0; s_pin_len = 0; s_step = 1;
            ctx->needs_redraw = true;
            return;
        }
        if (strcmp(s_pin, s_pin2) != 0) {
            os_dialog_toast(ctx, "\xe4\xb8\xa4\xe6\xac\xa1\xe4\xb8\x8d\xe4\xb8\x80\xe8\x87\xb4"); /* 两次不一致 */
            s_pin[0] = 0; s_pin_len = 0; s_pin2[0] = 0; s_step = 0;
            ctx->needs_redraw = true;
            return;
        }
        os_keyvault_init();
        int sr = os_keyvault_set_pin(s_pin2);
        if (sr != 0) {
            char m[32]; snprintf(m, sizeof(m), "PIN \xe4\xbf\x9d\xe5\xad\x98\xe5\xa4\xb1\xe8\xb4\xa5(%d)", sr);
            os_dialog_toast(ctx, m);
            s_pin[0]=0; s_pin_len=0; s_pin2[0]=0; s_step=0;
            ctx->needs_redraw = true;
            return;
        }
        ESP_LOGI(TAG, "PIN 已保存 (%d 位)", (int)strlen(s_pin2));
        os_keyvault_unlock_secret(s_pin2, s_key);
        s_have_key = true;
        os_dialog_toast(ctx, "PIN \xe5\xb7\xb2\xe8\xae\xbe\xe7\xbd\xae");  /* PIN 已设置 */
        s_pin[0] = 0; s_pin_len = 0; s_pin2[0] = 0;
        s_need_setup = false; s_active = false; s_step = 0;
        ctx->needs_redraw = true;
        return;
    }
    int r = os_keyvault_verify_pin(s_pin);
    if (r == 0) {
        os_keyvault_unlock_secret(s_pin, s_key);   /* ★ 派生数据密钥, 供本页加密/解密 */
        s_have_key = true;
        s_active = false;
        s_pin[0] = 0; s_pin_len = 0;
        ESP_LOGI(TAG, "解锁: app=%s", s_app);
        ctx->needs_redraw = true;
        return;
    }
    char msg[48];
    os_dialog_toast(ctx, os_keyvault_pin_msg(r, msg, sizeof(msg)));
    s_pin[0] = 0; s_pin_len = 0;
    ctx->needs_redraw = true;
}

static void pin_cancel(ui_ctx_t *ctx) {
    memset(s_key, 0, sizeof(s_key)); s_have_key = false;
    s_active = false;
    s_pin[0] = 0; s_pin_len = 0;
    ESP_LOGI(TAG, "取消: app=%s", s_app);
    os_pop(ctx);
}

bool applock_action(ui_ctx_t *ctx, os_action_t a) {
    if (!s_active) return false;
    if (s_need_setup && a == OS_ACTION_BACK) {
        s_active = false; s_need_setup = false; s_step = 0;
        s_pin[0] = 0; s_pin_len = 0; s_pin2[0] = 0;
        os_pop(ctx);
        return true;
    }
    s_pr = -1; s_pc = -1;
    if (a == OS_ACTION_UP && s_cr > 0) s_cr--;
    else if (a == OS_ACTION_DOWN && s_cr < 3) s_cr++;
    else if (a == OS_ACTION_LEFT && s_cc > 0) s_cc--;
    else if (a == OS_ACTION_RIGHT && s_cc < 2) s_cc++;
    else if (a == OS_ACTION_CONFIRM) {
        if (s_cr < 3) pin_digit((char)('0' + (s_cr * 3 + s_cc + 1)));
        else if (s_cc == 0) { if (s_pin_len > 0) s_pin[--s_pin_len] = 0; }
        else if (s_cc == 1) pin_digit('0');            /* ★ 底行中间的 0 (之前漏了) */
        else if (s_cc == 2) pin_submit(ctx);
    }
    else if (a == OS_ACTION_BACK) { pin_cancel(ctx); return true; }
    ctx->needs_redraw = true;
    return true;
}

bool applock_touch(ui_ctx_t *ctx, int x, int y) {
    if (!s_active) return false;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int cx, cy, w, h; kp_rect(r, c, &cx, &cy, &w, &h);
            if (x >= cx && x <= cx + w - 1 && y >= cy && y <= cy + h - 1) {
                s_cr = r; s_cc = c;
                s_pr = r; s_pc = c;
                s_pr_until_us = esp_timer_get_time() + 120000;   /* 按下反馈亮 120ms */
                if (r < 3) pin_digit((char)('0' + (r * 3 + c + 1)));
                else if (c == 0) { if (s_pin_len > 0) s_pin[--s_pin_len] = 0; }
                else if (c == 1) pin_digit('0');       /* ★ 底行中间的 0 (之前漏了) */
                else if (c == 2) pin_submit(ctx);
                ctx->needs_redraw = true;
                return true;
            }
        }
    }
    return true;   /* 锁屏期间吞掉所有触摸 */
}

/* ================= 对外: 密钥与加解密 ================= */
const uint8_t *applock_key(void) { return s_have_key ? s_key : NULL; }

size_t applock_encrypt(const void *in, size_t n, uint8_t *out) {
    if (!s_have_key || !in || !out) return 0;
    return os_keyvault_blob_encrypt(s_key, (const uint8_t *)in, n, out);
}

size_t applock_decrypt(const uint8_t *in, size_t n, void *out) {
    if (!s_have_key || !in || !out) return 0;
    return os_keyvault_blob_decrypt(s_key, in, n, (uint8_t *)out);
}

/* ================= 每帧轮询 (页面 poll 里调用) =================
 * 触摸是"事件"式的: 按下/抬起不一定触发页面重绘, 所以高亮要靠这里每帧检测状态变化。 */
void applock_tick(ui_ctx_t *ctx) {
    if (!s_active) return;
    int x, y;
    bool down = input_get_touch_pos(&x, &y);
    if (down != s_prev_down) {
        s_prev_down = down;
        ctx->needs_redraw = true;      /* 按下→画黑; 抬起→恢复白 */
    }
}
