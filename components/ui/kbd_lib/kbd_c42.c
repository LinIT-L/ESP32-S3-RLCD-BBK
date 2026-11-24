/**
 * kbd_c42.c — 经典 42 键紧凑键盘 (布局 / 标签 / 字符 / 命中 / 绘制).
 *
 * 数据只有一份: 两套层表 (字母/符号) × 4 行字符串, 与两份历史实现逐字节相同。
 * 差异全部收进 kbd_c42_geo_t: 键宽、排高、键高、分割线画法、字体。
 */
#include "kbd_c42.h"
#include "ui_common.h"      /* draw_text / text_width / fill_rect / draw_hline */
#include "font_zh16.h"      /* 16px 混排字体 (页内版原样) */
#include <string.h>

/* 与虚拟键鼠 / 标准键盘共用同一套 ASCII 字形 (定义于 menu_system.c) */
extern const uint8_t (*FONT8X12)[16];

/* ================= 层表 (字母 / 符号) ================= */
static const char *const s_c42_abc[4] = {
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm,."
};
static const char *const s_c42_sym[4] = {
    "1234567890", "!@#$%^&*()", "-_=+[]{};:'", "\\|/?<>\"~"
};

/* ================= 几何预设 ================= */
void kbd_c42_geo_popup(kbd_c42_geo_t *g, int x0, int y0) {
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->x0 = x0; g->y0 = y0;
    g->row_h = 25; g->key_h = 25;
    g->kw_a = 34;  g->kw_b = 38; g->off_b = 0;
    g->kw_shift = 50; g->kw_c = 30; g->kw_del = 52;
    g->kw_sym = 64; g->kw_space = 218; g->kw_enter = 60;
    g->div_full = true;
    g->font = KBD_C42_FONT_16X24;
}

void kbd_c42_geo_page(kbd_c42_geo_t *g, int x0, int y0, int width) {
    if (!g) return;
    if (width < 100) width = 373;
    memset(g, 0, sizeof(*g));
    const int usable = width - 8;              /* 历史实现: (W-8)/n */
    g->x0 = x0; g->y0 = y0;
    g->row_h = 26; g->key_h = 24;
    g->kw_a = usable / 10;
    g->kw_b = usable / 9;
    g->off_b = (usable - 9 * g->kw_b) / 2;     /* 行2 居中 */
    g->kw_shift = 60; g->kw_c = 33; g->kw_del = 49;
    g->kw_sym = 54;
    g->kw_space = usable - 54 - 72;            /* 余宽给空格 */
    g->kw_enter = 72;
    if (g->kw_space < 40) g->kw_space = 40;
    g->div_full = false;
    g->font = KBD_C42_FONT_16MIX;
}

int kbd_c42_height(const kbd_c42_geo_t *g) {
    return g ? 5 * g->row_h : 0;
}

/* ================= 单键矩形 ================= */
void kbd_c42_key_rect(const kbd_c42_geo_t *g, int id, int *x, int *y, int *w) {
    int rx = 0, ry = 0, rw = 0;
    if (g && id >= 0 && id < KBD_C42_COUNT) {
        if (id < 10) {                     /* 行0 */
            ry = 0; rx = id * g->kw_a; rw = g->kw_a;
        } else if (id < 20) {               /* 行1 */
            ry = 1; rx = (id - 10) * g->kw_a; rw = g->kw_a;
        } else if (id < 29) {               /* 行2 (9 键) */
            ry = 2; rx = g->off_b + (id - 20) * g->kw_b; rw = g->kw_b;
        } else if (id < 39) {               /* 行3: Shift + 8 字母 + 删除 */
            ry = 3;
            if (id == KBD_C42_SHIFT)      { rx = 0; rw = g->kw_shift; }
            else if (id == KBD_C42_DEL)   { rx = g->kw_shift + 8 * g->kw_c; rw = g->kw_del; }
            else { rx = g->kw_shift + (id - KBD_C42_LETTER0) * g->kw_c; rw = g->kw_c; }
        } else {                            /* 行4: &123 + 空格 + 回车 */
            ry = 4;
            if (id == KBD_C42_SYM)        { rx = 0; rw = g->kw_sym; }
            else if (id == KBD_C42_SPACE) { rx = g->kw_sym; rw = g->kw_space; }
            else                          { rx = g->kw_sym + g->kw_space; rw = g->kw_enter; }
        }
        rx += g->x0; ry = g->y0 + ry * g->row_h;
    }
    if (x) *x = rx;
    if (y) *y = ry;
    if (w) *w = rw;
}

