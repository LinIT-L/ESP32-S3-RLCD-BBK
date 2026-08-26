/**
 * page_wallpaper.c — 壁纸 页面模块.
 *
 * 单列选项列表: 仅保留「星空」(其余动态壁纸已删除), 加「返回」.
 * 选中保存 (os_wallpaper_get_program 供桌面/屏保读取).
 * 私有 state 一律 static 留在本文件, 不放进 ui_ctx.
 */
#include "os.h"
#include "ui_common.h"
#include "wallpapers.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

#define TAG "PWP"

#define ROW_H       32
#define LIST_Y      30
#define LIST_BOTTOM (UI_SCREEN_H - 4)

/* 私有 state */
typedef struct {
    int   selected;
    int   scroll;
} wp_state_t;
static wp_state_t s_wp;

/* 当前生效壁纸程序 (0=星空默认). 桌面渲染读此值. */
static int s_wallpaper_program = 0;

int os_wallpaper_get_program(void) { return s_wallpaper_program; }

/* 选项列表: 星空 + 返回 */
#define WP_ITEM_STARS  0
#define WP_ITEM_BACK   1
static int wp_item_count(void) { return 2; }
static const char *wp_item_name(int idx) {
    if (idx == WP_ITEM_STARS) return "\xe6\x98\x9f\xe7\xa9\xba"; /* 星空 */
    return "\xe8\xbf\x94\xe5\x9b\x9e";                            /* 返回 */
}

static void p_wp_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    int total = wp_item_count();
    int max_vis = (LIST_BOTTOM - LIST_Y) / ROW_H;
    if (max_vis < 1) max_vis = 1;
    if (s_wp.selected < s_wp.scroll) s_wp.scroll = s_wp.selected;
    if (s_wp.selected >= s_wp.scroll + max_vis) s_wp.scroll = s_wp.selected - max_vis + 1;
    int y = LIST_Y;
    for (int i = 0; i < max_vis && (s_wp.scroll + i) < total; i++) {
        int idx = s_wp.scroll + i;
        char buf[48];
        snprintf(buf, sizeof(buf), "%s%s",
                 (s_wallpaper_program == 0 && idx == WP_ITEM_STARS) ? "* " : "",
                 wp_item_name(idx));
        if (idx == s_wp.selected)
            draw_hline(lcd, 2, UI_SCREEN_W - 4, y + ROW_H - 3, ST7305_COLOR_BLACK);
        draw_text(lcd, 12, y + (ROW_H - 16) / 2, buf, false);
        y += ROW_H;
    }
}

static void p_wp_action(ui_ctx_t *ctx, os_action_t a) {
    int total = wp_item_count();
    switch (a) {
    case OS_ACTION_UP:
        if (s_wp.selected > 0) s_wp.selected--;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_DOWN:
        if (s_wp.selected < total - 1) s_wp.selected++;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM: {
        if (s_wp.selected == WP_ITEM_STARS) {
            s_wallpaper_program = WP_PROG_STARS;
        } else {
            os_pop(ctx);
        }
        ctx->needs_redraw = true;
        break;
    }
    case OS_ACTION_BACK:
        os_pop(ctx);
        break;
    default:
        break;
    }
}

static bool p_wp_touch(ui_ctx_t *ctx, int x, int y) {
    (void)x;
    if (y < LIST_Y || y >= LIST_BOTTOM) return false;
    int idx = s_wp.scroll + (y - LIST_Y) / ROW_H;
    if (idx >= wp_item_count()) return false;
    s_wp.selected = idx;
    p_wp_action(ctx, OS_ACTION_CONFIRM);
    return true;
}

static const os_module_t s_mod_wallpaper = {
    .name       = "wallpaper",
    .page_id    = OS_PAGE_WALLPAPER,
    .render     = p_wp_render,
    .action     = p_wp_action,
    .touch      = p_wp_touch,
    .fullscreen = false,
};

void os_page_wallpaper_register(void) { os_register(&s_mod_wallpaper); }
