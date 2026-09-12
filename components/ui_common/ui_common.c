/**
 * ui_common.c — 通用绘制内核 (从 menu_system.c 拆出).
 * 纯绘制: 不依赖 menu_state_t, 仅依赖 st7305 + font_zh 字库 + 内置图标位图.
 */
#include "ui_common.h"
#include <string.h>
#include <math.h>
#include "font_zh16.h"

/* 当前逻辑屏幕尺寸: 默认横屏 400×300; 书库旋转左/右时切为竖屏 300×400 (os_pane/os_dialog 布局按此重排). */
static int s_ui_w = UI_SCREEN_W;
static int s_ui_h = UI_SCREEN_H;
int  ui_screen_w(void) { return s_ui_w; }
int  ui_screen_h(void) { return s_ui_h; }
void ui_set_portrait(bool on) {
    s_ui_w = on ? 300 : UI_SCREEN_W;
    s_ui_h = on ? 400 : UI_SCREEN_H;
}

/* 主菜单/应用管理侧栏图标位图: 由本组件内置 (icons_main.inc / cat_icons.inc), 单一来源.
 * ui_common 为唯一持有者, 其他模块经 ui_common.h extern 引用. */
#include "icons_main.inc"
#include "cat_icons.inc"
/* FONT8X12 (8x12 ASCII) 由 fonts/font_ascii.c 提供 (ui_common.h extern 引用). */

/* ---------- 基础图形原语 ---------- */
void ui_draw_pixel(st7305_handle_t *lcd, int x, int y, st7305_color_t color) {
    st7305_draw_pixel(lcd, x, y, color);
}
void ui_draw_pixel_inv(st7305_handle_t *lcd, int x, int y, st7305_color_t color, bool inverted) {
    st7305_draw_pixel(lcd, x, y, inverted ? (1 - color) : color);
}
void fill_rect(st7305_handle_t *lcd, int x0, int y0, int x1, int y1, st7305_color_t color) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            st7305_draw_pixel(lcd, x, y, color);
}
void draw_hline(st7305_handle_t *lcd, int x0, int x1, int y, st7305_color_t color) {
    for (int x = x0; x <= x1; x++) st7305_draw_pixel(lcd, x, y, color);
}
void draw_vline(st7305_handle_t *lcd, int x, int y0, int y1, st7305_color_t color) {
    for (int y = y0; y <= y1; y++) st7305_draw_pixel(lcd, x, y, color);
}
void draw_rect_outline(st7305_handle_t *lcd, int x0, int y0, int x1, int y1, st7305_color_t color) {
    draw_hline(lcd, x0, x1, y0, color);
    draw_hline(lcd, x0, x1, y1, color);
    draw_vline(lcd, x0, y0, y1, color);
    draw_vline(lcd, x1, y0, y1, color);
}
void draw_rect_outline_side_round(st7305_handle_t *lcd, int x0, int y0, int x1, int y1,
                                  st7305_color_t color, int r, char side) {
    if (r < 1 || r >= (x1 - x0 + 1) / 2) r = 0;
    if (r < 1 || r >= (y1 - y0 + 1) / 2) r = 0;
    if (r < 1) { draw_rect_outline(lcd, x0, y0, x1, y1, color); return; }
    if (side == 'R') {
        draw_vline(lcd, x0, y0, y1, color);
        draw_hline(lcd, x0, x1 - r, y0, color);
        draw_hline(lcd, x0, x1 - r, y1, color);
        draw_vline(lcd, x1, y0 + r, y1 - r, color);
        for (int px = x1 - r; px <= x1; px++) {
            int dx = px - (x1 - r);
            int dy = r - (int)sqrtf((float)(r * r - dx * dx));
            st7305_draw_pixel(lcd, px, y0 + dy, color);
            st7305_draw_pixel(lcd, px, y1 - dy, color);
        }
    } else { /* 'L' */
        draw_vline(lcd, x1, y0, y1, color);
        draw_hline(lcd, x0 + r, x1, y0, color);
        draw_hline(lcd, x0 + r, x1, y1, color);
        draw_vline(lcd, x0, y0 + r, y1 - r, color);
        for (int px = x0; px <= x0 + r; px++) {
            int dx = px - (x0 + r);
            int dy = r - (int)sqrtf((float)(r * r - dx * dx));
            st7305_draw_pixel(lcd, px, y0 + dy, color);
            st7305_draw_pixel(lcd, px, y1 - dy, color);
        }
    }
}
void draw_dialog_frame(st7305_handle_t *lcd, int x, int y, int w, int h, int border) {
    if (border < 1) border = 1;
    fill_rect(lcd, x, y, x + w - 1, y + h - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < border; k++) {
        fill_rect(lcd, x, y + k, x + w - 1, y + k, ST7305_COLOR_BLACK);
        fill_rect(lcd, x, y + h - 1 - k, x + w - 1, y + h - 1 - k, ST7305_COLOR_BLACK);
    }
    for (int k = 0; k < border; k++) {
        fill_rect(lcd, x + k, y, x + k, y + h - 1, ST7305_COLOR_BLACK);
        fill_rect(lcd, x + w - 1 - k, y, x + w - 1 - k, y + h - 1, ST7305_COLOR_BLACK);
    }
}
void draw_slot_marker(st7305_handle_t *lcd, int cx, int cy, int size) {
    int x0 = cx - size / 2, x1 = cx + size / 2;
    int y0 = cy - size / 2, y1 = cy + size / 2;
    for (int x = x0; x <= x1; x += 4)
        st7305_draw_pixel(lcd, x, y0, ST7305_COLOR_BLACK);
    for (int x = x0; x <= x1; x += 4)
        st7305_draw_pixel(lcd, x, y1, ST7305_COLOR_BLACK);
    for (int y = y0; y <= y1; y += 4)
        st7305_draw_pixel(lcd, x0, y, ST7305_COLOR_BLACK);
    for (int y = y0; y <= y1; y += 4)
        st7305_draw_pixel(lcd, x1, y, ST7305_COLOR_BLACK);
    st7305_draw_pixel(lcd, cx, cy - 2, ST7305_COLOR_BLACK);
    st7305_draw_pixel(lcd, cx, cy + 2, ST7305_COLOR_BLACK);
    st7305_draw_pixel(lcd, cx - 2, cy, ST7305_COLOR_BLACK);
    st7305_draw_pixel(lcd, cx + 2, cy, ST7305_COLOR_BLACK);
}

