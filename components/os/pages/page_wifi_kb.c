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
#define KB_KEY_H    48   /* V1.1.1: 5 行键盘紧贴到最下方 (58 + 5×48 = 298) */
#define KB_ROW_GAP  0    /* V1.1.1: 行间无空隙 */
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
/* 设置页选择 WiFi 后预填的 SSID: 独立缓存, wkb_reset/on_enter 不会把它冲掉,
 * 保证从"无线连接→连接WiFi→选网"进入时直接显示输密码. */
static char s_preset_ssid[32] = "";

static void wkb_reset(void) {
    s_wkb.ssid[0] = '\0';
    s_wkb.pass[0] = '\0';
    s_wkb.field = 0;
    s_wkb.shift = false;
    s_wkb.sym = false;
    s_wkb.msg[0] = '\0';
    s_wkb.field = 1;   /* 始终输密码 (SSID 只读, 由设置页预填) */
    if (s_preset_ssid[0]) {
        /* 预填缓存的 SSID (来自设置页选网), 直接输密码 */
        snprintf(s_wkb.ssid, sizeof(s_wkb.ssid), "%s", s_preset_ssid);
    } else {
        /* 预填 NVS 保存的配置 (有则直接改密码) */
        char saved_ssid[32], saved_pass[64];
        wifi_manager_get_saved(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass));
        if (saved_ssid[0]) {
            snprintf(s_wkb.ssid, sizeof(s_wkb.ssid), "%s", saved_ssid);
            s_wkb.field = 1;
        }
    }
}

/* V1.1.1: WiFi 列表选择后预填 SSID, 直接输密码 (field=1) */
void wifi_kb_preset_ssid(const char *ssid) {
    if (ssid && ssid[0])
        snprintf(s_preset_ssid, sizeof(s_preset_ssid), "%s", ssid);
    else
        s_preset_ssid[0] = '\0';
    wkb_reset();
}

/* 本页仅用于输入密码 (SSID 由设置页选网预填), 不再在线编辑 SSID */
static char *wkb_buf(void) { return s_wkb.pass; }
static size_t wkb_max(void) { return sizeof(s_wkb.pass) - 1; }

