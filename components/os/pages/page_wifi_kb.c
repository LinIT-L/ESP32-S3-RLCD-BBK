/**
 * page_wifi_kb.c — WiFi 虚拟键盘 页面模块 (P3 迁移).
 *
 * 从 menu_system.c 的 wifi_kb_* 迁移, 自包含页面:
 *   - SSID / 密码两字段输入, 字母/符号两层 4 行键盘
 *   - 确认 = 连接 (wifi_manager_connect), 返回 = 退出
 *   - 进入时预填 NVS 保存的配置; 连接状态由状态栏服务显示
 *   - 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "ui_common.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#define TAG "WIFIKB"

#define KB_KEY_W    40
#define KB_KEY_H    42
#define KB_ROW_GAP  3
#define KB_TOP_Y    58

/* 按键动作 */
#define KBK_CHAR   0
#define KBK_SHIFT  1
#define KBK_SYM    2
#define KBK_DEL    3
#define KBK_SPACE  4
#define KBK_RET    5
#define KBK_FIELD  6
#define KBK_OK     7

/* 字母层 4 个字符行 (行3 中间 8 键, 左右是 Shift/退格) */
static const char *const kb_abc_line[4] = {
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm,."
};
/* 符号层 4 个字符行 */
static const char *const kb_sym_line[4] = {
    "1234567890", "!@#$%^&*()", "-_=+[]{};:'", "\\|/?<>\"~"
};

/* 私有 state */
typedef struct {
    char ssid[32];
    char pass[64];
    int  field;    /* 0=SSID 1=密码 */
    bool shift;
    bool sym;
    char msg[24];
} wkb_state_t;
static wkb_state_t s_wkb;

static void wkb_reset(void) {
    s_wkb.ssid[0] = '\0';
    s_wkb.pass[0] = '\0';
    s_wkb.field = 0;
    s_wkb.shift = false;
    s_wkb.sym = false;
    s_wkb.msg[0] = '\0';
    /* 预填 NVS 保存的配置 (有则直接改密码) */
    char saved_ssid[32], saved_pass[64];
    wifi_manager_get_saved(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass));
    if (saved_ssid[0]) {
        snprintf(s_wkb.ssid, sizeof(s_wkb.ssid), "%s", saved_ssid);
        s_wkb.field = 1;
    }
}

static char *wkb_buf(void) { return (s_wkb.field == 0) ? s_wkb.ssid : s_wkb.pass; }
static size_t wkb_max(void) { return (s_wkb.field == 0) ? sizeof(s_wkb.ssid) - 1 : sizeof(s_wkb.pass) - 1; }