/* ================= 标签 / 字符 ================= */
static char c42_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

const char *kbd_c42_label(const kbd_c42_state_t *st, int id) {
    static char b[2];
    const bool sym   = st && st->sym;
    const bool shift = st && st->shift;
    const char *const *rows = sym ? s_c42_sym : s_c42_abc;

    if (id < 0 || id >= KBD_C42_COUNT) return "";
    if (id < 29) {
        char c = (id < 20) ? rows[id / 10][id % 10] : rows[2][id - 20];
        if (shift && !sym) c = c42_upper(c);
        b[0] = c; b[1] = 0;
        return b;
    }
    if (id < 39) {
        if (id == KBD_C42_SHIFT) return sym ? "ABC" : (shift ? "\xe5\xb0\x8f\xe5\x86\x99"     /* 小写 */
                                                             : "\xe5\xa4\xa7\xe5\x86\x99");     /* 大写 */
        if (id == KBD_C42_DEL)   return "\xe5\x88\xa0\xe9\x99\xa4";                             /* 删除 */
        char c = rows[3][id - KBD_C42_LETTER0];
        if (shift && !sym) c = c42_upper(c);
        b[0] = c; b[1] = 0;
        return b;
    }
    if (id == KBD_C42_SYM)   return sym ? "ABC" : "&123";
    if (id == KBD_C42_SPACE) return "\xe7\xa9\xba\xe6\xa0\xbc";                                 /* 空格 */
    return "\xe5\x9b\x9e\xe8\xbd\xa6";                                                          /* 回车 */
}

char kbd_c42_char(const kbd_c42_state_t *st, int id) {
    const bool sym   = st && st->sym;
    const bool shift = st && st->shift;
    const char *const *rows = sym ? s_c42_sym : s_c42_abc;
    char c = 0;
    if (id < 0 || id >= KBD_C42_COUNT) return 0;
    if (id < 29)      c = (id < 20) ? rows[id / 10][id % 10] : rows[2][id - 20];
    else if (id < 39) { if (id < KBD_C42_LETTER0 || id == KBD_C42_DEL) return 0;
                        c = rows[3][id - KBD_C42_LETTER0]; }
    else return 0;
    if (shift && !sym) c = c42_upper(c);
    return c;
}

int kbd_c42_act(int id) {
    if (id == KBD_C42_SHIFT) return KBD_C42_ACT_SHIFT;
    if (id == KBD_C42_DEL)   return KBD_C42_ACT_BKSP;
    if (id == KBD_C42_SYM)   return KBD_C42_ACT_SYM;
    if (id == KBD_C42_SPACE) return KBD_C42_ACT_SPACE;
    if (id == KBD_C42_ENTER) return KBD_C42_ACT_ENTER;
    return KBD_C42_ACT_CHAR;
}

/* ================= 文字渲染 (两套字体) ================= */
#define C42_ASCII_W 8
#define C42_ASCII_H 16

/* 16px 混排 (ASCII 8×16 + 汉字 16×16) —— 与网络检测页原 nd16/nd16w 逐像素一致 */
static int c42_mix_w(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) { w += 16; p += 3; }
        else                     { w += 8;  p++;    }
    }
    return w;
}
static void c42_mix(st7305_handle_t *l, int x, int y, const char *s, bool inv) {
    const st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    const int aofs = (ZH16_FONT_H - C42_ASCII_H) / 2;   /* = 0 */
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) {
            int idx = font_zh16_find_utf8((const char *)p);
            const uint8_t *bm = (idx >= 0) ? font_zh16_get_bitmap_by_index(idx) : NULL;
            if (bm)
                for (int r = 0; r < ZH16_FONT_H; r++)
                    for (int c = 0; c < ZH16_FONT_W; c++)
                        if (bm[r * 2 + (c >> 3)] & (0x80 >> (c & 7)))
                            st7305_draw_pixel(l, x + c, y + r, fg);
            x += ZH16_FONT_W; p += 3;
        } else {
            if (*p >= 0x20 && *p <= 0x7e) {
                const uint8_t *bm = FONT8X12[*p - 0x20];
                for (int cy = 0; cy < C42_ASCII_H; cy++)
                    for (int cc = 0; cc < C42_ASCII_W; cc++)
                        if (bm[cy] & (0x80u >> cc))
                            st7305_draw_pixel(l, x + cc, y + aofs + cy, fg);
            }
            x += 8; p++;
        }
    }
}

