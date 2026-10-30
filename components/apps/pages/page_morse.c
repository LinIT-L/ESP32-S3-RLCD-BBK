/**
 * page_morse.c — 摩斯电码 (精简版)
 *
 * 三个功能, 一个页面内两种视图 (顶部 Tab 切换):
 *   ① 转换 (双向):
 *        文→码: 键盘输入, 实时出摩斯串; 退格/清空/空格/符号插入;
 *        码→文: 用 点/划/斜杠 三个大键输入, 实时译出文本;
 *        长摩斯串可拖动滚动; 不支持字符给出提示, 绝不静默丢弃。
 *   ② 播放: 播放/停止, 5~35 WPM, 400~1000Hz 音调; 点=1u 划=3u,
 *           符号间隔 1u, 字符间隔合计 3u, 词间隔合计 7u (不重复累计)。
 *   ③ 查表: 字母/数字/标点三分类, 点条目即试听 (与播放共用同一时序与音频实现)。
 *
 * 音频: 走 morse_engine (自合成 PCM → os_notify_sound_feed)。
 *   不用 tone_beep: 它在 5WPM 时单符号阻塞 720ms, 无法及时停止。
 * 存储: svc_cfg 命名空间 "morse" 的 wpm / tone 两个键 (见 docs/设置与存储归属.md)。
 */
#include "os.h"
#include "ui_common.h"
#include "input.h"
#include "keyboard.h"
#include "ui_scroll.h"
#include "svc_cfg.h"
#include "morse_codec.h"
#include "morse_engine.h"

#include <stdio.h>
#include <string.h>

#define S_W 400
#define S_H 300

/* ---- 布局 ---- */
#define TAB_H        26
#define KBD_SHOW     4
#define KBD_BASE_Y   (S_H - KBD_SHOW * KBD_ROW)      /* 184 */
#define TEXT_Y       30
#define TEXT_LH      24
#define TEXT_COLS    24                              /* 24px 字体: 16px/字符 → 24 字符/行 */
#define MORSE_Y      78
#define MORSE_LH     12
#define MORSE_LINES  5
#define MORSE_H      (MORSE_LINES * MORSE_LH)        /* 60 */
#define MORSE_COLS   49                              /* 12px 字体: 8px/字符, x=6 起 → 6+49*8-1=397 ≤ 399 */
#define MORSE_X      6
#define PROG_Y       144
#define BTN_Y        152
#define BTN_H        30
#define BTN_W        58                              /* 文→码 6 键: 4 + 6*58 + 5*6 = 382 ≤ 400 */
#define BTN_GAP      6
#define BTN_N        6

#define GRID_Y       58
#define GRID_CW      66
#define GRID_CH      48
#define GRID_COLS    6
#define GRID_ROWS    5
#define GRID_X       2

/* ---- 参数 ---- */
#define NS_MORSE     "morse"
#define WPM_MIN      5
#define WPM_MAX      35
#define TONE_MIN     400
#define TONE_MAX     1000
#define TONE_STEP    50
#define TONE_N       ((TONE_MAX - TONE_MIN) / TONE_STEP + 1)   /* 13 */

#define MORSE_BUF    700

/* ---- 状态 ---- */
static char    s_text[MR_TEXT_MAX + 1];
static int     s_len;
static uint8_t s_wpm  = 20;
static uint16_t s_tone = 700;
static int     s_view;                 /* 0=转换 1=查表 */
static int     s_cat;                  /* 0=字母 1=数字 2=标点 */
static int     s_dec;                  /* 0=文→码 1=码→文 */
static char    s_min[MR_TEXT_MAX + 1]; /* 码→文: 用户输入的点划串 ('.' '-' ' ' '/') */
static int     s_mlen;
static char    s_shown[MR_TEXT_MAX + 1];/* 文本区实际显示内容 (编码模式=原文, 解码模式=译出的文本) */
static char    s_morse[MORSE_BUF];
static int     s_morse_lines;
static ui_scroll_t s_sc;
static bool    s_sc_ready;
static int     s_press_k = -1;
static kbd_t   s_kbd;                  /* 页内唯一键盘实例 (Caps/Shift 状态在这) */
static int     s_prog_last;
static bool    s_playing_last;
static bool    s_sc_touch;              /* 本次手势是否落在摩斯区内 (保证松手事件能送到 ui_scroll) */