/* 显示字符: 字母层且 Shift 激活时, 小写字母转大写 */
static char kb_disp(char c) {
    if (!s_wkb.sym && s_wkb.shift && c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return c;
}

/* 画一个按键 (黑边框 + 居中文本; sel=反色) */
static void kb_draw_key(st7305_handle_t *lcd, int x, int y, int w, const char *label, bool sel) {
    if (sel) {
        fill_rect(lcd, x, y, x + w - 1, y + KB_KEY_H - 1, ST7305_COLOR_BLACK);
    } else {
        fill_rect(lcd, x, y, x + w - 1, y + KB_KEY_H - 1, ST7305_COLOR_WHITE);
        draw_hline(lcd, x, x + w - 1, y, ST7305_COLOR_BLACK);
        draw_hline(lcd, x, x + w - 1, y + KB_KEY_H - 1, ST7305_COLOR_BLACK);
        draw_vline(lcd, x, y, y + KB_KEY_H - 1, ST7305_COLOR_BLACK);
        draw_vline(lcd, x + w - 1, y, y + KB_KEY_H - 1, ST7305_COLOR_BLACK);
    }
    int tw = text_width(label);
    draw_text(lcd, x + (w - tw) / 2, y + (KB_KEY_H - 24) / 2, label, sel);
}

/* 执行一个按键动作 */
static void wkb_do_key(ui_ctx_t *ctx, int kind, int ch) {
    switch (kind) {
    case KBK_CHAR: {
        char c = (char)ch;
        if (s_wkb.shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        char *buf = wkb_buf();
        size_t max = wkb_max();
        size_t len = strlen(buf);
        if (len < max) { buf[len] = c; buf[len + 1] = '\0'; }
        break;
    }
    case KBK_SPACE: {
        char *buf = wkb_buf();
        size_t max = wkb_max();
        size_t len = strlen(buf);
        if (len < max) { buf[len] = ' '; buf[len + 1] = '\0'; }
        break;
    }
    case KBK_SHIFT:
        s_wkb.shift = !s_wkb.shift;
        break;
    case KBK_SYM:
        s_wkb.sym = !s_wkb.sym;
        break;
    case KBK_DEL: {
        char *buf = wkb_buf();
        size_t len = strlen(buf);
        if (len > 0) buf[len - 1] = '\0';
        break;
    }
    case KBK_FIELD:
        s_wkb.field = !s_wkb.field;
        break;
    case KBK_RET:
        os_pop(ctx);
        return;
    case KBK_OK:
        if (s_wkb.ssid[0] == '\0') {
            snprintf(s_wkb.msg, sizeof(s_wkb.msg), "\xe8\xaf\xb7\xe5\x85\x88\xe8\xbe\x93\xe5\x85\xa5 SSID"); /* 请先输入 SSID */
        } else {
            wifi_manager_connect(s_wkb.ssid, s_wkb.pass);
            s_wkb.msg[0] = '\0';
            ESP_LOGI(TAG, "WiFi 连接: %s", s_wkb.ssid);
            os_pop(ctx);
            return;
        }
        break;
    }
    ctx->needs_redraw = true;
}

/* ============ 渲染 ============ */
static void p_wifi_kb_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    draw_text_centered(lcd, 2, "Wi-Fi \xe8\xbf\x9e\xe6\x8e\xa5", false); /* Wi-Fi 连接 */

    /* 顶部当前输入行 (点击切换字段) */
    char line[80];
    const char *tag = (s_wkb.field == 0) ? "SSID" : "\xe5\xaf\x86\xe7\xa0\x81"; /* SSID / 密码 */
    const char *val = (s_wkb.field == 0) ? s_wkb.ssid : s_wkb.pass;
    snprintf(line, sizeof(line), "%s: %s_", tag, val);
    draw_text(lcd, 8, 30, line, false);

    const char *const *lines = s_wkb.sym ? kb_sym_line : kb_abc_line;
    int y = KB_TOP_Y;

    /* 行 0/1: 10 键 */
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < 10; i++) {
            char t[2] = { kb_disp(lines[r][i]), 0 };
            kb_draw_key(lcd, i * KB_KEY_W, y, KB_KEY_W, t, false);
        }
        y += KB_KEY_H + KB_ROW_GAP;
    }
    /* 行 2: 9 键居中 */
    int x2 = (UI_SCREEN_W - 9 * KB_KEY_W) / 2;
    for (int i = 0; i < 9; i++) {
        char t[2] = { kb_disp(lines[2][i]), 0 };
        kb_draw_key(lcd, x2 + i * KB_KEY_W, y, KB_KEY_W, t, false);
    }
    y += KB_KEY_H + KB_ROW_GAP;

    /* 行 3: 左(Shift/ABC) + 8 键 + 右(退格) */
    kb_draw_key(lcd, 0, y, 52, s_wkb.sym ? "ABC" : (s_wkb.shift ? "\xe5\xb0\x8f\xe5\x86\x99" : "\xe5\xa4\xa7\xe5\x86\x99"), false); /* 小写/大写 */
    int x3 = 52;
    const int mid_w = (UI_SCREEN_W - 104) / 8;
    for (int i = 0; i < 8; i++) {
        char t[2] = { kb_disp(lines[3][i]), 0 };
        kb_draw_key(lcd, x3 + i * mid_w, y, mid_w, t, false);
    }
    kb_draw_key(lcd, UI_SCREEN_W - 52, y, 52, "\xe9\x80\x80\xe6\xa0\xbc", false); /* 退格 */
    y += KB_KEY_H + KB_ROW_GAP;

    /* 行 4: &123 + 空格 + 删除 + 连接 */
    kb_draw_key(lcd, 0, y, 60, s_wkb.sym ? "ABC" : "&123", false);
    kb_draw_key(lcd, 63, y, 150, "\xe7\xa9\xba\xe6\xa0\xbc", false);           /* 空格 */
    kb_draw_key(lcd, 216, y, 86, "\xe5\x88\xa0\xe9\x99\xa4", false);           /* 删除 */
    kb_draw_key(lcd, 305, y, UI_SCREEN_W - 305, "\xe8\xbf\x9e\xe6\x8e\xa5", false); /* 连接 */

    if (s_wkb.msg[0]) {
        draw_text(lcd, 8, 285, s_wkb.msg, false);
    }
}

