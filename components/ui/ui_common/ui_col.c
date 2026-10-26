/**
 * ui_col.c — 整列离屏缓冲实现（见 ui_col.h）
 */
#include "ui_col.h"
#include "ui_common.h"
#include "font_zh16.h"
#include "font_zh.h"
#include "esp_heap_caps.h"
#include <string.h>

bool ui_col_init(ui_col_t *c, int w, int h)
{
    if (!c) return false;
    if (w <= 0 || h <= 0) return false;
    int bpr = (w + 7) / 8;
    if (c->buf && c->w == w && c->h >= h) {   /* 复用: 只需清 */
        c->h = h;
        ui_col_clear(c);
        return true;
    }
    ui_col_free(c);
    size_t bytes = (size_t)bpr * (size_t)h;
    uint8_t *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return false;
    memset(p, 0, bytes);
    c->buf = p; c->w = w; c->h = h; c->bpr = bpr;
    return true;
}

void ui_col_free(ui_col_t *c)
{
    if (!c) return;
    if (c->buf) free(c->buf);
    c->buf = NULL; c->w = 0; c->h = 0; c->bpr = 0;
}

void ui_col_clear(ui_col_t *c)
{
    if (!c || !c->buf) return;
    memset(c->buf, 0, (size_t)c->bpr * (size_t)c->h);
}

void ui_col_px(ui_col_t *c, int x, int y, bool black)
{
    if (!c || !c->buf) return;
    if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h) return;
    uint8_t *p = c->buf + (size_t)y * c->bpr + (x >> 3);
    uint8_t m = (uint8_t)(0x80u >> (x & 7));
    if (black) *p |= m; else *p &= (uint8_t)~m;
}

void ui_col_fill(ui_col_t *c, int x0, int y0, int x1, int y1, bool black)
{
    if (!c || !c->buf) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->w) x1 = c->w - 1;
    if (y1 >= c->h) y1 = c->h - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int y = y0; y <= y1; y++) ui_col_hline(c, x0, x1, y, black);
}

void ui_col_hline(ui_col_t *c, int x0, int x1, int y, bool black)
{
    if (!c || !c->buf) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y < 0 || y >= c->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= c->w) x1 = c->w - 1;
    if (x0 > x1) return;
    uint8_t *row = c->buf + (size_t)y * c->bpr;
    int b0 = x0 >> 3, b1 = x1 >> 3;
    uint8_t m0 = (uint8_t)(0xFF >> (x0 & 7));
    uint8_t m1 = (uint8_t)(0xFF << (7 - (x1 & 7)));
    if (b0 == b1) {
        uint8_t m = (uint8_t)(m0 & m1);
        if (black) row[b0] |= m; else row[b0] &= (uint8_t)~m;
    } else {
        if (black) {
            row[b0] |= m0;
            for (int b = b0 + 1; b < b1; b++) row[b] = 0xFF;
            row[b1] |= m1;
        } else {
            row[b0] &= (uint8_t)~m0;
            for (int b = b0 + 1; b < b1; b++) row[b] = 0x00;
            row[b1] &= (uint8_t)~m1;
        }
    }
}

void ui_col_text16(ui_col_t *c, int x, int y, const char *s, bool inverted)
{
    if (!c || !c->buf || !s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xE0) == 0xE0) {
            int idx = font_zh16_find_utf8((const char *)p);
            const uint8_t *b = (idx >= 0) ? font_zh16_get_bitmap_by_index(idx) : NULL;
            if (b) {
                for (int r = 0; r < ZH16_FONT_H; r++)
                    for (int cc = 0; cc < ZH16_FONT_W; cc++) {
                        bool on = (b[r * 2 + (cc >> 3)] & (0x80u >> (cc & 7))) != 0;
                        ui_col_px(c, x + cc, y + r, inverted ? !on : on);   /* ★ 正/反都要画 */
                    }
            }
            x += ZH16_FONT_W; p += 3;
        } else {
            bool adv16 = false;
            if (*p >= 0x20 && *p <= 0x7E) {
                /* 优先用 24px 字库里的 ASCII 字形 (24→16 横向采样, 与中文同高同粗);
                 * ★★ 字库里没有该 ASCII 槽时**必须回退 FONT8X12** —— 否则 ASCII 只占位不画,
                 *    现象就是"设备上完全看不到英文/数字" (favorites 页踩过)。
                 *    回退时字宽只有 8px, x 前进量也要跟着变, 否则会留一条 8px 空隙。 */
                int idx = font_zh_find_ascii((uint8_t)*p);
                const uint8_t *zb = (idx >= 0) ? font_zh_get_bitmap_by_index(idx) : NULL;
                if (zb) {
                    for (int row = 0; row < 24; row++)
                        for (int col = 0; col < 16; col++) {
                            int sc = col * 24 / 16;
                            bool on = (zb[row * 3 + (sc >> 3)] & (0x80u >> (sc & 7))) != 0;
                            ui_col_px(c, x + col, y + row, inverted ? !on : on);
                        }
                    adv16 = true;
                } else {
                    const uint8_t *b = FONT8X12[*p - 0x20];
                    for (int cy = 0; cy < 12; cy++)
                        for (int cc = 0; cc < 8; cc++)
                            if (b[cy] & (0x80u >> cc))
                                ui_col_px(c, x + cc, y + 2 + cy, !inverted);
                }
            }
            x += adv16 ? 16 : 8; p++;
        }
    }
}

