/**
 * ui_common.c — 通用绘制内核 (从 menu_system.c 拆出).
 * 纯绘制: 不依赖 menu_state_t, 仅依赖 st7305 + font_zh 字库 + 内置图标位图.
 */
#include "ui_common.h"
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"   /* ui_icon_register_extra: 外来图标放 PSRAM */
#include "font_zh16.h"
#include "glyph.h"           /* ★ 2026-11-22: 汉字取模唯一入口 (宋体/黑体 × 16/24) */

/* 当前逻辑屏幕尺寸: 默认横屏 400×300; 书库旋转左/右时切为竖屏 300×400 (os_pane/os_dialog 布局按此重排). */
static int s_ui_w = UI_SCREEN_W;
static int s_ui_h = UI_SCREEN_H;
int  ui_screen_w(void) { return s_ui_w; }
int  ui_screen_h(void) { return s_ui_h; }
void ui_set_portrait(bool on) {
    s_ui_w = on ? 300 : UI_SCREEN_W;
    s_ui_h = on ? 400 : UI_SCREEN_H;
}
/* ★ 2026-11-22: 旋转方向 → 逻辑宽高的**唯一推导点**。
 *   原来每个调用方都要自己写 `rot == 2 || rot == 3` 再调 ui_set_portrait(),
 *   全仓出现 4 份同样的推导 (书库/阅读器/os_core/壁纸)。这类"同一判据多处重写"
 *   一旦有人写成 `rot == 2` 就只在右竖屏出错, 极难复现。
 *   现在判据只在本函数里存在一次; 调用方直接传旋转方向即可。 */
void ui_set_rotation_mode(int rot) {
    ui_set_portrait(rot == 2 || rot == 3);   /* 2=左竖 3=右竖; 0=横屏 1=横屏180° */
}
bool ui_is_portrait(void) { return s_ui_w != UI_SCREEN_W; }

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

/* ★ 2026-10-02 版本管理整合: 删除 4 个 ARROW_UP/DOWN/LEFT/RIGHT 字形常量。
 *   它们已全仓无引用, 而 IDF 默认带 `-Werror=unused-const-variable`, 一旦不再用
 *   `-Wno-unused-variable` 屏蔽就**直接编译失败** —— 正是这层屏蔽长期掩盖了它们。 */

/* ★★ 2026-11-22 linit「gba 都显示成 abc 了」根因修复 ★★
 *
 * FONT8X12 这个符号名是历史遗留：它指向 font8x16_zh —— **8×16、共 16 行**的字模。
 * 但当年按"8×12"写的绘制循环只扫 12 行 (0..11)，把小写字母的下伸部分
 * (行 12..14, 即 g/j/p/q/y 的尾巴) 整段切掉：
 *     'g' 行 5..11 = .###.##./##..##../##..##../##..##../##..##../##..##../.#####..
 *     —— 与 'a' 的形状几乎一致 ⇒ 文件名 ".gba" 被看成 ".abc"。
 * 这里**完整 16 行**映射 (不越界、不切尾巴)，实现见下面的 ui_blit_ascii_band。
 * ⚠ 不要再退回"只画 12 行"：那等于重新引入这个 bug（顺带还会切掉大写 Q 的撇）。 */

/* ★★ 2026-11-22 第二轮：24px 档英文"显小"的排版学修复 ★★
 *
 * 实测数据 (不是估)：
 *   · font8x16_zh (源字模) —— 大写/数字墨迹恒占第 2..11 行 (cap 10 行)，
 *     第 0/1 行留给重音、第 12..15 行留给下伸；小写 x-height 只有 5..11 (7 行)。
 *   · font_zh.bin (24px 汉字) —— 6818 字里 5466 字从第 1 行起笔，底行 21/22，
 *     墨迹高**中位 21px**。
 * 把源字模当成 16 行等比铺到 24 行 (旧做法) 的后果：
 *   cap = 10 × 1.5 = 15px、x-height = 7 × 1.5 = 10.5px —— 而汉字是 21px，
 *   于是"同一行里英文明显比中文小一圈"，小写字尤其迷你。
 *
 * 正确做法是按**排版带**映射 (不是等比)：把源字模的四条基准线分别落到
 * 目标槽里的对应位置，中间每段独立等比。
 *     源行 0..1  (重音/上标)   →  0..2
 *     源行 2     (cap 顶)      →  2
 *     源行 5     (x-height 顶) →  7
 *     源行 12    (基线)        →  20
 *     源行 15    (下伸底)      →  24
 * 得到 cap 18px / x-height 13px，与 21px 汉字同视觉重量；且 g/j/p/q/y 的尾巴、
 * Q 的撇(到源行 13)、$ 的上下出头全部落在槽内，一个像素都不切。
 *
 * 目标行 = 表值 × h / 24，所以 22px 档 (商店应用名) 也自动协调。
 * ⚠ h < 20 时不走这张表：24 行表压到 16 行会出现相邻源行合并，
 *   横画容易丢，小字号仍用原等比 —— 那里的 ASCII 本来就是 1:1 像素。
 * ⛔ 不要再退回"16 行等比铺满"：那等于把英文重新缩小 20%~44%。 */