/* 显示字符: 字母层且 Shift 激活时, 小写字母转大写 */
static char kb_disp(char c) {
    if (!s_wkb.sym && s_wkb.shift && c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return c;
}

/* ===== 手柄/方向键导航光标 (可选中任意键并按确认输入) ===== */
typedef struct { int kind, ch; int cx, w; } kb_nav_t;   /* ch 对 KBK_DEL 区分行, 见 kb_key_label */
static kb_nav_t s_nav[64];
static int  s_nav_n = 0;
static int  s_nav_cur = 0;
#define NAV_DEL_ROW3 3   /* 行3 退格标签 */
#define NAV_DEL_ROW4 4   /* 行4 删除标签 */

/* 键标签 (渲染高亮复用) */
static const char *kb_key_label(int kind, int ch) {
    static char one[2];
    switch (kind) {
    case KBK_CHAR: one[0] = kb_disp((char)ch); one[1] = 0; return one;
    case KBK_SHIFT: return s_wkb.sym ? "ABC" : (s_wkb.shift ? "\xe5\xb0\x8f\xe5\x86\x99" : "\xe5\xa4\xa7\xe5\x86\x99"); /* 小写/大写 */
    case KBK_SYM:   return s_wkb.sym ? "ABC" : "&123";
    case KBK_DEL:   return (ch == NAV_DEL_ROW4) ? "\xe5\x88\xa0\xe9\x99\xa4" : "\xe9\x80\x80\xe6\xa0\xbc"; /* 删除/退格 */
    case KBK_SPACE: return "\xe7\xa9\xba\xe6\xa0\xbc";  /* 空格 */
    case KBK_RET:   return "\xe8\xbf\x94\xe5\x9b\x9e";  /* 返回 */
    case KBK_OK:    return "\xe8\xbf\x9e\xe6\x8e\xa5";   /* 连接 */
    default: return "?";
    }
}

/* 构建当前导航键表 (位置固定; 字母/符号切换会改字符, 需在切换时重建) */
static void kb_nav_build(void) {
    s_nav_n = 0;
    const char *const *lines = s_wkb.sym ? kb_sym_line : kb_abc_line;
    int y = KB_TOP_Y;
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < 10; i++)
            s_nav[s_nav_n++] = (kb_nav_t){ KBK_CHAR, lines[r][i], i * KB_KEY_W + KB_KEY_W / 2, KB_KEY_W };
        y += KB_KEY_H + KB_ROW_GAP;
    }
    int x2 = (UI_SCREEN_W - 9 * KB_KEY_W) / 2;
    for (int i = 0; i < 9; i++)
        s_nav[s_nav_n++] = (kb_nav_t){ KBK_CHAR, lines[2][i], x2 + i * KB_KEY_W + KB_KEY_W / 2, KB_KEY_W };
    y += KB_KEY_H + KB_ROW_GAP;
    int mid_w = (UI_SCREEN_W - 104) / 8;
    s_nav[s_nav_n++] = (kb_nav_t){ KBK_SHIFT, 0, 52 / 2, 52 };
    for (int i = 0; i < 8; i++)
        s_nav[s_nav_n++] = (kb_nav_t){ KBK_CHAR, lines[3][i], 52 + i * mid_w + mid_w / 2, mid_w };
    s_nav[s_nav_n++] = (kb_nav_t){ KBK_DEL, NAV_DEL_ROW3, UI_SCREEN_W - 52 + 52 / 2, 52 };
    y += KB_KEY_H + KB_ROW_GAP;
    s_nav[s_nav_n++] = (kb_nav_t){ KBK_SYM, 0, 60 / 2, 60 };
    s_nav[s_nav_n++] = (kb_nav_t){ KBK_SPACE, 0, 60 + 152 / 2, 152 };
    s_nav[s_nav_n++] = (kb_nav_t){ KBK_DEL, NAV_DEL_ROW4, 212 + 88 / 2, 88 };
    s_nav[s_nav_n++] = (kb_nav_t){ KBK_OK, 0, 300 + (UI_SCREEN_W - 300) / 2, UI_SCREEN_W - 300 };
    if (s_nav_cur >= s_nav_n) s_nav_cur = s_nav_n - 1;
    if (s_nav_cur < 0) s_nav_cur = 0;
}