/* ---------- 方向箭头字形 (8x12, 绘制时 1.5x 缩放为 12x18) ---------- */
static const uint8_t ARROW_UP[12] = {
    0x18,0x3C,0x7E,0x7E,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,
};
static const uint8_t ARROW_DOWN[12] = {
    0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x7E,0x7E,0x3C,0x18,0x18,
};
static const uint8_t ARROW_LEFT[12] = {
    0x00,0x00,0x18,0x18,0x38,0xF8,0xF8,0x38,0x18,0x18,0x00,0x00,
};
static const uint8_t ARROW_RIGHT[12] = {
    0x00,0x00,0x18,0x18,0x1C,0x1F,0x1F,0x1C,0x18,0x18,0x00,0x00,
};

void draw_ascii_medium_bmp(st7305_handle_t *lcd, int x, int y, const uint8_t *bmp, bool inverted) {
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 11, y + 17, bg);
    static const int col_map[12] = {0,0,1,2,2,3,4,4,5,6,6,7};
    static const int row_map[18] = {0,0,1,2,2,3,4,4,5,6,6,7,8,8,9,10,10,11};
    for (int dy = 0; dy < 18; dy++) {
        uint8_t bits = bmp[row_map[dy]];
        for (int dc = 0; dc < 12; dc++) {
            int scol = col_map[dc];
            st7305_color_t color = (bits & (1 << (7 - scol))) ? fg : bg;
            st7305_draw_pixel(lcd, x + dc, y + dy, color);
        }
    }
}
void draw_ascii_small(st7305_handle_t *lcd, int x, int y, char c, bool inverted) {
    int idx;
    if (c >= 0x20 && c <= 0x7E) idx = c - 0x20;
    else idx = '?' - 0x20;
    const uint8_t *bmp = FONT8X12[idx];
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 7, y + 11, bg);
    for (int row = 0; row < 12; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 8; col++) {
            st7305_color_t color = (bits & (1 << (7 - col))) ? fg : bg;
            st7305_draw_pixel(lcd, x + col, y + row, color);
        }
    }
}
void draw_ascii_medium(st7305_handle_t *lcd, int x, int y, char c, bool inverted) {
    int idx;
    if (c >= 0x20 && c <= 0x7E) idx = c - 0x20;
    else idx = '?' - 0x20;
    const uint8_t *bmp = FONT8X12[idx];
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 11, y + 17, bg);
    static const int col_map[12] = {0,0,1,2,2,3,4,4,5,6,6,7};
    static const int row_map[18] = {0,0,1,2,2,3,4,4,5,6,6,7,8,8,9,10,10,11};
    for (int dy = 0; dy < 18; dy++) {
        uint8_t bits = bmp[row_map[dy]];
        for (int dx = 0; dx < 12; dx++) {
            st7305_color_t color = (bits & (1 << (7 - col_map[dx]))) ? fg : bg;
            st7305_draw_pixel(lcd, x + dx, y + dy, color);
        }
    }
}
void draw_ascii(st7305_handle_t *lcd, int x, int y, char c, bool inverted) {
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 15, y + 23, bg);
    int idx = font_zh_find_ascii((uint8_t)c);
    const uint8_t *zb = (idx >= 0) ? font_zh_get_bitmap_by_index(idx) : NULL;
    if (zb) {
        for (int row = 0; row < 24; row++) {
            for (int col = 0; col < 16; col++) {
                int sc = col * 24 / 16;
                int bi = row * 3 + (sc >> 3);
                st7305_color_t color = (zb[bi] & (1 << (7 - (sc & 7)))) ? fg : bg;
                st7305_draw_pixel(lcd, x + col, y + row, color);
            }
        }
        return;
    }
    int aidx;
    if (c >= 0x20 && c <= 0x7E) aidx = c - 0x20;
    else aidx = '?' - 0x20;
    const uint8_t *bmp = FONT8X12[aidx];
    for (int row = 0; row < 12; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 8; col++) {
            st7305_color_t color = (bits & (1 << (7 - col))) ? fg : bg;
            st7305_draw_pixel(lcd, x + col * 2,     y + row * 2,     color);
            st7305_draw_pixel(lcd, x + col * 2 + 1, y + row * 2,     color);
            st7305_draw_pixel(lcd, x + col * 2,     y + row * 2 + 1, color);
            st7305_draw_pixel(lcd, x + col * 2 + 1, y + row * 2 + 1, color);
        }
    }
}
void draw_zh(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted, int scale) {
    int idx = font_zh_find_utf8(str);
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    int sz = 24 * scale;
    fill_rect(lcd, x, y, x + sz - 1, y + sz - 1, bg);
    if (idx < 0) {
        if ((uint8_t)str[0] == 0xE3 && (uint8_t)str[1] == 0x80 && (uint8_t)str[2] == 0x80)
            return;
        for (int i = 0; i < sz; i++) {
            st7305_draw_pixel(lcd, x + i, y + i, fg);
            st7305_draw_pixel(lcd, x + i, y + sz - 1 - i, fg);
        }
        return;
    }
    const uint8_t *bmp = zh_font_data[idx];
    int bytes_per_row = (ZH_FONT_W + 7) / 8;
    for (int row = 0; row < ZH_FONT_H; row++) {
        for (int col = 0; col < ZH_FONT_W; col++) {
            int byte_idx = row * bytes_per_row + (col / 8);
            int bit = 7 - (col % 8);
            st7305_color_t color = (bmp[byte_idx] & (1 << bit)) ? fg : bg;
            if (scale == 1) {
                st7305_draw_pixel(lcd, x + col, y + row, color);
            } else {
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        st7305_draw_pixel(lcd, x + col * scale + dx, y + row * scale + dy, color);
            }
        }
    }
}
void draw_zh_small(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted) {
    int idx = font_zh_find_utf8(str);
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 11, y + 11, bg);
    if (idx < 0) return;
    const uint8_t *bmp = zh_font_data[idx];
    int bytes_per_row = (ZH_FONT_W + 7) / 8;
    for (int r = 0; r < 12; r++) {
        for (int c = 0; c < 12; c++) {
            bool any = false;
            for (int dy = 0; dy < 2 && !any; dy++) {
                int row = r * 2 + dy;
                for (int dx = 0; dx < 2 && !any; dx++) {
                    int col = c * 2 + dx;
                    int byte_idx = row * bytes_per_row + (col / 8);
                    if (bmp[byte_idx] & (1 << (7 - (col % 8)))) any = true;
                }
            }
            if (any) st7305_draw_pixel(lcd, x + c, y + r, fg);
            else     st7305_draw_pixel(lcd, x + c, y + r, bg);
        }
    }
}
void draw_ascii_sb(st7305_handle_t *lcd, int x, int y, char c, bool inverted) {
    int idx;
    if (c >= 0x20 && c <= 0x7E) idx = c - 0x20;
    else idx = '?' - 0x20;
    const uint8_t *bmp = FONT8X12[idx];
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 11, y + 15, bg);
    int py = y;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 8; col++) {
            st7305_color_t color = (bits & (1 << (7 - col))) ? fg : bg;
            st7305_draw_pixel(lcd, x + col, py + row, color);
        }
    }
}
#define ZH_SB_GLYPH    22
#define ZH_SB_OFFSET   ((ZH_FONT_H - ZH_SB_GLYPH) / 2)