static const uint8_t s_ascii_band[17] = { 0, 1, 2, 3, 5, 7, 9, 11, 13, 14, 16, 18, 20, 21, 22, 23, 24 };

/* 源行 row → 目标行区间 [*py0, *py1)，结果保证 0 <= y0 < y1 <= h 且 y1 > y0。
 * 导出给列缓冲渲染 (ui_col / page_app_manager) 用 —— 映射规则只此一份。 */
void ui_ascii_band_span(int row, int h, int *py0, int *py1) {
    int y0, y1;
    if (h >= 20) {
        y0 = s_ascii_band[row]     * h / 24;
        y1 = s_ascii_band[row + 1] * h / 24;
    } else {
        y0 = row * h / 16;
        y1 = (row + 1) * h / 16;
    }
    if (y0 >= h) y0 = h - 1;
    if (y1 <= y0) y1 = y0 + 1;      /* 量化合并: 保底 1 行, 绝不丢横画 */
    if (y1 > h) y1 = h;
    if (y1 <= y0) y1 = y0 + 1;
    *py0 = y0;
    *py1 = y1;
}

/* 完整 16 行源字模 → 目标 w×h，纵向按排版带 (h>=20) 或等比 (h<20)。
 * 导出给 ui_col.c / page_app_manager.c 共用 —— 全项目只保留这一份映射规则。 */
void ui_blit_ascii_band(st7305_handle_t *lcd, int x, int y, const uint8_t *bmp,
                        int w, int h, st7305_color_t fg) {
    if (w < 4) w = 4;
    if (h < 4) h = 4;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = bmp[row];
        if (!bits) continue;
        int y0, y1;
        ui_ascii_band_span(row, h, &y0, &y1);
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80u >> col))) continue;
            int x0 = col * w / 8;
            int x1 = (col + 1) * w / 8;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++)
                    st7305_draw_pixel(lcd, x + xx, y + yy, fg);
        }
    }
}

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
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 7, y + 15, bg);
    /* 源是 8×16 完整 16 行, 按 1:1 画 (数字/大写像素高与旧版完全一致, 不再被压小;
     * 只是多占了下伸字母的 12..14 行 ⇒ 清屏高度也要跟着到 y+15) */
    ui_blit_ascii_band(lcd, x, y, FONT8X12[idx], 8, 16, fg);
}
void draw_ascii_medium(st7305_handle_t *lcd, int x, int y, char c, bool inverted) {
    int idx;
    if (c >= 0x20 && c <= 0x7E) idx = c - 0x20;
    else idx = '?' - 0x20;
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    fill_rect(lcd, x, y, x + 11, y + 17, bg);
    /* 完整 16 行 → 12×18 占位 (原 12 行会切掉 g/j/p/q/y 的下伸) */
    ui_blit_ascii_band(lcd, x, y, FONT8X12[idx], 12, 18, fg);
}
/* 菜单 24px 字体: 兜底走 ui_blit_ascii_band (完整 16 行 + 排版带, 见文件上方说明) */
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
    /* 完整 16 行映射到 16×24 占位 (背景已由上面的 fill_rect 清过) */
    ui_blit_ascii_band(lcd, x, y, FONT8X12[aidx], 16, 24, fg);
}
/* ★ 2026-11-08: 透明 ASCII —— 字形与 draw_ascii 逐像素一致, 唯一区别是
 *   **不铺背景**(draw_ascii 会先 fill_rect(bg), 把底纹/反色底盖成一块白/黑矩形)。
 *   用途: 数独这类"格子里有底纹(50% 菱形网格)"的界面, 数字必须叠在底纹上,
 *   否则每个数字周围都出现一块白色方块。 */