/* ================= 小工具 ================= */
static void redraw(ui_ctx_t *ctx) { ctx->needs_redraw = true; }
static void text_changed(ui_ctx_t *ctx);      /* 前向声明: morin_* 先用到 */

static void draw_small(st7305_handle_t *l, int x, int y, const char *s)
{
    for (const char *p = s; *p; p++, x += 8) draw_ascii_small(l, x, y, *p, false);
}

static void draw_btn(st7305_handle_t *l, int i, const char *label, bool active)
{
    int x0 = 4 + i * (BTN_W + BTN_GAP);
    int x1 = x0 + BTN_W - 1;
    if (active) fill_rect(l, x0, BTN_Y, x1, BTN_Y + BTN_H - 1, ST7305_COLOR_BLACK);
    else        draw_rect_outline(l, x0, BTN_Y, x1, BTN_Y + BTN_H - 1, ST7305_COLOR_BLACK);
    int tw = text_width(label);
    draw_text(l, x0 + (BTN_W - tw) / 2, BTN_Y + 4, label, active);
}

/* ================= 数据 ================= */
static void save_cfg(void)
{
    svc_cfg_set_u8(NS_MORSE, "wpm", s_wpm);
    svc_cfg_set_u8(NS_MORSE, "tone", (uint8_t)((s_tone - TONE_MIN) / TONE_STEP));
}

static void load_cfg(void)
{
    int w = (int)svc_cfg_get_u8(NS_MORSE, "wpm", 20);
    if (w < WPM_MIN || w > WPM_MAX) w = 20;
    s_wpm = (uint8_t)w;
    int t = (int)svc_cfg_get_u8(NS_MORSE, "tone", (700 - TONE_MIN) / TONE_STEP);
    if (t < 0 || t >= TONE_N) t = (700 - TONE_MIN) / TONE_STEP;
    s_tone = (uint16_t)(TONE_MIN + t * TONE_STEP);
}

/* 码→文: 把 s_min 里以空格/'/' 分隔的点划串逐个译码 (非法 → '?') */
static void decode_to_text(void)
{
    int o = 0;
    char pat[MR_MARK_MAX + 1];
    int pn = 0;
    s_shown[0] = 0;
    for (int i = 0; i <= s_mlen; i++) {
        char c = (i < s_mlen) ? s_min[i] : ' ';
        if (c == ' ' || c == '/') {
            if (pn > 0) {
                pat[pn] = 0;
                char ch = mr_decode(pat);
                if (o < MR_TEXT_MAX) s_shown[o++] = ch ? ch : '?';
                if ((c == '/') && o < MR_TEXT_MAX) s_shown[o++] = ' ';   /* 词间隔 */
                pn = 0;
            } else if (c == '/' && o > 0 && s_shown[o - 1] != ' ' && o < MR_TEXT_MAX) {
                s_shown[o++] = ' ';
            }
            continue;
        }
        if (pn < MR_MARK_MAX) pat[pn++] = c;
    }
    s_shown[o] = 0;
}

/* 重新生成显示串与行数; 内容长度变化时重设滚动范围 */
static void rebuild_morse(void)
{
    mr_plan_t p;
    s_morse[0] = 0;
    s_morse_lines = 0;

    if (!s_dec) {
        snprintf(s_shown, sizeof(s_shown), "%s", s_text);
        if (s_len > 0 && mr_build_plan(s_text, &p) == 0 && p.n > 0) {
            if (mr_fmt_morse(&p, s_morse, sizeof(s_morse)) < 0) s_morse[0] = 0;
        }
    } else {
        decode_to_text();
        snprintf(s_morse, sizeof(s_morse), "%s", s_min);   /* 码→文: 摩斯区显示原始输入 */
    }

    int len = (int)strlen(s_morse);
    s_morse_lines = (len + MORSE_COLS - 1) / MORSE_COLS;
    if (s_morse_lines < 1) s_morse_lines = 1;

    int content = s_morse_lines * MORSE_LH;
    if (!s_sc_ready) {
        ui_scroll_config(&s_sc, UI_SCROLL_PIXEL, UI_SCROLL_F_NO_BOUNCE);
        ui_scroll_init(&s_sc, MORSE_LH, MORSE_H, content);
        s_sc_ready = true;
    } else if (!ui_scroll_busy(&s_sc)) {
        ui_scroll_init(&s_sc, MORSE_LH, MORSE_H, content);
    } else {
        ui_scroll_set_content(&s_sc, content);
    }
}

