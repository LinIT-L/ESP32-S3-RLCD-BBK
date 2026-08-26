/**
 * ui_common.c — 通用绘制内核 (从 menu_system.c 拆出).
 * 纯绘制: 不依赖 menu_state_t, 仅依赖 st7305 + font_zh 字库 + 内置图标位图.
 */
#include "ui_common.h"
#include "icons_main.inc"   /* main_icons[]: 主菜单 SF 大图标 */
#include "cat_icons.inc"    /* cat_icons[]:  应用管理四分类图标 */
#include <math.h>

/* 8x12 ASCII 字体 (缩放 2x = 16x24, 与中文字体等高) */
const uint8_t FONT8X12[][12] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x00,0x18,0x18}, /* '!' */
    {0x00,0x00,0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x00,0x00,0x00,0x3C,0x7E,0x3C,0x3C,0x7E,0x3C,0x00,0x00,0x00}, /* '#' */
    {0x00,0x00,0x18,0x3C,0x18,0x70,0x0E,0x1C,0x3C,0x18,0x00,0x00}, /* '$' */
    {0x00,0x00,0x00,0x63,0x63,0x06,0x0C,0x18,0x30,0x63,0x63,0x00}, /* '%' */
    {0x00,0x00,0x00,0x1C,0x36,0x1C,0x3B,0x6E,0x66,0x3B,0x00,0x00}, /* '&' */
    {0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ''' */
    {0x00,0x00,0x00,0x0E,0x1C,0x38,0x38,0x38,0x1C,0x0E,0x00,0x00}, /* '(' */
    {0x00,0x00,0x00,0x70,0x38,0x1C,0x1C,0x1C,0x38,0x70,0x00,0x00}, /* ')' */
    {0x00,0x00,0x00,0x00,0x18,0x3C,0x7E,0x3C,0x18,0x00,0x00,0x00}, /* '*' */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00}, /* ',' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x00}, /* '.' */
    {0x00,0x00,0x00,0x06,0x0E,0x1C,0x38,0x70,0x60,0x00,0x00,0x00}, /* '/' */
    {0x00,0x00,0x3C,0x66,0x6E,0x7E,0x76,0x66,0x66,0x3C,0x00,0x00}, /* '0' */
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x7E,0x00,0x00}, /* '1' */
    {0x00,0x00,0x3C,0x66,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00,0x00}, /* '2' */
    {0x00,0x00,0x7E,0x0C,0x18,0x0C,0x06,0x06,0x66,0x3C,0x00,0x00}, /* '3' */
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x0C,0x00,0x00}, /* '4' */
    {0x00,0x00,0x7E,0x60,0x7C,0x06,0x06,0x06,0x66,0x3C,0x00,0x00}, /* '5' */
    {0x00,0x00,0x3C,0x60,0x60,0x7C,0x66,0x66,0x66,0x3C,0x00,0x00}, /* '6' */
    {0x00,0x00,0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00}, /* '7' */
    {0x00,0x00,0x3C,0x66,0x66,0x3C,0x66,0x66,0x66,0x3C,0x00,0x00}, /* '8' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3E,0x06,0x06,0x3C,0x00,0x00}, /* '9' */
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00}, /* ':' */
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30,0x00,0x00}, /* ';' */
    {0x00,0x00,0x00,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00,0x00}, /* '<' */
    {0x00,0x00,0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00,0x00,0x00}, /* '=' */
    {0x00,0x00,0x00,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00,0x00}, /* '>' */
    {0x00,0x00,0x3C,0x66,0x06,0x0C,0x18,0x18,0x00,0x18,0x00,0x00}, /* '?' */
    {0x00,0x00,0x3C,0x66,0x66,0x6E,0x6E,0x60,0x60,0x3C,0x00,0x00}, /* '@' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00,0x00}, /* 'A' */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00}, /* 'B' */
    {0x00,0x00,0x3C,0x66,0x60,0x60,0x60,0x60,0x66,0x3C,0x00,0x00}, /* 'C' */
    {0x00,0x00,0x78,0x6C,0x66,0x66,0x66,0x66,0x6C,0x78,0x00,0x00}, /* 'D' */
    {0x00,0x00,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x7E,0x00,0x00}, /* 'E' */
    {0x00,0x00,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x00,0x00}, /* 'F' */
    {0x00,0x00,0x3C,0x66,0x60,0x60,0x6E,0x66,0x66,0x3C,0x00,0x00}, /* 'G' */
    {0x00,0x00,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00}, /* 'H' */
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00}, /* 'I' */
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00,0x00}, /* 'J' */
    {0x00,0x00,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x66,0x00,0x00}, /* 'K' */
    {0x00,0x00,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00,0x00}, /* 'L' */
    {0x00,0x00,0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x63,0x00,0x00}, /* 'M' */
    {0x00,0x00,0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x66,0x00,0x00}, /* 'N' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00}, /* 'O' */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0x00,0x00}, /* 'P' */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x6E,0x6C,0x36,0x00,0x00}, /* 'Q' */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x66,0x00,0x00}, /* 'R' */
    {0x00,0x00,0x3C,0x66,0x60,0x3C,0x06,0x66,0x66,0x3C,0x00,0x00}, /* 'S' */
    {0x00,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00}, /* 'T' */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00}, /* 'U' */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00}, /* 'V' */
    {0x00,0x00,0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x63,0x00,0x00}, /* 'W' */
    {0x00,0x00,0x66,0x66,0x3C,0x3C,0x3C,0x3C,0x66,0x66,0x00,0x00}, /* 'X' */
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x00,0x00}, /* 'Y' */
    {0x00,0x00,0x7E,0x06,0x0C,0x18,0x30,0x60,0x40,0x7E,0x00,0x00}, /* 'Z' */
    {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00}, /* '[' */
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x03,0x00,0x00,0x00}, /* '\' */
    {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00}, /* ']' */
    {0x00,0x00,0x18,0x3C,0x66,0x42,0x00,0x00,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00}, /* '_' */
    {0x00,0x00,0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x00,0x00,0x3C,0x06,0x3E,0x66,0x66,0x3E,0x00,0x00}, /* 'a' */
    {0x00,0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00}, /* 'b' */
    {0x00,0x00,0x00,0x00,0x3C,0x66,0x60,0x60,0x66,0x3C,0x00,0x00}, /* 'c' */
    {0x00,0x00,0x00,0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00,0x00}, /* 'd' */
    {0x00,0x00,0x00,0x00,0x3C,0x66,0x66,0x7E,0x60,0x3C,0x00,0x00}, /* 'e' */
    {0x00,0x00,0x00,0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00,0x00}, /* 'f' */
    {0x00,0x00,0x00,0x00,0x3E,0x66,0x66,0x66,0x3E,0x06,0x06,0x3C}, /* 'g' */
    {0x00,0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00,0x00}, /* 'h' */
    {0x00,0x00,0x00,0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00,0x00}, /* 'i' */
    {0x00,0x00,0x00,0x06,0x00,0x06,0x06,0x06,0x06,0x06,0x06,0x3C}, /* 'j' */
    {0x00,0x00,0x00,0x60,0x60,0x6C,0x78,0x78,0x6C,0x66,0x00,0x00}, /* 'k' */
    {0x00,0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00}, /* 'l' */
    {0x00,0x00,0x00,0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00,0x00}, /* 'm' */
    {0x00,0x00,0x00,0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00,0x00}, /* 'n' */
    {0x00,0x00,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x00,0x00}, /* 'o' */
    {0x00,0x00,0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x60}, /* 'p' */
    {0x00,0x00,0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06,0x06,0x06}, /* 'q' */
    {0x00,0x00,0x00,0x00,0x6C,0x76,0x60,0x60,0x60,0x60,0x00,0x00}, /* 'r' */
    {0x00,0x00,0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00,0x00,0x00}, /* 's' */
    {0x00,0x00,0x00,0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0x00,0x00}, /* 't' */
    {0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3E,0x00,0x00}, /* 'u' */
    {0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00}, /* 'v' */
    {0x00,0x00,0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00,0x00,0x00}, /* 'w' */
    {0x00,0x00,0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00,0x00,0x00}, /* 'x' */
    {0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x06,0x06,0x3C}, /* 'y' */
    {0x00,0x00,0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00,0x00,0x00}, /* 'z' */
    {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0x00}, /* '{' */
    {0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18}, /* '|' */
    {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0x00}, /* '}' */
    {0x00,0x00,0x00,0x00,0x76,0x7E,0x1F,0x00,0x00,0x00,0x00,0x00}, /* '~' */
};

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
    int py = y + 2;
    for (int row = 0; row < 12; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 8; col++) {
            st7305_color_t color = (bits & (1 << (7 - col))) ? fg : bg;
            st7305_draw_pixel(lcd, x + col, py + row, color);
        }
    }
}
#define ZH_SB_GLYPH    22
#define ZH_SB_OFFSET   ((ZH_FONT_H - ZH_SB_GLYPH) / 2)
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
void draw_text(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted) {
    int cursor_x = x;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) {
            draw_ascii(lcd, cursor_x, y, c, inverted);
            cursor_x += 16;
            str++;
        } else if ((c & 0xE0) == 0xC0) {
            cursor_x += 16;
            str += 2;
        } else if ((c & 0xF0) == 0xE0) {
            draw_zh(lcd, cursor_x, y, str, inverted, 1);
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
    int x = (UI_SCREEN_W - w) / 2;
    if (x < 0) x = 0;
    draw_text(lcd, x, y, str, inverted);
}
void menu_show_center_msg(st7305_handle_t *lcd, const char *line1, const char *line2) {
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    if (line1 && line1[0]) draw_text_centered(lcd, 105, line1, false);
    if (line2 && line2[0]) draw_text_centered(lcd, 165, line2, false);
    st7305_flush(lcd);
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
    if (icon_idx < 0 || icon_idx >= 32) return 0;
    return tbl[icon_idx];
}
void draw_icon_bitmap(st7305_handle_t *lcd, int cx, int cy, int size, int icon_idx) {
    draw_icon_bitmap_stretched(lcd, cx, cy, size, size, icon_idx);
}
void draw_icon_bitmap_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int icon_idx) {
    int mi = main_icon_index(icon_idx);
    if (mi < 0 || mi >= MAIN_ICON_COUNT) return;
    const uint8_t *bmp = main_icons[mi];
    int x0 = cx - size_w / 2;
    int y0 = cy - size_h / 2;
    for (int dy = 0; dy < size_h; dy++) {
        for (int dx = 0; dx < size_w; dx++) {
            int src_x = (dx * MAIN_ICON_W) / size_w;
            int src_y = (dy * MAIN_ICON_H) / size_h;
            int byte_idx = src_y * ((MAIN_ICON_W + 7) / 8) + (src_x / 8);
            int bit = 7 - (src_x % 8);
            if (bmp[byte_idx] & (1 << bit)) {
                st7305_draw_pixel(lcd, x0 + dx, y0 + dy, ST7305_COLOR_BLACK);
            }
        }
    }
}
void draw_main_icon_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int icon_idx) {
    if (icon_idx < 0 || icon_idx >= MAIN_ICON_COUNT) return;
    const uint8_t *bmp = main_icons[icon_idx];
    int x0 = cx - size_w / 2;
    int y0 = cy - size_h / 2;
    for (int dy = 0; dy < size_h; dy++) {
        for (int dx = 0; dx < size_w; dx++) {
            int src_x = (dx * MAIN_ICON_W) / size_w;
            int src_y = (dy * MAIN_ICON_H) / size_h;
            int byte_idx = src_y * ((MAIN_ICON_W + 7) / 8) + (src_x / 8);
            int bit = 7 - (src_x % 8);
            if (bmp[byte_idx] & (1 << bit)) {
                st7305_draw_pixel(lcd, x0 + dx, y0 + dy, ST7305_COLOR_BLACK);
            }
        }
    }
}
void draw_cat_icon_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int cat_idx) {
    if (cat_idx < 0 || cat_idx >= CAT_ICON_COUNT) return;
    const uint8_t *bmp = cat_icons[cat_idx];
    int x0 = cx - size_w / 2;
    int y0 = cy - size_h / 2;
    for (int dy = 0; dy < size_h; dy++) {
        for (int dx = 0; dx < size_w; dx++) {
            int src_x = (dx * CAT_ICON_W) / size_w;
            int src_y = (dy * CAT_ICON_H) / size_h;
            int byte_idx = src_y * ((CAT_ICON_W + 7) / 8) + (src_x / 8);
            int bit = 7 - (src_x % 8);
            if (bmp[byte_idx] & (1 << bit)) {
                st7305_draw_pixel(lcd, x0 + dx, y0 + dy, ST7305_COLOR_BLACK);
            }
        }
    }
}
void menu_draw_cat_icon(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int cat_idx) {
    draw_cat_icon_stretched(lcd, cx, cy, size_w, size_h, cat_idx);
}