void draw_ascii_t(st7305_handle_t *lcd, int x, int y, char c, bool inverted) {
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    int idx = font_zh_find_ascii((uint8_t)c);
    const uint8_t *zb = (idx >= 0) ? font_zh_get_bitmap_by_index(idx) : NULL;

    /* 先把字形光栅化到 ink[24][24], 再统一处理描边 + 绘制。 */
    bool ink[24][24];
    memset(ink, 0, sizeof ink);
    if (zb) {
        for (int row = 0; row < 24; row++) {
            for (int col = 0; col < 16; col++) {
                int sc = col * 24 / 16;
                int bi = row * 3 + (sc >> 3);
                if (zb[bi] & (1 << (7 - (sc & 7)))) ink[row][col] = true;
            }
        }
    } else {
        int ai = (c >= 0x20 && c <= 0x7E) ? (c - 0x20) : ('?' - 0x20);
        const uint8_t *bp = FONT8X12[ai];
        /* 完整 16 行 + 24 行槽的排版带 (与 draw_ascii 逐像素同源) */
        for (int row = 0; row < 16; row++) {
            int y0, y1;
            ui_ascii_band_span(row, 24, &y0, &y1);
            for (int col = 0; col < 8; col++) {
                if (!(bp[row] & (1 << (7 - col)))) continue;
                int pc = col * 2;
                for (int pr = y0; pr < y1; pr++)
                    ink[pr][pc] = ink[pr][pc + 1] = true;
            }
        }
    }

    /* ★ 2026-11-08 linit: "数字周围加一像素强制性白" —— 是**沿笔画描边 1px**,
     *   不是把整个墨迹包围盒填白。之前用 bbox 填充, 数字(如 7/4)墨迹铺得很开,
     *   白块几乎盖满整格, 看上去"灰色底纹没包住数字"。反色格不描边。
     *   实现: 第一遍给每个笔画点的 8 邻域画白, 第二遍画字本身 —— 只有笔画外沿 1px 变白。 */
    if (!inverted) {
        for (int r = 0; r < 24; r++) {
            for (int col = 0; col < 16; col++) {
                if (!ink[r][col]) continue;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = col + dx, ny = r + dy;
                        if (nx < 0 || ny < 0) continue;
                        st7305_draw_pixel(lcd, x + nx, y + ny, ST7305_COLOR_WHITE);
                    }
                }
            }
        }
    }
    for (int r = 0; r < 24; r++)
        for (int col = 0; col < 16; col++)
            if (ink[r][col]) st7305_draw_pixel(lcd, x + col, y + r, fg);
}

