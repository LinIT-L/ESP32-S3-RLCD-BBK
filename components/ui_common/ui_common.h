/**
 * ui_common.h — 通用绘制内核 (纯绘制原语 + 中英文字体渲染 + 位图图标).
 *
 * 从 menu_system.c 拆出: 这些函数不依赖 menu_state_t, 只依赖 st7305 显示驱动
 * 与中文字库(font_zh)。凡需"画块/画线/写文字/画图标"的模块都应 include 本头.
 */
#ifndef UI_COMMON_H
#define UI_COMMON_H

#include "st7305.h"
#include "font_zh.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_SCREEN_W   ST7305_WIDTH    /* 400 */
#define UI_SCREEN_H   ST7305_HEIGHT   /* 300 */

/* 当前逻辑屏幕尺寸 (默认 400×300 横屏; 书库旋转左/右时由调用方切为 300×400 竖屏).
 * os_pane/os_dialog/书库弹窗等布局读取此处, 竖屏时自动按 300 宽重排. */
int  ui_screen_w(void);
int  ui_screen_h(void);
void ui_set_portrait(bool on);   /* true → 300×400 竖屏; false → 400×300 横屏 */

/* 8x12 ASCII 字体 (缩放 2x = 16x24, 与中文字体等高).
 * 历史定义于 menu_system.c, keyboard.c / terminal.c 直接 extern 引用同一符号. */
extern const uint8_t (*FONT8X12)[16];

/* ===== mini-UI 基础控件 (类 LVGL 简化, 供全项目统一) =====
 * 统一字体: 中文 font_zh16(16×16) + 英文 8×16; 文案风格一致. */
int  ui_text16w(const char *s);                              /* 整行像素宽(中16/英8) */
void ui_text16(st7305_handle_t *lcd, int x, int y, const char *s, bool inverted); /* 左对齐 */
void ui_text16c(st7305_handle_t *lcd, int cx, int y, const char *s, bool inverted);/* 居中 */

/* ---------- 基础图形原语 ---------- */
void ui_draw_pixel(st7305_handle_t *lcd, int x, int y, st7305_color_t color);
void ui_draw_pixel_inv(st7305_handle_t *lcd, int x, int y, st7305_color_t color, bool inverted);
void fill_rect(st7305_handle_t *lcd, int x0, int y0, int x1, int y1, st7305_color_t color);
void draw_hline(st7305_handle_t *lcd, int x0, int x1, int y, st7305_color_t color);
void draw_vline(st7305_handle_t *lcd, int x, int y0, int y1, st7305_color_t color);
void draw_rect_outline(st7305_handle_t *lcd, int x0, int y0, int x1, int y1, st7305_color_t color);
void draw_rect_outline_side_round(st7305_handle_t *lcd, int x0, int y0, int x1, int y1,
                                  st7305_color_t color, int r, char side);
void draw_dialog_frame(st7305_handle_t *lcd, int x, int y, int w, int h, int border);
void draw_slot_marker(st7305_handle_t *lcd, int cx, int cy, int size);

/* ---------- 字符串/字符渲染 ---------- */
void draw_ascii_small(st7305_handle_t *lcd, int x, int y, char c, bool inverted);
void draw_ascii_medium(st7305_handle_t *lcd, int x, int y, char c, bool inverted);
void draw_ascii_medium_bmp(st7305_handle_t *lcd, int x, int y, const uint8_t *bmp, bool inverted);
void draw_ascii(st7305_handle_t *lcd, int x, int y, char c, bool inverted);
void draw_ascii_sb(st7305_handle_t *lcd, int x, int y, char c, bool inverted);
void draw_zh(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted, int scale);
void draw_zh_small(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted);
void draw_zh_sb(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted);
void draw_text(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted);
void draw_text_capped(st7305_handle_t *lcd, int x, int y, const char *str,
                      bool inverted, int max_w);
void draw_label(st7305_handle_t *lcd, int x, int y, const char *str, bool inverted);
void draw_text_centered(st7305_handle_t *lcd, int y, const char *str, bool inverted);
void draw_label_centered_at(st7305_handle_t *lcd, int cx, int y, const char *str, bool inverted);
int  text_width(const char *str);
int  text_width_bounded(const char *start, const char *end);
const char *text_clip(const char *str, int max_w);   /* 超长裁剪: 20英文/10中文, UTF-8 安全 */
int  label_width(const char *str);
int  draw_text_wrapped(st7305_handle_t *lcd, int x0, int x1, int y0,
                       const char *text, bool inverted, int align, int line_h);
void menu_show_center_msg(st7305_handle_t *lcd, const char *line1, const char *line2);

/* ---------- 位图图标 ---------- */
void draw_icon_bitmap(st7305_handle_t *lcd, int cx, int cy, int size, int icon_idx);
void draw_icon_bitmap_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int icon_idx);
void draw_main_icon_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int icon_idx);
/* 通用 1bpp 位图缩放直写 (同主图标朝向), 供嵌入自定义位图 (如诊断器件图标) */
void draw_1bpp_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h,
                         int src_w, int src_h, const uint8_t *src);
void draw_cat_icon_stretched(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int cat_idx);
void menu_draw_cat_icon(st7305_handle_t *lcd, int cx, int cy, int size_w, int size_h, int cat_idx);

#ifdef __cplusplus
}
#endif

#endif /* UI_COMMON_H */

void ui_draw_notice_popup(st7305_handle_t *lcd, const char *text);