/* ===== mini-UI 文本控件 (统一 16px: 中文 font_zh16 + 英文 8×16, 与 app_manager/netdect 同款) ===== */
int ui_text16w(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) { w += ZH16_FONT_W; p += 3; } else { w += 8; p++; }
    }
    return w;
}
void ui_text16(st7305_handle_t *lcd, int x, int y, const char *s, bool inverted) {
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    int aofs = (ZH16_FONT_H - 16) / 2;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) {
            int idx = font_zh16_find_utf8((const char *)p);
            const uint8_t *b = (idx >= 0) ? font_zh16_get_bitmap_by_index(idx) : NULL;
            if (b) for (int r = 0; r < ZH16_FONT_H; r++) for (int c = 0; c < ZH16_FONT_W; c++)
                if (b[r * 2 + (c >> 3)] & (0x80 >> (c & 7))) st7305_draw_pixel(lcd, x + c, y + r, fg);
            x += ZH16_FONT_W; p += 3;
        } else {
            if (*p >= 0x20 && *p <= 0x7e) {
                const uint8_t *b = FONT8X12[*p - 0x20];
                for (int cy = 0; cy < 16; cy++) for (int cc = 0; cc < 8; cc++)
                    if (b[cy] & (0x80u >> cc)) st7305_draw_pixel(lcd, x + cc, y + aofs + cy, fg);
            }
            x += 8; p++;
        }
    }
}
void ui_text16c(st7305_handle_t *lcd, int cx, int y, const char *s, bool inverted) {
    ui_text16(lcd, cx - ui_text16w(s) / 2, y, s, inverted);
}
void draw_zh_sb(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted) {
    int idx = font_zh_find_utf8(str);
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 15, y + 15, bg);
    if (idx < 0) return;
    const uint8_t *bmp = zh_font_data[idx];
    int bytes_per_row = (ZH_FONT_W + 7) / 8;
    for (int dy = 0; dy < 16; dy++) {
        int src_row = ZH_SB_OFFSET + (dy * ZH_SB_GLYPH) / 16;
        for (int dx = 0; dx < 16; dx++) {
            int src_col = ZH_SB_OFFSET + (dx * ZH_SB_GLYPH) / 16;
            int byte_idx = src_row * bytes_per_row + (src_col / 8);
            st7305_color_t color = (bmp[byte_idx] & (1 << (7 - (src_col % 8)))) ? fg : bg;
            st7305_draw_pixel(lcd, x + dx, y + dy, color);
        }
    }
}
/* 菜单UI字体大小 (由设置页维护, 引擎内部不走此函数): 16/18/20/22/24, 默认 24 */
int settings_font_size(void);
static int ui_menu_sz(void) {
    static const int t[5] = {16,18,20,22,24};
    int i = settings_font_size();
    return (i >= 0 && i <= 4) ? t[i] : 24;
}
/* 中文 24px font_zh 采样缩放到 sz (覆盖 bg, 不残留) */
static void ui_draw_zh_sz(st7305_handle_t *l, int x, int y, const char *str, bool inv, int sz) {
    int idx = font_zh_find_utf8(str);
    const uint8_t *bmp = (idx >= 0) ? zh_font_data[idx] : NULL;
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    for (int dy = 0; dy < sz; dy++) for (int dx = 0; dx < sz; dx++) {
        int srow = dy*24/sz, scol = dx*24/sz;
        bool on = bmp && (bmp[srow*3 + (scol>>3)] & (0x80>>(scol&7)));
        st7305_draw_pixel(l, x+dx, y+dy, on ? fg : bg);
    }
}
/* ASCII: 用 font_zh 的 ascii 字形(与中文同字体), 24×24 源采样到 高sz/宽sz*16/24 (同 BBK, 英数字与中文同高同风格) */
static void ui_draw_ascii_sz(st7305_handle_t *l, int x, int y, char c, bool inv, int sz) {
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    int idx = font_zh_find_ascii((unsigned char)c);
    const uint8_t *zb = (idx >= 0) ? font_zh_get_bitmap_by_index(idx) : NULL;
    if (zb) {                                    /* 同源: font_zh ascii 24→sz */
        int aw = sz*16/24;
        for (int dy=0; dy<sz; dy++) for (int dx=0; dx<aw; dx++) {
            int srow = dy*24/sz, scol = dx*24/aw;
            bool on = zb[srow*3 + (scol>>3)] & (0x80>>(scol&7));
            st7305_draw_pixel(l, x+dx, y+dy, on ? fg : bg);
        }
    } else {                                     /* 兜底: FONT8X12 8×12→sz */
        int aw = sz*8/12;
        bool has = (c >= 0x20 && c <= 0x7e);
        for (int dy=0; dy<sz; dy++) for (int dx=0; dx<aw; dx++) {
            int srow = dy*12/sz, scol = dx*8/aw;
            bool on = has && (FONT8X12[(unsigned char)c - 0x20][srow] & (0x80>>(scol&7)));
            st7305_draw_pixel(l, x+dx, y+dy, on ? fg : bg);
        }
    }
}