/* 码→文: 追加一个点划/分隔符 */
static void morin_append(ui_ctx_t *ctx, char c)
{
    if (s_mlen >= MR_TEXT_MAX) { os_dialog_toast(ctx, "已达 48 字符上限"); return; }
    s_min[s_mlen++] = c;
    s_min[s_mlen] = 0;
    text_changed(ctx);
}

static void morin_back(ui_ctx_t *ctx)
{
    if (s_mlen <= 0) return;
    s_min[--s_mlen] = 0;
    if (s_sc_ready) ui_scroll_to_top(&s_sc);
    text_changed(ctx);
}

static void text_changed(ui_ctx_t *ctx)
{
    rebuild_morse();
    redraw(ctx);
}

static void append_char(ui_ctx_t *ctx, char c)
{
    if (s_len >= MR_TEXT_MAX) { os_dialog_toast(ctx, "已达 48 字符上限"); return; }
    if (c != ' ' && !mr_encode(c)) { os_dialog_toast(ctx, "不支持该字符"); return; }
    if (s_len > 0 && c == ' ' && s_text[s_len - 1] == ' ') return;   /* 连空格折叠 */
    s_text[s_len++] = c;
    s_text[s_len] = 0;
    text_changed(ctx);
}

static void backspace(ui_ctx_t *ctx)
{
    if (s_dec) { morin_back(ctx); return; }
    if (s_len <= 0) return;
    s_text[--s_len] = 0;
    text_changed(ctx);
}

static void clear_all(ui_ctx_t *ctx)
{
    mre_stop();
    if (s_dec) {
        s_mlen = 0;
        s_min[0] = 0;
        if (s_sc_ready) ui_scroll_to_top(&s_sc);
    } else {
        s_len = 0;
        s_text[0] = 0;
    }
    text_changed(ctx);
}

/* ================= 播放 ================= */
static void do_play(ui_ctx_t *ctx)
{
    if (mre_playing()) { mre_stop(); redraw(ctx); return; }

    const char *src = s_dec ? s_shown : s_text;      /* 码→文 时播放译出的文本 */
    if (!src[0]) { os_dialog_toast(ctx, "先输入内容"); return; }

    mr_plan_t p;
    if (mr_build_plan(src, &p) != 0) { os_dialog_toast(ctx, "输入过长"); return; }
    if (mr_char_count(&p) == 0) { os_dialog_toast(ctx, "没有可播放的字符"); return; }

    int rc = mre_play(src, s_wpm, s_tone);
    if (rc != 0) { os_dialog_toast(ctx, "播放启动失败"); return; }
    if (p.bad_cnt > 0) os_dialog_toast_ms(ctx, "已跳过不支持字符", 900);
    else if (mre_muted()) os_dialog_toast_ms(ctx, "系统已静音", 900);
    s_prog_last = 0;
    redraw(ctx);
}

/* 试听单个字符 (查表页 / 与文本播放同一实现) */
static void preview_char(ui_ctx_t *ctx, char c)
{
    char buf[2] = { c, 0 };
    if (!mr_encode(c)) { os_dialog_toast(ctx, "该字符无摩斯码"); return; }
    int rc = mre_play(buf, s_wpm, s_tone);
    if (rc != 0) { os_dialog_toast(ctx, "音频不可用"); return; }
    if (mre_muted()) os_dialog_toast_ms(ctx, "系统已静音", 900);
    redraw(ctx);
}

/* ================= 弹窗: 速度 / 音调 / 符号 ================= */
static void wheel_cb(ui_ctx_t *ctx, int result, void *ud)
{
    int which = (int)(intptr_t)ud;
    if (result < 0) { redraw(ctx); return; }
    if (which == 0) s_wpm = (uint8_t)(WPM_MIN + result);
    else            s_tone = (uint16_t)(TONE_MIN + result * TONE_STEP);
    save_cfg();
    redraw(ctx);
}