/* 按字体取 (宽, 高, 绘制) —— 高用于纵向居中 */
static int  c42_text_h(const kbd_c42_geo_t *g)   { return (g->font == KBD_C42_FONT_16X24) ? 24 : 16; }
static int  c42_text_w(const kbd_c42_geo_t *g, const char *s) {
    return (g->font == KBD_C42_FONT_16X24) ? text_width(s) : c42_mix_w(s);
}
static void c42_text_draw(const kbd_c42_geo_t *g, st7305_handle_t *l, int x, int y,
                          const char *s, bool inv) {
    if (g->font == KBD_C42_FONT_16X24) draw_text(l, x, y, s, inv);
    else                               c42_mix(l, x, y, s, inv);
}

/* ================= 绘制 ================= */

/* 满宽横线该画多长 = 最宽一排的实际宽度 (弹窗版历史实现就是画到 342) */
static int c42_full_w(const kbd_c42_geo_t *g) {
    int w = g->kw_a * 10;
    int r2 = g->off_b + g->kw_b * 9;                  if (r2 > w) w = r2;
    int r3 = g->kw_shift + 8 * g->kw_c + g->kw_del;   if (r3 > w) w = r3;
    int r4 = g->kw_sym + g->kw_space + g->kw_enter;   if (r4 > w) w = r4;
    return w;
}

void kbd_c42_draw(st7305_handle_t *l, const kbd_c42_geo_t *g,
                  const kbd_c42_state_t *st, int press) {
    if (!l || !g) return;

    const int full_w = c42_full_w(g);
    const int th = c42_text_h(g);
    for (int id = 0; id < KBD_C42_COUNT; id++) {
        int x, y, w;
        kbd_c42_key_rect(g, id, &x, &y, &w);
        const bool inv = (id == press);
        fill_rect(l, x, y, x + w - 1, y + g->key_h - 1, inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
        if (!g->div_full) {   /* 页内版: 每键右 + 下分割线 (原 nd_kbd 画法) */
            draw_vline(l, x + w - 1, y, y + g->key_h - 1, ST7305_COLOR_BLACK);
            draw_hline(l, x, x + w - 1, y + g->key_h - 1, ST7305_COLOR_BLACK);
        } else {              /* 弹窗版: 只有右分割线, 行间横线在下面统画 */
            draw_vline(l, x + w - 1, y, y + g->key_h - 1, ST7305_COLOR_BLACK);
        }
        const char *lab = kbd_c42_label(st, id);
        int tw = c42_text_w(g, lab);
        c42_text_draw(g, l, x + (w - tw) / 2, y + (g->key_h - th) / 2, lab, inv);
    }
    if (g->div_full) {        /* 行0..3 的满宽横线 (底排不画, 免得贴弹窗外框) */
        for (int r = 0; r < 4; r++)
            draw_hline(l, g->x0, g->x0 + full_w - 1,
                       g->y0 + (r + 1) * g->row_h - 1, ST7305_COLOR_BLACK);
    }
}

/* ================= 命中 ================= */
int kbd_c42_hit(const kbd_c42_geo_t *g, int x, int y) {
    if (!g) return -1;
    int dy = y - g->y0;
    if (dy < 0) return -1;
    int row = dy / g->row_h;
    if (row > 4) return -1;

    if (row < 2) {
        if (g->kw_a <= 0) return -1;
        int c = (x - g->x0) / g->kw_a;
        return (c >= 0 && c < 10) ? row * 10 + c : -1;
    }
    if (row == 2) {
        if (g->kw_b <= 0) return -1;
        int c = (x - (g->x0 + g->off_b)) / g->kw_b;
        return (c >= 0 && c < 9) ? 20 + c : -1;
    }
    if (row == 3) {
        if (x < g->x0 + g->kw_shift) return KBD_C42_SHIFT;
        if (x >= g->x0 + g->kw_shift + 8 * g->kw_c) return KBD_C42_DEL;
        if (g->kw_c <= 0) return -1;
        int c = (x - (g->x0 + g->kw_shift)) / g->kw_c;
        return (c >= 0 && c < 8) ? KBD_C42_LETTER0 + c : -1;
    }
    if (x < g->x0 + g->kw_sym) return KBD_C42_SYM;
    if (x < g->x0 + g->kw_sym + g->kw_space) return KBD_C42_SPACE;
    if (x < g->x0 + g->kw_sym + g->kw_space + g->kw_enter) return KBD_C42_ENTER;
    return -1;
}