void draw_text(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted) {
    int cursor_x = x;
    int sz = ui_menu_sz();                        /* 菜单UI按设置缩放; 引擎内部不走此函数不受影响 */
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) {
            if (sz != 24) ui_draw_ascii_sz(lcd, cursor_x + (16 - sz*16/24)/2, y, (char)c, inverted, sz);
            else          draw_ascii(lcd, cursor_x, y, c, inverted);
            cursor_x += 16;
            str++;
        } else if ((c & 0xE0) == 0xC0) {
            cursor_x += 16;
            str += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (sz != 24) ui_draw_zh_sz(lcd, cursor_x, y, str, inverted, sz);
            else          draw_zh(lcd, cursor_x, y, str, inverted, 1);
            cursor_x += 24;
            str += 3;
        } else if ((c & 0xF8) == 0xF0) {
            cursor_x += 24;
            str += 4;
        } else {
            str++;
        }
    }
}

/* 带右缘裁切: 超出 x+max_w 之后不再绘制 (不换行). */
void draw_text_capped(st7305_handle_t *lcd, int x, int y, const char *str,
                      bool inverted, int max_w) {
    int cursor_x = x;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) {
            if (cursor_x + 16 > x + max_w) break;
            draw_ascii(lcd, cursor_x, y, c, inverted);
            cursor_x += 16;
            str++;
        } else if ((c & 0xE0) == 0xC0) {
            if (cursor_x + 16 > x + max_w) break;
            cursor_x += 16;
            str += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (cursor_x + 24 > x + max_w) break;
            draw_zh(lcd, cursor_x, y, str, inverted, 1);
            cursor_x += 24;
            str += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (cursor_x + 24 > x + max_w) break;
            cursor_x += 24;
            str += 4;
        } else {
            str++;
        }
    }
}
void draw_label(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted) {
    int cursor_x = x;
    int scale = 1;
    int ascii_w = 16 * scale;
    int zh_w = 24 * scale;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) {
            char ch = c;
            st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
            st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
            fill_rect(lcd, cursor_x, y, cursor_x + ascii_w - 1, y + ascii_w - 1, bg);
            int aidx = font_zh_find_ascii((uint8_t)ch);
            const uint8_t *zbmp = (aidx >= 0) ? font_zh_get_bitmap_by_index(aidx) : NULL;
            if (zbmp) {
                for (int row = 0; row < ascii_w; row++) {
                    for (int col = 0; col < ascii_w; col++) {
                        int sr = row * 24 / ascii_w;
                        int sc = col * 24 / ascii_w;
                        int bi = sr * 3 + (sc >> 3);
                        st7305_color_t color = (zbmp[bi] & (1 << (7 - (sc & 7)))) ? fg : bg;
                        st7305_draw_pixel(lcd, cursor_x + col, y + row, color);
                    }
                }
            } else {
                int idx = (ch >= 0x20 && ch <= 0x7E) ? (ch - 0x20) : ('?' - 0x20);
                const uint8_t *bmp = FONT8X12[idx];
                for (int row = 0; row < 12; row++) {
                    uint8_t bits = bmp[row];
                    for (int col = 0; col < 8; col++) {
                        st7305_color_t color = (bits & (1 << (7 - col))) ? fg : bg;
                        st7305_draw_pixel(lcd, cursor_x + col * 2,     y + row * 2,     color);
                        st7305_draw_pixel(lcd, cursor_x + col * 2 + 1, y + row * 2,     color);
                        st7305_draw_pixel(lcd, cursor_x + col * 2,     y + row * 2 + 1, color);
                        st7305_draw_pixel(lcd, cursor_x + col * 2 + 1, y + row * 2 + 1, color);
                    }
                }
            }
            cursor_x += ascii_w;
            str++;
        } else if ((c & 0xF0) == 0xE0) {
            draw_zh(lcd, cursor_x, y, str, inverted, scale);
            cursor_x += zh_w;
            str += 3;
        } else {
            str += 3;
        }
    }
}
int text_width(const char *str) {
    int w = 0;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) { w += 16; str++; }
        else if ((c & 0xE0) == 0xC0) { w += 16; str += 2; }
        else if ((c & 0xF0) == 0xE0) { w += 24; str += 3; }
        else if ((c & 0xF8) == 0xF0) { w += 24; str += 4; }
        else str++;
    }
    return w;
}
int text_width_bounded(const char *start, const char *end) {
    int w = 0;
    const char *s = start;
    while (s < end) {
        uint8_t c = (uint8_t)*s;
        if (c < 0x80) { w += 16; s++; }
        else if ((c & 0xE0) == 0xC0) { w += 16; s += 2; }
        else if ((c & 0xF0) == 0xE0) { w += 24; s += 3; }
        else if ((c & 0xF8) == 0xF0) { w += 24; s += 4; }
        else s++;
    }
    return w;
}
/* 文本超长裁剪: 只保留前面最多 20 个英文 / 10 个中文 (且总宽不超 max_w),
 * UTF-8 安全 (不切坏多字节字符中间; 输入尾部被外部截断的非法序列直接停止).
 * 返回静态缓冲, 仅供立即绘制. */
