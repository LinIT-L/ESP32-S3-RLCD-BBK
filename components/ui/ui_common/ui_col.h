/**
 * ui_col.h — 「整列离屏预渲染 + 滚动只抄窗」通用缓冲（与应用商店同款机制）
 *
 * 为什么需要它：
 *   1bpp 屏上逐帧重绘列表（清屏 + 每行重绘字形）每帧要写 10 万+ 像素，直接掉到 20fps。
 *   商店的做法是：**内容变化时把整列画到 PSRAM 缓冲一次**，滚动时只
 *   `st7305_blit_1bit` 把可视窗抄到屏上（纯内存拷贝）⇒ 拖动跟手、帧率高。
 *
 * 标准用法:
 *   static ui_col_t col;
 *   // 进页面或内容变化:
 *   ui_col_init(&col, LIST_W, (n + 1) * ROW_H);
 *   ui_col_clear(&col);
 *   for (行...) { ui_col_fill(...); ui_col_text16(...); ui_col_icon(...); }
 *   // 每帧(或滚动时):
 *   ui_col_present(&col, lcd, LIST_X, HDR_H, S_H - HDR_H, scroll_px);
 *
 * 位约定: 缓冲位=1 表示**黑**，与 st7305_blit_1bit 一致。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;   /* PSRAM */
    int      w;
    int      h;
    int      bpr;   /* bytes per row */
} ui_col_t;

bool ui_col_init(ui_col_t *c, int w, int h);      /* 尺寸变化才重新分配 */
void ui_col_free(ui_col_t *c);
void ui_col_clear(ui_col_t *c);                   /* 全白 */
void ui_col_px(ui_col_t *c, int x, int y, bool black);
void ui_col_fill(ui_col_t *c, int x0, int y0, int x1, int y1, bool black);
void ui_col_hline(ui_col_t *c, int x0, int x1, int y, bool black);
void ui_col_text16(ui_col_t *c, int x, int y, const char *s, bool inverted);
void ui_col_text24(ui_col_t *c, int x, int y, const char *s, bool inverted);   /* 24px (设置/分类列表) */
void ui_col_icon(ui_col_t *c, int cx, int cy, int size, int icon_idx);            /* 64px 主图标表 */
void ui_col_icon32(ui_col_t *c, int cx, int cy, int size, const uint8_t *bmp32);  /* 32×32 自定义图标 */

/* 把 [src_y, src_y+win_h) 抄到屏 (x,y)。win_h 自动夹紧。 */
void ui_col_present(ui_col_t *c, st7305_handle_t *lcd, int x, int y, int win_h, int src_y);

#ifdef __cplusplus
}
#endif
