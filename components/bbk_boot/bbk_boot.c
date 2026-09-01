#include "bbk_boot.h"
#include "ui_common.h"   /* FONT8X12: 内置 ASCII 点阵, 无需字体初始化 */
#include <string.h>

/* ===== 开机画面: 大号 "LinTOS" 文字 (内联渲染, 替代 400x300 位图, 省 ~15KB flash) =====
 * 用 FONT8X12 (8x12 ASCII 点阵) 放大 BOOT_SCALE 倍绘制, 水平居中偏上.
 * 开机阶段字体(中文)尚未初始化, 只用纯 ASCII 点阵故安全. */
#define BOOT_SCALE 8
#define BOOT_ROWS  12
static void bbk_draw_glyph(st7305_handle_t *lcd, int ox, int oy, const uint8_t *b) {
    for (int r = 0; r < BOOT_ROWS; r++)
        for (int c = 0; c < 8; c++)
            if (b[r] & (0x80 >> c))
                for (int dy = 0; dy < BOOT_SCALE; dy++)
                    for (int dx = 0; dx < BOOT_SCALE; dx++)
                        st7305_draw_pixel(lcd, ox + c * BOOT_SCALE + dx,
                                          oy + r * BOOT_SCALE + dy, ST7305_COLOR_BLACK);
}

void bbk_boot_draw_logo(st7305_handle_t *lcd) {
    if (!lcd) return;
    const char *txt = "LinTOS";
    const int len = (int)strlen(txt);
    const int gw = 8 * BOOT_SCALE, gh = BOOT_ROWS * BOOT_SCALE;
    const int total_w = len * gw;
    int ox = (ST7305_WIDTH - total_w) / 2;
    int oy = (ST7305_HEIGHT - gh) / 2 - 22;   /* 略偏上, 下方可挂副提示 */
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)txt[i];
        if (c < 0x20 || c > 0x7e) continue;
        bbk_draw_glyph(lcd, ox + i * gw, oy, FONT8X12[c - 0x20]);
    }
}