const char *text_clip(const char *str, int max_w) {
    static char s_clip[64];
    char *o = s_clip;
    const char *p = str;
    int w = 0, cjk = 0, ascii = 0;
    if (max_w <= 0) max_w = 320;
    while (*p && o - s_clip < (int)sizeof(s_clip) - 4) {
        unsigned char c = (unsigned char)*p;
        int len, cw;
        if (c < 0x80)                { len = 1; cw = 16; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cw = 16; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cw = 24; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cw = 24; }
        else { p++; continue; }
        if (strlen(p) < (size_t)len) break;      /* 尾部被截断的残缺序列, 丢弃 */
        if (len == 1) {
            if (ascii >= 20 || w + cw > max_w) break;
            ascii++;
        } else {
            if (cjk >= 10 || w + cw > max_w) break;
            cjk++;
        }
        w += cw;
        for (int i = 0; i < len; i++) *o++ = *p++;
    }
    *o = '\0';
    return s_clip;
}
int label_width(const char *str) {
    int w = 0;
    int scale = 1;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) { w += 16 * scale; str++; }
        else if ((c & 0xF0) == 0xE0) { w += 24 * scale; str += 3; }
        else str += 3;
    }
    return w;
}
void draw_label_centered_at(st7305_handle_t *lcd, int cx, int y, const char *str, bool inverted) {
    int w = label_width(str);
    int x = cx - w / 2;
    if (x < 0) x = 0;
    if (x + w > UI_SCREEN_W) x = UI_SCREEN_W - w;
    draw_label(lcd, x, y, str, inverted);
}
void draw_text_centered(st7305_handle_t *lcd, int y, const char *str, bool inverted) {
    int w = text_width(str);
    int x = (ui_screen_w() - w) / 2;   /* 跟随当前屏幕宽度 (竖屏 300, 横屏 400) */
    if (x < 0) x = 0;
    draw_text(lcd, x, y, str, inverted);
}
int draw_text_wrapped(st7305_handle_t *lcd, int x0, int x1, int y0,
                      const char *text, bool inverted, int align, int line_h) {
    int max_w = x1 - x0;
    if (max_w < 16) max_w = 16;
    int y = y0;
    int lines = 0;
    const char *p = text;
    char para[128];
    while (*p && lines < 16) {
        int plen = 0;
        while (*p && *p != '\n' && plen < (int)sizeof(para) - 1) {
            para[plen++] = *p++;
        }
        para[plen] = '\0';
        if (*p == '\n') p++;
        int i = 0;
        while (i < plen && lines < 16) {
            int line_w = 0;
            int j = i;
            int last_break = -1;
            while (j < plen) {
                uint8_t c = (uint8_t)para[j];
                int cw;
                if (c < 0x80) cw = 16;
                else if ((c & 0xF0) == 0xE0) cw = 24;
                else if ((c & 0xE0) == 0xC0) cw = 16;
                else if ((c & 0xF8) == 0xF0) cw = 24;
                else cw = 16;
                if (line_w + cw > max_w && j > i) break;
                if (c == ' ') last_break = j;
                line_w += cw;
                j++;
            }
            int line_len = j - i;
            if (j < plen && last_break > i) {
                line_len = last_break - i + 1;
                j = last_break + 1;
            }
            char buf[64];
            int n = 0;
            for (int k = i; k < i + line_len && n < (int)sizeof(buf) - 1; k++) {
                buf[n++] = para[k];
            }
            while (n > 0 && buf[n - 1] == ' ') n--;
            buf[n] = '\0';
            int draw_x;
            if (align == 1) {
                int w = text_width(buf);
                draw_x = x0 + (max_w - w) / 2;
                if (draw_x < x0) draw_x = x0;
            } else {
                draw_x = x0;
            }
            draw_text(lcd, draw_x, y, buf, inverted);
            y += line_h;
            lines++;
            i = j;
        }
    }
    return lines;
}