static void open_wpm(ui_ctx_t *ctx)
{
    static const char *items[WPM_MAX - WPM_MIN + 1];
    static char buf[WPM_MAX - WPM_MIN + 1][10];
    for (int i = 0; i < WPM_MAX - WPM_MIN + 1; i++) {
        snprintf(buf[i], sizeof(buf[i]), "%d WPM", WPM_MIN + i);
        items[i] = buf[i];
    }
    os_dialog_wheel(ctx, items, WPM_MAX - WPM_MIN + 1, s_wpm - WPM_MIN, wheel_cb);
}

static void open_tone(ui_ctx_t *ctx)
{
    static const char *items[TONE_N];
    static char buf[TONE_N][10];
    for (int i = 0; i < TONE_N; i++) {
        snprintf(buf[i], sizeof(buf[i]), "%d Hz", TONE_MIN + i * TONE_STEP);
        items[i] = buf[i];
    }
    os_dialog_wheel(ctx, items, TONE_N, (s_tone - TONE_MIN) / TONE_STEP, wheel_cb);
}

static void sym_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    if (result < 0) { redraw(ctx); return; }
    char c = mr_charset_at((uint8_t)(36 + result));      /* 标点段从 36 开始 */
    append_char(ctx, c);
    redraw(ctx);
}

static void open_symbols(ui_ctx_t *ctx)
{
    static const char *items[18];
    static char buf[18][4];
    int n = (int)mr_charset_count() - 36;
    if (n > 18) n = 18;
    for (int i = 0; i < n; i++) {
        buf[i][0] = mr_charset_at((uint8_t)(36 + i));
        buf[i][1] = 0;
        items[i] = buf[i];
    }
    os_dialog_list(ctx, "插入符号", items, n, 0, sym_cb, NULL);
}

/* ================= 渲染 ================= */
static void render_tabs(st7305_handle_t *l)
{
    static const char *nm[2] = { "转换", "查表" };
    for (int i = 0; i < 2; i++) {
        int x0 = i * 96, x1 = x0 + 95;
        bool on = (s_view == i);
        if (on) fill_rect(l, x0, 0, x1, TAB_H - 1, ST7305_COLOR_BLACK);
        else    draw_rect_outline(l, x0, 0, x1, TAB_H - 1, ST7305_COLOR_BLACK);
        draw_text(l, x0 + (96 - text_width(nm[i])) / 2, 1, nm[i], on);
    }
    /* 速度 / 音调 (可点, 弹滚轮) */
    char b[16];
    draw_rect_outline(l, 200, 2, 288, TAB_H - 3, ST7305_COLOR_BLACK);
    snprintf(b, sizeof(b), "%dWPM", s_wpm);
    draw_small(l, 204, 7, b);
    draw_rect_outline(l, 296, 2, 396, TAB_H - 3, ST7305_COLOR_BLACK);
    snprintf(b, sizeof(b), "%dHz", s_tone);
    draw_small(l, 300, 7, b);
}

