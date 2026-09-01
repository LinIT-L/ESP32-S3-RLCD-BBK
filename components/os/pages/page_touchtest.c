/**
 * page_touchtest.c — 多点触控测试工具页 (临时调测).
 *
 * 全屏显示 GT911 两个触点的实时位置 (实心圆 + 坐标), BACK 退出.
 * 直接读 touch_panel (避免 main/input.h 与 components/input/input.h 重名遮蔽).
 */
#include "os.h"
#include "ui_common.h"
#include "touch_panel.h"
#include <stdio.h>

#define TT_R 18   /* 触点提示圆半径 */

/* 画一个触点提示圆 + 坐标 */
static void tt_draw_point(st7305_handle_t *lcd, int x, int y, bool pressed,
                          int idx) {
    if (!pressed) return;
    for (int dy = -TT_R; dy <= TT_R; dy++)
        for (int dx = -TT_R; dx <= TT_R; dx++)
            if (dx * dx + dy * dy <= TT_R * TT_R)
                st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_BLACK);
    char buf[24];
    snprintf(buf, sizeof(buf), "P%d %d,%d", idx, x, y);
    draw_text(lcd, (idx == 0) ? 10 : 230, (idx == 0) ? 4 : 26, buf, false);
}

static void p_touchtest_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    draw_text_centered(lcd, 2, "\xe8\xa7\xa6\xe5\xb1\x8f\xe6\xb5\x8b\xe8\xaf\x95", false); /* 触屏测试 */
    draw_text(lcd, 10, 120, "\xe5\x8f\x8c\xe6\x8c\x87\xe5\x8f\xaf\xe5\x90\x8c\xe6\x97\xb6\xe6\xb5\x8b\xe8\xaf\x95", false); /* 双指可同时测试 */
    tp_point_t pts[2] = {{0, 0, false}, {0, 0, false}};
    int cnt = 0;
    touch_panel_read_points(pts, &cnt);
    tt_draw_point(lcd, pts[0].x, pts[0].y, (cnt > 0 && pts[0].pressed), 1);
    tt_draw_point(lcd, pts[1].x, pts[1].y, (cnt > 1 && pts[1].pressed), 2);
}

static void p_touchtest_action(ui_ctx_t *ctx, os_action_t a) {
    if (a == OS_ACTION_BACK || a == OS_ACTION_HOME) os_pop(ctx);
}

static void p_touchtest_poll(ui_ctx_t *ctx) {
    ctx->needs_redraw = true;   /* 测试页实时刷新 */
}

static const os_module_t s_mod_touchtest = {
    .name       = "touchtest",
    .page_id    = OS_PAGE_TOUCHTEST,
    .render     = p_touchtest_render,
    .action     = p_touchtest_action,
    .poll       = p_touchtest_poll,
    .fullscreen = false,
};

void os_page_touchtest_register(void) { os_register(&s_mod_touchtest); }