/* ============ 触摸 ============ */
static bool p_wifi_kb_touch(ui_ctx_t *ctx, int x, int y) {
    if (y < KB_TOP_Y) {   /* 点击顶部输入行 -> 切换字段 */
        wkb_do_key(ctx, KBK_FIELD, 0);
        return true;
    }
    int row = (y - KB_TOP_Y) / (KB_KEY_H + KB_ROW_GAP);
    const char *const *lines = s_wkb.sym ? kb_sym_line : kb_abc_line;
    if (row == 0 || row == 1) {
        if (x < 0 || x >= 10 * KB_KEY_W) return false;
        wkb_do_key(ctx, KBK_CHAR, lines[row][x / KB_KEY_W]);
        return true;
    }
    if (row == 2) {
        int x2 = (UI_SCREEN_W - 9 * KB_KEY_W) / 2;
        if (x < x2 || x >= x2 + 9 * KB_KEY_W) return false;
        wkb_do_key(ctx, KBK_CHAR, lines[2][(x - x2) / KB_KEY_W]);
        return true;
    }
    if (row == 3) {
        if (x < 52) { wkb_do_key(ctx, s_wkb.sym ? KBK_SYM : KBK_SHIFT, 0); return true; }
        if (x >= UI_SCREEN_W - 52) { wkb_do_key(ctx, KBK_DEL, 0); return true; }
        const int mid_w = (UI_SCREEN_W - 104) / 8;
        int mi = (x - 52) / mid_w;
        if (mi >= 0 && mi < 8) wkb_do_key(ctx, KBK_CHAR, lines[3][mi]);
        return true;
    }
    if (row == 4) {
        if (x < 60) { wkb_do_key(ctx, KBK_SYM, 0); return true; }
        if (x < 63 + 150) { wkb_do_key(ctx, KBK_SPACE, 0); return true; }
        if (x < 216 + 86) { wkb_do_key(ctx, KBK_DEL, 0); return true; }
        wkb_do_key(ctx, KBK_OK, 0);
        return true;
    }
    return false;
}

/* ============ 动作 ============ */
static void p_wifi_kb_action(ui_ctx_t *ctx, os_action_t a) {
    switch (a) {
    case OS_ACTION_CONFIRM:
        wkb_do_key(ctx, KBK_OK, 0);   /* 确认 = 连接 */
        break;
    case OS_ACTION_BACK:
        os_pop(ctx);
        break;
    default:
        break;
    }
}

/* ============ 模块契约 ============ */
static void p_wifi_kb_enter(ui_ctx_t *ctx) {
    (void)ctx;
    wkb_reset();
}

static const os_module_t s_mod_wifi_kb = {
    .name       = "wifi_kb",
    .page_id    = OS_PAGE_WIFI_KB,
    .on_enter   = p_wifi_kb_enter,
    .render     = p_wifi_kb_render,
    .action     = p_wifi_kb_action,
    .touch      = p_wifi_kb_touch,
    .fullscreen = false,
};

void os_page_wifi_kb_register(void) { os_register(&s_mod_wifi_kb); }