static void render_convert(st7305_handle_t *l)
{
    /* 文本区: 2 行 × 24px, 每行 24 字符 (上限 48 正好两行)
     * 编码模式显示原文, 解码模式显示译出的文本 */
    char line[TEXT_COLS + 1];
    int slen = (int)strlen(s_shown);
    for (int r = 0; r < 2; r++) {
        int st = r * TEXT_COLS;
        if (st >= slen) { line[0] = 0; }
        else {
            int n = slen - st; if (n > TEXT_COLS) n = TEXT_COLS;
            memcpy(line, s_shown + st, (size_t)n);
            line[n] = 0;
        }
        if (line[0]) draw_text(l, 6, TEXT_Y + r * TEXT_LH, line, false);
    }
    if (slen == 0) {
        if (!s_dec) draw_text(l, 6, TEXT_Y, "输入英文后按播放", false);
        else        draw_text(l, 6, TEXT_Y, "用下方 点/划 输入", false);
    }

    /* 摩斯串区: 12px, 5 行可视, 超出可拖动 */
    if (s_morse_lines > MORSE_LINES) {
        int offl = ui_scroll_offset(&s_sc) / MORSE_LH;
        if (offl < 0) offl = 0;
        for (int i = 0; i < MORSE_LINES; i++) {
            int li = offl + i;
            if (li >= s_morse_lines) break;
            char tmp[MORSE_COLS + 2];
            int st = li * MORSE_COLS;
            int n = (int)strlen(s_morse) - st;
            if (n <= 0) break;
            if (n > MORSE_COLS) n = MORSE_COLS;
            memcpy(tmp, s_morse + st, (size_t)n);
            tmp[n] = 0;
            draw_small(l, MORSE_X, MORSE_Y + i * MORSE_LH, tmp);
        }
        ui_scroll_drawbar(&s_sc, l, 396, MORSE_Y, MORSE_Y + MORSE_H);
    } else {
        int len = (int)strlen(s_morse);
        int li = 0;
        for (int i = 0; i < len && li < MORSE_LINES; i += MORSE_COLS, li++) {
            char tmp[MORSE_COLS + 2];
            int n = len - i; if (n > MORSE_COLS) n = MORSE_COLS;
            memcpy(tmp, s_morse + i, (size_t)n);
            tmp[n] = 0;
            draw_small(l, MORSE_X, MORSE_Y + li * MORSE_LH, tmp);
        }
    }

    /* 进度条 (仅播放中) */
    if (mre_playing() && mre_total() > 0) {
        int pct = mre_progress() * 100 / mre_total();
        draw_rect_outline(l, 6, PROG_Y, 394, PROG_Y + 5, ST7305_COLOR_BLACK);
        int w = (390 * pct) / 100;
        if (w > 2) fill_rect(l, 7, PROG_Y + 1, 6 + w, PROG_Y + 4, ST7305_COLOR_BLACK);
    }

    if (!s_dec) {
        /* 文→码: 键盘输入 */
        static const char *lbl[BTN_N] = { "播放", "退格", "空格", "清空", "符号", "解码" };
        for (int i = 0; i < BTN_N; i++) {
            const char *t = lbl[i];
            if (i == 0 && mre_playing()) t = "停止";
            draw_btn(l, i, t, i == 0 && mre_playing());
        }
        kbd_draw_rows(l, KBD_BASE_Y, &s_kbd, s_press_k, KBD_SHOW);
    } else {
        /* 码→文: 四个功能键 + 三个大按键 */
        static const char *lbl2[4] = { "退格", "清空", "播放", "编码" };
        for (int i = 0; i < 4; i++) {
            const char *t = lbl2[i];
            bool act = false;
            int x0 = 4 + i * 94;
            if (i == 2 && mre_playing()) { t = "停止"; act = true; }
            if (act) fill_rect(l, x0, BTN_Y, x0 + 87, BTN_Y + BTN_H - 1, ST7305_COLOR_BLACK);
            else     draw_rect_outline(l, x0, BTN_Y, x0 + 87, BTN_Y + BTN_H - 1, ST7305_COLOR_BLACK);
            draw_text(l, x0 + (88 - text_width(t)) / 2, BTN_Y + 4, t, act);
        }
        static const char *big[3] = { ".", "-", "/" };
        for (int i = 0; i < 3; i++) {
            int x0 = 4 + i * 128, x1 = x0 + 123;
            draw_rect_outline(l, x0, 190, x1, 290, ST7305_COLOR_BLACK);
            int tw = text_width(big[i]);
            draw_text(l, x0 + (124 - tw) / 2, 225, big[i], false);
        }
    }
}

