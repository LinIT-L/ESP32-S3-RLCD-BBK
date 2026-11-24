/* 主机测试用桩: ui_scroll.c 的滚动条绘制在这里只需要类型与颜色常量,
 * 像素回调直接丢弃 —— 本测试只验证行号反算 (ui_scroll_row_at)。
 * 签名照真实 components/drivers/st7305/st7305.h 抄, 避免隐式转换告警。 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ST7305_COLOR_BLACK    0
#define ST7305_COLOR_WHITE    1

typedef uint8_t st7305_color_t;
typedef struct st7305_s st7305_handle_t;

static inline void st7305_draw_pixel(st7305_handle_t *dev, int x, int y, st7305_color_t color)
{
    (void)dev; (void)x; (void)y; (void)color;
}