void ui_col_icon(ui_col_t *c, int cx, int cy, int size, int icon_idx)
{
    if (!c || !c->buf || size <= 0) return;
    int sw = 0, sh = 0;
    const uint8_t *src = ui_main_icon_bitmap(icon_idx, &sw, &sh);
    if (!src || sw <= 0 || sh <= 0) return;
    int bpr = (sw + 7) / 8;
    int x0 = cx - size / 2, y0 = cy - size / 2;
    for (int dy = 0; dy < size; dy++) {
        int sy = dy * sh / size;
        const uint8_t *row = src + sy * bpr;
        for (int dx = 0; dx < size; dx++) {
            int sx = dx * sw / size;
            if (row[sx >> 3] & (0x80u >> (sx & 7))) ui_col_px(c, x0 + dx, y0 + dy, true);
        }
    }
}

void ui_col_icon32(ui_col_t *c, int cx, int cy, int size, const uint8_t *bmp32)
{
    if (!c || !c->buf || !bmp32 || size <= 0) return;
    int x0 = cx - size / 2, y0 = cy - size / 2;
    for (int dy = 0; dy < size; dy++) {
        int sy = dy * 32 / size;
        const uint8_t *row = bmp32 + sy * 4;
        for (int dx = 0; dx < size; dx++) {
            int sx = dx * 32 / size;
            if (row[sx >> 3] & (0x80u >> (sx & 7))) ui_col_px(c, x0 + dx, y0 + dy, true);
        }
    }
}

void ui_col_present(ui_col_t *c, st7305_handle_t *lcd, int x, int y, int win_h, int src_y)
{
    if (!c || !c->buf || !lcd) return;
    if (src_y < 0) src_y = 0;
    if (src_y >= c->h) return;
    if (win_h > c->h - src_y) win_h = c->h - src_y;
    if (win_h <= 0) return;
    st7305_blit_1bit(lcd, x, y, c->w, win_h, c->buf + (size_t)src_y * c->bpr);
}


/* 24px 文本 (os_pane / 设置列表用): CJK 24×24, ASCII 8×12 垂直居中 */
void ui_col_text24(ui_col_t *c, int x, int y, const char *s, bool inverted)
{
    if (!c || !c->buf || !s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xE0) == 0xE0) {
            int idx = font_zh_find_utf8((const char *)p);
            const uint8_t *b = (idx >= 0) ? font_zh_get_bitmap_by_index(idx) : NULL;
            if (b) {
                for (int r = 0; r < ZH_FONT_H; r++)
                    for (int cc = 0; cc < ZH_FONT_W; cc++) {
                        bool on = (b[r * 3 + (cc >> 3)] & (0x80u >> (cc & 7))) != 0;
                        ui_col_px(c, x + cc, y + r, inverted ? !on : on);
                    }
            }
            x += ZH_FONT_W; p += 3;
        } else {
            if (*p >= 0x20 && *p <= 0x7E) {
                /* ★ 24px 行内 ASCII: 用 24px 字库的 ASCII 字形, 24→16 横向采样 ⇒ 与中文同高同粗 */
                /* 先用 24px 字库的 ASCII 字形 (有则最匹配); 取不到就退回 FONT8X12 放大 2 倍 */
                int aidx = font_zh_find_ascii((uint8_t)*p);
                const uint8_t *zb = (aidx >= 0) ? font_zh_get_bitmap_by_index(aidx) : NULL;
                if (zb) {
                    for (int row = 0; row < 24; row++)
                        for (int col = 0; col < 16; col++) {
                            int sc = col * 24 / 16;
                            bool on = (zb[row * 3 + (sc >> 3)] & (0x80u >> (sc & 7))) != 0;
                            ui_col_px(c, x + col, y + row, inverted ? !on : on);
                        }
                } else {
                    const uint8_t *b = FONT8X12[*p - 0x20];
                    for (int cy = 0; cy < 12; cy++)
                        for (int cc = 0; cc < 8; cc++) {
                            bool on = (b[cy] & (0x80u >> cc)) != 0;
                            for (int dy = 0; dy < 2; dy++)
                                for (int dx = 0; dx < 2; dx++)
                                    ui_col_px(c, x + cc * 2 + dx, y + cy * 2 - 2 + dy, inverted ? !on : on);
                        }
                }
            }
            x += 16; p++;
        }
    }
}