/* ---------- 图标绘制 ---------- */
/* 主菜单模块 icon_idx (xmb 复古索引) -> 主菜单 SF 图标索引 */
static int main_icon_index(int icon_idx) {
    static const int tbl[32] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 9, 10, 11, 12, 13, 14, 15,
        1, 3, 8, 6, 6, 0, 6,
        16, 17, 18, 19, 20, 21, 22, 23, 24,
    };
    if (icon_idx < 0) return 0;
    /* 传统 xmb 索引(<32)走重映射表; 之后新增的图标按 main_icons[] 数组下标直接映射 */
    if (icon_idx < 32) return tbl[icon_idx];
    return icon_idx;
}
void draw_icon_bitmap(st7305_handle_t *lcd, int cx, int cy, int size, int icon_idx) {
    draw_icon_bitmap_stretched(lcd, cx, cy, size, size, icon_idx);
}
static void ui_blit_scaled(st7305_handle_t *dev, int x0, int y0, int dst_w, int dst_h,
                           int src_w, int src_h, const uint8_t *src);   /* 前向声明 */
void draw_icon_bitmap_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int icon_idx) {
    int mi = main_icon_index(icon_idx);
    if (mi < 0 || mi >= MAIN_ICON_COUNT) return;
    ui_blit_scaled(lcd, cx - size_w / 2, cy - size_h / 2, size_w, size_h,
                   MAIN_ICON_W, MAIN_ICON_H, main_icons[mi]);
}
/* 高性能 1bpp 缩放直写: 直接操作 ST7305 fb (PSRAM), 跳过 st7305_draw_pixel 逐点开销.
 * 速度比逐点缩放快 5-10x, 主菜单轮盘多图标渲染流畅性的关键.
 * src: src_w x src_h 1bpp, MSB-first; 最近邻缩放到 dst_w x dst_h 画到 (x0,y0). */