static void render_chart(st7305_handle_t *l)
{
    static const char *cat[3] = { "字母", "数字", "标点" };
    for (int i = 0; i < 3; i++) {
        int x0 = 2 + i * 132, x1 = x0 + 128;
        bool on = (s_cat == i);
        if (on) fill_rect(l, x0, 30, x1, 53, ST7305_COLOR_BLACK);
        else    draw_rect_outline(l, x0, 30, x1, 53, ST7305_COLOR_BLACK);
        draw_text(l, x0 + (128 - text_width(cat[i])) / 2, 30, cat[i], on);
    }

    int base = (s_cat == 0) ? 0 : (s_cat == 1 ? 26 : 36);
    int n = (s_cat == 0) ? 26 : (s_cat == 1 ? 10 : 18);
    for (int i = 0; i < n; i++) {
        int r = i / GRID_COLS, c = i % GRID_COLS;
        int x0 = GRID_X + c * GRID_CW, y0 = GRID_Y + r * GRID_CH;
        int x1 = x0 + GRID_CW - 1, y1 = y0 + GRID_CH - 1;
        draw_rect_outline(l, x0, y0, x1, y1, ST7305_COLOR_BLACK);
        char ch = mr_charset_at((uint8_t)(base + i));
        const char *code = mr_charset_code((uint8_t)(base + i));
        char s[2] = { ch, 0 };
        draw_text(l, x0 + (GRID_CW - 16) / 2, y0 + 1, s, false);
        draw_small(l, x0 + 3, y0 + 28, code);
    }
}

static void morse_render(ui_ctx_t *ctx)
{
    st7305_handle_t *l = ctx->lcd;
    if (!l) return;
    st7305_clear(l, ST7305_COLOR_WHITE);
    render_tabs(l);
    if (s_view == 0) render_convert(l);
    else             render_chart(l);
}

/* ================= 触摸 ================= */
static bool chart_touch(ui_ctx_t *ctx, int x, int y)
{
    if (y >= 30 && y < 54) {                       /* 分类 */
        for (int i = 0; i < 3; i++) {
            int x0 = 2 + i * 132;
            if (x >= x0 && x < x0 + 128) {
                if (s_cat != i) { s_cat = i; redraw(ctx); }
                return true;
            }
        }
        return false;
    }
    if (y < GRID_Y) return false;
    int r = (y - GRID_Y) / GRID_CH, c = (x - GRID_X) / GRID_CW;
    if (r < 0 || r >= GRID_ROWS || c < 0 || c >= GRID_COLS) return false;
    int base = (s_cat == 0) ? 0 : (s_cat == 1 ? 26 : 36);
    int n = (s_cat == 0) ? 26 : (s_cat == 1 ? 10 : 18);
    int i = r * GRID_COLS + c;
    if (i < 0 || i >= n) return false;
    preview_char(ctx, mr_charset_at((uint8_t)(base + i)));
    return true;
}

static bool morse_touch(ui_ctx_t *ctx, int x, int y)
{
    if (x < 0 || y < 0) return false;

    /* 顶部 Tab 与参数 */
    if (y < TAB_H) {
        if (x < 96)      { if (s_view != 0) { s_view = 0; redraw(ctx); } return true; }
        else if (x < 192){ if (s_view != 1) { s_view = 1; mre_stop(); redraw(ctx); } return true; }
        else if (x >= 200 && x < 290) { open_wpm(ctx);  return true; }
        else if (x >= 296 && x < 398) { open_tone(ctx); return true; }
        return false;
    }

    if (s_view == 1) return chart_touch(ctx, x, y);

    if (s_dec) {
        /* 码→文: 上方 4 键 + 下方 3 个大键 */
        if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
            for (int i = 0; i < 4; i++) {
                int x0 = 4 + i * 94;
                if (x < x0 || x >= x0 + 88) continue;
                switch (i) {
                case 0: mre_stop(); morin_back(ctx); break;
                case 1: clear_all(ctx); break;
                case 2: do_play(ctx); break;
                case 3: s_dec = 0; mre_stop(); text_changed(ctx); break;
                }
                return true;
            }
            return false;
        }
        if (y >= 190 && y <= 290) {
            for (int i = 0; i < 3; i++) {
                int x0 = 4 + i * 128;
                if (x >= x0 && x < x0 + 124) {
                    morin_append(ctx, (i == 0) ? '.' : (i == 1 ? '-' : '/'));
                    return true;
                }
            }
        }
        return false;
    }

    /* 文→码 按钮行 */
    if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
        for (int i = 0; i < BTN_N; i++) {
            int x0 = 4 + i * (BTN_W + BTN_GAP);
            if (x >= x0 && x < x0 + BTN_W) {
                switch (i) {
                case 0: do_play(ctx); break;
                case 1: mre_stop(); backspace(ctx); break;
                case 2: append_char(ctx, ' '); break;
                case 3: clear_all(ctx); break;
                case 4: open_symbols(ctx); break;
                case 5: s_dec = 1; mre_stop(); text_changed(ctx); break;
                }
                return true;
            }
        }
        return false;
    }

    /* 键盘 */
    if (y < KBD_BASE_Y || y >= S_H) return false;
    int idx = kbd_hit_rows(&s_kbd, KBD_BASE_Y, x, y, KBD_SHOW);
    if (idx < 0) return false;
    s_press_k = idx;
    int act = kbd_press(&s_kbd, idx);
    if (act == KBD_ACT_CHAR)      append_char(ctx, kbd_char(&s_kbd, idx));
    else if (act == KBD_ACT_BKSP) backspace(ctx);
    else if (act == KBD_ACT_SPACE) append_char(ctx, ' ');
    else if (act == KBD_ACT_TAB || act == KBD_ACT_ENTER) do_play(ctx);
    redraw(ctx);
    return true;
}

