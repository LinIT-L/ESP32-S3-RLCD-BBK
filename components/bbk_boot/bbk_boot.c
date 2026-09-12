#include "bbk_boot.h"
#include "ui_common.h"   /* FONT8X12: 内置 ASCII 点阵, 无需字体初始化 */
#include <string.h>

/* ===== 开机画面: 文字版 "LinTOS" (FONT8X12 点阵放大, 宽度≈200 居中) =====
 * 右下角署名 by: LinIT (小号). 开机阶段字体(中文)尚未初始化, 只用 ASCII 点阵故安全.
 * BOOT_ROWS=12: FONT8X12 实际 12 行 (16 会越界读下一个字形致乱码). */
#define BOOT_SCALE 4    /* 8×4=32px/字 ×6字 ≈192≈200 */
#define BOOT_ROWS  12
static void bbk_draw_glyph(st7305_handle_t *lcd, int ox, int oy, const uint8_t *b, int scale) {
    for (int r = 0; r < BOOT_ROWS; r++)
        for (int c = 0; c < 8; c++)
            if (b[r] & (0x80 >> c))
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        st7305_draw_pixel(lcd, ox + c * scale + dx,
                                          oy + r * scale + dy, ST7305_COLOR_BLACK);
}
static void bbk_draw_txt(st7305_handle_t *lcd, int x, int y, const char *txt, int scale) {
    for (int i = 0; txt[i]; i++) {
        unsigned char c = (unsigned char)txt[i];
        if (c < 0x20 || c > 0x7e) continue;
        bbk_draw_glyph(lcd, x + i * 8 * scale, y, FONT8X12[c - 0x20], scale);
    }
}

void bbk_boot_draw_logo(st7305_handle_t *lcd) {
    if (!lcd) return;
    const char *txt = "LinTOS";
    const int gw = 8 * BOOT_SCALE, gh = BOOT_ROWS * BOOT_SCALE;
    const int total_w = (int)strlen(txt) * gw;
    /* 宽度≈200 水平居中 + 垂直居中 */
    int ox = (ST7305_WIDTH - total_w) / 2;
    int oy = (ST7305_HEIGHT - gh) / 2;
    bbk_draw_txt(lcd, ox, oy, txt, BOOT_SCALE);
    /* 右下角署名: by: LinIT (小号 8×16, 距右下 4px) */
    const char *by = "by: LinIT";
    int bx = ST7305_WIDTH - (int)strlen(by) * 8 - 4;
    int byy = ST7305_HEIGHT - BOOT_ROWS - 4;
    bbk_draw_txt(lcd, bx, byy, by, 1);
}