static void ui_blit_scaled(st7305_handle_t *dev, int x0, int y0, int dst_w, int dst_h,
                           int src_w, int src_h, const uint8_t *src) {
    if (!dev || !dev->fb || !src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    const int bytes_per_row = (src_w + 7) >> 3;
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = (dy * src_h) / dst_h;
        const uint8_t *row = src + sy * bytes_per_row;
        int y = y0 + dy;
        if ((uint32_t)y >= (uint32_t)ST7305_HEIGHT) continue;
        int inv_y = ST7305_HEIGHT - 1 - y;
        int y_group = inv_y >> 2;
        int y_sub = inv_y & 3;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx = (dx * src_w) / dst_w;
            uint8_t b = row[sx >> 3];
            if (!(b & (0x80u >> (sx & 7)))) continue;
            int x = x0 + dx;
            if ((uint32_t)x >= (uint32_t)ST7305_WIDTH) continue;
            int x_pair = x >> 1;
            uint8_t mask = (uint8_t)(1u << (7u - ((y_sub << 1) | (x & 1))));
            dev->fb[(uint32_t)x_pair * (ST7305_HEIGHT >> 2) + (uint32_t)y_group] &= ~mask;
        }
    }
}

void draw_main_icon_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int icon_idx) {
    if (icon_idx < 0 || icon_idx >= MAIN_ICON_COUNT) return;
    ui_blit_scaled(lcd, cx - size_w / 2, cy - size_h / 2, size_w, size_h,
                   MAIN_ICON_W, MAIN_ICON_H, main_icons[icon_idx]);
}
/* 通用 1bpp 位图缩放直写 (与主图标同款朝向/缩放), 供诊断等自绘页嵌入自定义图标.
 * src: src_w x src_h 1bpp, MSB-first. */