void draw_zh(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted, int scale) {
    /* ★ 2026-11-22: 取模改走 glyph (宋体/黑体可切), 不再直接读 zh_font_data。
     *   scale>1 仍是块放大, 但源字形已经是"当前字体族 24px"合成结果。 */
    st7305_color_t bg = inverted ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    int sz = 24 * scale;
    fill_rect(lcd, x, y, x + sz - 1, y + sz - 1, bg);

    uint8_t g[24 * 3];                              /* 24px 1bpp: 每行 3 字节 */
    if (!glyph_render(str, 24, g)) {
        if ((uint8_t)str[0] == 0xE3 && (uint8_t)str[1] == 0x80 && (uint8_t)str[2] == 0x80)
            return;
        for (int i = 0; i < sz; i++) {
            st7305_draw_pixel(lcd, x + i, y + i, fg);
            st7305_draw_pixel(lcd, x + i, y + sz - 1 - i, fg);
        }
        return;
    }
    for (int row = 0; row < 24; row++) {
        for (int col = 0; col < 24; col++) {
            bool on = (g[row * 3 + (col >> 3)] & (uint8_t)(0x80u >> (col & 7))) != 0;
            st7305_color_t color = on ? fg : bg;
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
bool ui_text16_ready(void) { return font_zh16_ready(); }   /* ★ 16px 字库是否可用 */

void ui_text16(st7305_handle_t *lcd, int x, int y, const char *s, bool inverted) {
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    /* ★ 2026-11-22 linit「状态栏中文和数字没有上下居中对齐, 中文比较靠下」。
     *
     *   实测 (不是估) —— 两套字模同是 16 行, 但字形实际占的行不同:
     *     · font_zh16 汉字: 绝大多数占满 0..15 (实测 6818 字里 341 个落到第 15 行,
     *       322 个从第 0 行起笔; 中/电/期/九 都是 0..15) ⇒ 视觉中心 = 7.5
     *     · font8x16_zh ASCII: 数字/大写统一占 2..11 (下方 12..15 全空)
     *       ⇒ 视觉中心 = 6.5
     *   两者中心差 1px, 直接同 y 起画就是"数字偏上 / 汉字偏下"。
     *   把 ASCII 下移 1px 让中心对齐到 7.5 即可, 不动机器画的中文。 */
    const int aofs = 1;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) {
            /* ★ 2026-11-22 linit「其他内置应用和 Lua 应用字体不要修改」:
             *   这里**不经过 glyph** —— 直读 font_zh16 原生字模, 与字体切换
             *   功能上线前逐像素一致。字体族切换只对主菜单/游戏选择/阅读器生效。 */
            int idx = font_zh16_find_utf8((const char *)p);
            if (idx >= 0) {
                const uint8_t *b = font_zh16_get_bitmap_by_index(idx);
                for (int r = 0; r < ZH16_FONT_H; r++) for (int c = 0; c < ZH16_FONT_W; c++)
                    if (b[r * 2 + (c >> 3)] & (0x80u >> (c & 7))) st7305_draw_pixel(lcd, x + c, y + r, fg);
            }
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

/* ★ 2026-11-22 linit: 「状态栏」「MP3」这两处**不跟随**字体设置。
 *   ui_text16 本身已固定 16px 宋体原生 (不走 glyph), 所以这里只是**语义标记**:
 *   读代码的人一眼能看出这两处是刻意锁死的, 不是漏改。 */
void ui_text16_song(st7305_handle_t *lcd, int x, int y, const char *s, bool inverted) {
    ui_text16(lcd, x, y, s, inverted);
}
void ui_text16_song_c(st7305_handle_t *lcd, int cx, int y, const char *s, bool inverted) {
    ui_text16_song(lcd, cx - ui_text16w(s) / 2, y, s, inverted);
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
/* 供外部 (os_dialog 等手动居中的模块) 使用: 与 draw_text 的步进同一基准 */
int ui_menu_font_size(void) { return ui_menu_sz(); }
/* 中文: 按当前字体族合成到 sz×sz (源可能是 16px 宋 或 24px 黑, 由 glyph 决定缩放方向) */
static void ui_draw_zh_sz(st7305_handle_t *l, int x, int y, const char *str, bool inv, int sz) {
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    if (sz > 24) sz = 24;
    int stride = (sz + 7) >> 3;
    uint8_t g[24 * 3];
    bool ok = glyph_render(str, sz, g);
    for (int dy = 0; dy < sz; dy++) for (int dx = 0; dx < sz; dx++) {
        bool on = ok && (g[dy * stride + (dx >> 3)] & (uint8_t)(0x80u >> (dx & 7)));
        st7305_draw_pixel(l, x + dx, y + dy, on ? fg : bg);
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
    } else {                                     /* 兜底: FONT8X12 (实为 8×16, 完整 16 行) → sz */
        int aw = sz * 16 / 24;
        int aidx = (c >= 0x20 && c <= 0x7e) ? (c - 0x20) : ('?' - 0x20);
        fill_rect(l, x, y, x + aw - 1, y + sz - 1, bg);
        ui_blit_ascii_band(l, x, y, FONT8X12[aidx], aw, sz, fg);
    }
}

void draw_text(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted) {
    int cursor_x = x;
    int sz = ui_menu_sz();                        /* 菜单UI按设置缩放; 引擎内部不走此函数不受影响 */
    /* ★ 2026-11-22: 步进必须跟着字号走。原来无论 sz 是 16 还是 24, 中文都前进 24、
     *   英文都前进 16 —— 16px 档实际只画 16px 宽却占 24px 位, 字间出现 8px 空隙,
     *   居中也会偏右 (居中按 text_width*sz/24 算宽度, 两者对不上)。
     *   sz==24 时下面两个值就正好是 24 / 16, 与旧行为逐像素一致。 */
    int cw_zh = sz;
    int cw_as = sz * 16 / 24;
    while (*str) {
        uint8_t c = (uint8_t)*str;
        if (c < 0x80) {
            if (sz != 24) ui_draw_ascii_sz(lcd, cursor_x, y, (char)c, inverted, sz);
            else          draw_ascii(lcd, cursor_x, y, c, inverted);
            cursor_x += cw_as;
            str++;
        } else if ((c & 0xE0) == 0xC0) {
            cursor_x += cw_as;
            str += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (sz != 24) ui_draw_zh_sz(lcd, cursor_x, y, str, inverted, sz);
            else          draw_zh(lcd, cursor_x, y, str, inverted, 1);
            cursor_x += cw_zh;
            str += 3;
        } else if ((c & 0xF8) == 0xF0) {
            cursor_x += cw_zh;
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
            /* ★ 2026-10-30 修复「24px 英文之间夹着很多横线」：
             *   原来高度用的是 ascii_w（=16），只清了 y..y+15；而下面的字形循环
             *   是 12 行 × 2 倍 = 24 行，画到 y..y+23 —— 底下 8 行没被清背景，
             *   上一帧的内容原样留在那里，每个字符下面都露一条，连起来就是横线。
             *   清屏高度必须跟字形实际占用的行数一致，即 zh_w（=24×scale）。 */
            fill_rect(lcd, cursor_x, y, cursor_x + ascii_w - 1, y + zh_w - 1, bg);
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
                /* 完整 16 行 + 排版带 (原 12 行 ⇒ g/j/p/q/y 尾巴丢失, 被读成别的字母) */
                ui_blit_ascii_band(lcd, cursor_x, y, FONT8X12[idx], ascii_w, zh_w, fg);
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
/* 文本超长裁剪: 按**宽度**裁剪, 且不切坏多字节字符中间。
 * ★★★ 2026-10-24 修: 原来还有 `cjk >= 10` / `ascii >= 20` 的**硬性字数上限** ——
 *   于是"是否连接网络更新应用？"(10 汉字 + 全角问号 = 11 字) 的第 11 个字被砍,
 *   弹窗上就是"最后少一个字"(用户报的"确定的确"那个现象)。
 *   宽度 max_w 已经能保证不溢出, 字数上限纯属多余, 已删除。
 * 返回静态缓冲, 仅供立即绘制。 */
const char *text_clip(const char *str, int max_w) {
    static char s_clip[160];
    char *o = s_clip;
    const char *p = str;
    int w = 0;
    if (max_w <= 0) max_w = 320;
    while (*p && o - s_clip < (int)sizeof(s_clip) - 5) {
        unsigned char c = (unsigned char)*p;
        int len, cw;
        if (c < 0x80)                { len = 1; cw = 16; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cw = 16; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cw = 24; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cw = 24; }
        else { p++; continue; }
        if (strlen(p) < (size_t)len) break;      /* 尾部被截断的残缺序列, 丢弃 */
        if (w + cw > max_w) break;               /* 唯一的裁剪条件: 宽度 */
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
void draw_text_centered_in(st7305_handle_t *lcd, int x0, int x1, int y, const char *str, bool inverted) {
    int sz = ui_menu_sz();
    int w = text_width(str) * sz / 24;          /* 菜单缩放后的实际宽度 */
    int x = x0 + ((x1 - x0) - w) / 2;
    if (x < x0) x = x0;
    draw_text(lcd, x, y, str, inverted);
}
void draw_text_centered(st7305_handle_t *lcd, int y, const char *str, bool inverted) {
    /* ★ 2026-11-22: 同样要按菜单字号缩放, 否则 16px 档整屏居中会偏右。 */
    int sz = ui_menu_sz();
    int w = text_width(str) * sz / 24;
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
    /* ★ 2026-11-22: 折行宽度也必须按当前字号缩放, 否则 16px 档会把行切得极短。 */
    int wsz = ui_menu_sz();
    int cw_zh = wsz;
    int cw_as = wsz * 16 / 24;
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
                if (c < 0x80) cw = cw_as;
                else if ((c & 0xF0) == 0xE0) cw = cw_zh;
                else if ((c & 0xE0) == 0xC0) cw = cw_as;
                else if ((c & 0xF8) == 0xF0) cw = cw_zh;
                else cw = cw_as;
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
                int w = text_width(buf) * wsz / 24;
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
/* icon_idx 即 main_icons[] 下标 (语义化 ICON_* 枚举值). 历史上曾有一张 tbl[32]
 * 兼容重映射表, 导致"应用管家离屏列"与"主菜单"对同一下标画出不同图标; 现已删除,
 * 单一入口: 所有路径都直接用 main_icons[icon_idx]. */
/* ★★ 2026-10-15 "图标跟着应用走" (linit: 以后从服务器下发新应用, 不该为了图标改固件):
 *   LAPP 头里本来就自带 64×64 位图 (--@app icon=), 但老实现只会拿它去**反查**主图标表,
 *   查不到就固定回退 ICON_MINI_GAMES —— 于是"服务器上新应用用新图标"永远显示成小游戏机。
 *   现在加一层**运行时外来图标表** (PSRAM, 下标从 MAIN_ICON_COUNT 开始),
 *   查不到就把它注册进来并返回新下标, 主菜单/管家/离屏列三条绘制路径都能画。
 *   ⚠ 这些下标只在**本次开机**有效 (注册顺序来自扫描), 不要写进 NVS 长期保存。 */
#define UI_EXTRA_ICON_MAX 32
static const uint8_t *s_extra_icon[UI_EXTRA_ICON_MAX];
static int            s_extra_icon_n;

int ui_icon_register_extra(const uint8_t *bmp512) {
    if (!bmp512) return -1;
    for (int i = 0; i < s_extra_icon_n; i++)                 /* 去重: 同图复用同一下标 */
        if (memcmp(s_extra_icon[i], bmp512, MAIN_ICON_BYTES) == 0)
            return MAIN_ICON_COUNT + i;
    if (s_extra_icon_n >= UI_EXTRA_ICON_MAX) return -1;
    uint8_t *cp = (uint8_t *)heap_caps_malloc(MAIN_ICON_BYTES, MALLOC_CAP_SPIRAM);
    if (!cp) return -1;
    memcpy(cp, bmp512, MAIN_ICON_BYTES);
    s_extra_icon[s_extra_icon_n] = cp;
    return MAIN_ICON_COUNT + s_extra_icon_n++;
}
/* 下标 → 位图 (内置表 + 外来表) */
static const uint8_t *icon_bmp_of(int icon_idx) {
    if (icon_idx < 0) return NULL;
    if (icon_idx < MAIN_ICON_COUNT) return main_icons[icon_idx];
    int e = icon_idx - MAIN_ICON_COUNT;
    return (e < s_extra_icon_n) ? s_extra_icon[e] : NULL;
}
__attribute__((unused)) static int main_icon_index(int icon_idx) {
    return icon_bmp_of(icon_idx) ? icon_idx : 0;
}
void draw_icon_bitmap(st7305_handle_t *lcd, int cx, int cy, int size, int icon_idx) {
    draw_icon_bitmap_stretched(lcd, cx, cy, size, size, icon_idx);
}
static void ui_blit_scaled(st7305_handle_t *dev, int x0, int y0, int dst_w, int dst_h,
                           int src_w, int src_h, const uint8_t *src);   /* 前向声明 */
void draw_icon_bitmap_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int icon_idx) {
    const uint8_t *b = icon_bmp_of(icon_idx);
    if (!b) return;
    ui_blit_scaled(lcd, cx - size_w / 2, cy - size_h / 2, size_w, size_h,
                   MAIN_ICON_W, MAIN_ICON_H, b);
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
    const uint8_t *b = icon_bmp_of(icon_idx);
    if (!b) return;
    ui_blit_scaled(lcd, cx - size_w / 2, cy - size_h / 2, size_w, size_h,
                   MAIN_ICON_W, MAIN_ICON_H, b);
}
/* 取主菜单图标原始 1bpp 位图 (供离屏整列预渲染直接嵌入, 不做缩放) */
const uint8_t *ui_main_icon_bitmap(int icon_idx, int *w, int *h) {
    const uint8_t *b = icon_bmp_of(icon_idx);
    if (!b) return NULL;
    if (w) *w = MAIN_ICON_W;
    if (h) *h = MAIN_ICON_H;
    return b;
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