/* ================= 每帧 ================= */
static void morse_poll(ui_ctx_t *ctx)
{
    /* 松手清高亮 */
    static bool prev_down;
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    if (!down && prev_down && s_press_k >= 0) { s_press_k = -1; redraw(ctx); }
    prev_down = down;

    /* 摩斯串滚动: 手势起点在本区内才接管整段(含松手), 避免松手事件丢失导致拖动态卡住 */
    if (s_view == 0 && s_sc_ready) {
        if (down && ty >= MORSE_Y && ty < MORSE_Y + MORSE_H) s_sc_touch = true;
        if (s_sc_touch) {
            if (ui_scroll_touch_xy(&s_sc, tx, ty, down)) redraw(ctx);
            if (!down) s_sc_touch = false;
        }
        if (ui_scroll_poll(&s_sc)) redraw(ctx);
    }

    /* 播放状态变化 → 重绘 (按钮/进度) */
    bool playing = mre_playing();
    int prog = mre_progress();
    if (playing != s_playing_last || (playing && prog != s_prog_last)) {
        s_playing_last = playing;
        s_prog_last = prog;
        redraw(ctx);
        if (!playing && !mre_audio_ok()) os_dialog_toast(ctx, "音频不可用");
    }
}

/* ================= 按键 ================= */
static void morse_action(ui_ctx_t *ctx, os_action_t a)
{
    if (a == OS_ACTION_BACK) {
        if (s_view == 1) { s_view = 0; redraw(ctx); return; }   /* 查表 → 转换 */
        os_request_exit_confirm(ctx);                           /* 转换 → 退出确认 */
        return;
    }
    if (a == OS_ACTION_CONFIRM) {
        if (s_view == 0) do_play(ctx);
        return;
    }
}

/* ================= 生命周期 ================= */
static void morse_enter(ui_ctx_t *ctx)
{
    load_cfg();
    s_len = 0;
    s_text[0] = 0;
    s_mlen = 0;
    s_min[0] = 0;
    s_dec = 0;
    s_view = 0;
    s_cat = 0;
    s_kbd = kbd_std;                    /* 每次进入重置 Caps/Shift/Fn */
    s_press_k = -1;
    s_prog_last = 0;
    s_playing_last = false;
    s_sc_ready = false;
    s_sc_touch = false;
    rebuild_morse();
    ctx->needs_redraw = true;
}

static void morse_exit(ui_ctx_t *ctx)
{
    (void)ctx;
    mre_stop();                         /* 停播 + flush, 不遗留任务与余音 */
    if (s_sc_ready) { ui_scroll_cancel(&s_sc); s_sc_ready = false; }
}

static const os_module_t s_mod_morse = {
    .name        = "morse",
    .page_id     = OS_PAGE_MORSE,
    .on_enter    = morse_enter,
    .on_exit     = morse_exit,
    .render      = morse_render,
    .action      = morse_action,
    .touch       = morse_touch,
    .poll        = morse_poll,
    .fullscreen  = true,
    .exit_confirm = false,   /* BACK 在页内处理: 查表→转换, 转换→退出确认 */
};

void os_page_morse_register(void) { os_register(&s_mod_morse); }