void draw_1bpp_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h,
                         int src_w, int src_h, const uint8_t *src) {
    if (!src || src_w <= 0 || src_h <= 0) return;
    ui_blit_scaled(lcd, cx - size_w / 2, cy - size_h / 2, size_w, size_h, src_w, src_h, src);
}
void draw_cat_icon_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int cat_idx) {
    if (cat_idx < 0 || cat_idx >= CAT_ICON_COUNT) return;
    ui_blit_scaled(lcd, cx - size_w / 2, cy - size_h / 2, size_w, size_h,
                   CAT_ICON_W, CAT_ICON_H, cat_icons[cat_idx]);
}
/* V1.1.1: 居中提示弹窗 "退出游戏？" 等 (从旧 menu draw_notice_popup 移植).
 * 几何与 gam4980 触摸命中一致: 白底 + 3px 黑边框 + 居中 24px 文字. */
void ui_draw_notice_popup(st7305_handle_t *lcd, const char *text) {
    if (!lcd || !text || !text[0]) return;
    const int BORDER = 3, PAD = 3, TEXT_H = 24;
    int tw = text_width(text);
    const int W = BORDER*2 + PAD*2 + tw;
    const int H = BORDER*2 + PAD*2 + TEXT_H;
    int x = (UI_SCREEN_W - W) / 2, y = (UI_SCREEN_H - H) / 2;
    for (int dy = 0; dy < H; dy++)
        for (int dx = 0; dx < W; dx++)
            st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_WHITE);
    for (int k = 0; k < BORDER; k++) {
        for (int dx = 0; dx < W; dx++) {
            st7305_draw_pixel(lcd, x + dx, y + k, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, x + dx, y + H - 1 - k, ST7305_COLOR_BLACK);
        }
        for (int dy = 0; dy < H; dy++) {
            st7305_draw_pixel(lcd, x + k, y + dy, ST7305_COLOR_BLACK);
            st7305_draw_pixel(lcd, x + W - 1 - k, y + dy, ST7305_COLOR_BLACK);
        }
    }
    draw_text_centered(lcd, y + BORDER + PAD, text, false);
}