/* 导航索引 → 键盘行号 (0..4) */
static int kb_nav_row(int idx) {
    return (idx < 10) ? 0 : (idx < 20) ? 1 : (idx < 29) ? 2 : (idx < 38) ? 3 : 4;
}
/* 指定方向最近键, dir: 0上 1下 2左 3右 */
static bool kb_nav_move(int dir) {
    if (s_nav_n <= 1) return false;
    int cx = s_nav[s_nav_cur].cx;
    int cur_row = kb_nav_row(s_nav_cur);
    int cyc = KB_TOP_Y + cur_row * (KB_KEY_H + KB_ROW_GAP);
    int best = -1, best_k = 1 << 30;
    for (int i = 0; i < s_nav_n; i++) {
        if (i == s_nav_cur) continue;
        int cy0 = KB_TOP_Y + kb_nav_row(i) * (KB_KEY_H + KB_ROW_GAP);
        if (dir == 0 && cy0 >= cyc) continue;   /* 上: 只在更靠上行 */
        if (dir == 1 && cy0 <= cyc) continue;   /* 下: 只在更靠下行 */
        if (dir == 2 && s_nav[i].cx >= cx) continue;   /* 左: 只在更靠左 */
        if (dir == 3 && s_nav[i].cx <= cx) continue;   /* 右: 只在更靠右 */
        int dx = s_nav[i].cx - cx; if (dx < 0) dx = -dx;
        int dy = cy0 - cyc;  if (dy < 0) dy = -dy;
        int k = (dir == 2 || dir == 3) ? dx : (dy * 2 + dx);   /* 上下优先竖向距离 */
        if (k < best_k) { best_k = k; best = i; }
    }
    if (best >= 0) { s_nav_cur = best; return true; }
    return false;
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
        kb_nav_build();   /* 切换大小写: 刷新导航标签 (位置不变) */
        break;
    case KBK_SYM:
        s_wkb.sym = !s_wkb.sym;
        kb_nav_build();   /* 切换字母/符号层: 重建导航字符集 */
        break;
    case KBK_DEL: {
        char *buf = wkb_buf();
        size_t len = strlen(buf);
        if (len > 0) buf[len - 1] = '\0';
        break;
    }
    case KBK_FIELD:
        /* 密码输入页: 不再切换 SSID/密码字段 */
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

    /* 顶部当前输入行 (点击切换字段); 有提示时显示提示 */
    char line[80];
    if (s_wkb.msg[0]) {
        snprintf(line, sizeof(line), "%s", s_wkb.msg);
    } else {
        const char *tag = (s_wkb.field == 0) ? "SSID" : "\xe5\xaf\x86\xe7\xa0\x81"; /* SSID / 密码 */
        const char *val = (s_wkb.field == 0) ? s_wkb.ssid : s_wkb.pass;
        snprintf(line, sizeof(line), "%s: %s_", tag, val);
    }
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
    kb_draw_key(lcd, 60, y, 152, "\xe7\xa9\xba\xe6\xa0\xbc", false);        /* 空格 */
    kb_draw_key(lcd, 212, y, 88, "\xe5\x88\xa0\xe9\x99\xa4", false);        /* 删除 */
    kb_draw_key(lcd, 300, y, UI_SCREEN_W - 300, "\xe8\xbf\x9e\xe6\x8e\xa5", false); /* 连接 (紧贴右缘) */

    /* 手柄/方向键导航: 最后重绘聚焦键为反色高亮 (覆盖普通绘制) */
    if (s_nav_n > 0 && s_nav_cur >= 0 && s_nav_cur < s_nav_n) {
        const kb_nav_t *k = &s_nav[s_nav_cur];
        int key_y = KB_TOP_Y + kb_nav_row(s_nav_cur) * (KB_KEY_H + KB_ROW_GAP);
        kb_draw_key(lcd, k->cx - k->w / 2, key_y, k->w, kb_key_label(k->kind, k->ch), true);
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
        if (x < 60 + 152) { wkb_do_key(ctx, KBK_SPACE, 0); return true; }
        if (x < 212 + 88) { wkb_do_key(ctx, KBK_DEL, 0); return true; }
        wkb_do_key(ctx, KBK_OK, 0);
        return true;
    }
    return false;
}

/* ============ 动作 (手柄/方向键) ============ */
static void p_wifi_kb_action(ui_ctx_t *ctx, os_action_t a) {
    switch (a) {
    case OS_ACTION_UP:   kb_nav_move(0); break;
    case OS_ACTION_DOWN: kb_nav_move(1); break;
    case OS_ACTION_LEFT: kb_nav_move(2); break;
    case OS_ACTION_RIGHT:kb_nav_move(3); break;
    case OS_ACTION_CONFIRM: {
        if (s_nav_n <= 0 || s_nav_cur < 0 || s_nav_cur >= s_nav_n) break;
        int kind = s_nav[s_nav_cur].kind;
        int ch = s_nav[s_nav_cur].ch;
        if (kind == KBK_SHIFT && s_wkb.sym) kind = KBK_SYM;   /* 符号层 行3左 = 切回 ABC */
        wkb_do_key(ctx, kind, ch);
        return;
    }
    case OS_ACTION_BACK:
        os_pop(ctx);
        return;
    default:
        break;
    }
    ctx->needs_redraw = true;
}

/* ============ 模块契约 ============ */
static void p_wifi_kb_enter(ui_ctx_t *ctx) {
    (void)ctx;
    wkb_reset();
    s_nav_cur = 0;
    kb_nav_build();   /* 进入即建导航键表 (手柄/方向键可用) */
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
