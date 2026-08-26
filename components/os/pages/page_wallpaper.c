/**
 * page_wallpaper.c — 壁纸 页面模块.
 *
 * 单列选项列表: 内置壁纸程序 / 外置图片 / 游戏壁纸 / 壁纸测试 / 返回.
 * 选中程序保存 (os_wallpaper_get_program 供桌面/屏保读取).
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
#define ITEM_MAX    16

/* 私有 state */
typedef struct {
    int   selected;        /* 选中项索引 */
    int   scroll;
    bool  preview;         /* 壁纸测试全屏预览 */
    int   preview_prog;    /* 预览的程序 id */
    bool  loaded;
} wp_state_t;
static wp_state_t s_wp;

/* 当前生效壁纸程序 (0=星空默认). 桌面渲染读此值. */
static int s_wallpaper_program = 0;

int os_wallpaper_get_program(void) { return s_wallpaper_program; }

/* 选项列表: 内置程序 (0=星空 + 1..8) + 外置图片 + 游戏壁纸 + 测试 + 返回 */
static int wp_item_count(void) {
    return 2 + WP_PROG_COUNT;   /* 外置图片 + 游戏壁纸 + 8 程序 + 测试 + 返回 = 12 */
}
static const char *wp_item_name(int idx) {
    if (idx == 0)               return "\xe6\x98\x9f\xe7\xa9\xba";            /* 星空 */
    if (idx <= WP_PROG_COUNT)   return wp_prog_name(idx);                     /* 内置程序 1..8 */
    if (idx == 1 + WP_PROG_COUNT) return "\xe5\xa4\x96\xe7\xbd\xae\xe5\x9b\xbe\xe7\x89\x87";   /* 外置图片 */
    if (idx == 2 + WP_PROG_COUNT) return "\xe6\xb8\xb8\xe6\x88\x8f\xe5\xa3\x81\xe7\xba\xb8";   /* 游戏壁纸 */
    if (idx == 3 + WP_PROG_COUNT) return "\xe5\xa3\x81\xe7\xba\xb8\xe6\xb5\x8b\xe8\xaf\x95";   /* 壁纸测试 */
    return "\xe8\xbf\x94\xe5\x9b\x9e";                                        /* 返回 */
}

static void p_wp_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    /* 壁纸测试预览: 全屏渲染程序 */
    if (s_wp.preview) {
        wp_program_render(lcd, s_wp.preview_prog, (uint32_t)(esp_timer_get_time() / 1000));
        ctx->fullscreen = true;
        return;
    }
    ctx->fullscreen = false;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    int total = wp_item_count();
    int max_vis = (LIST_BOTTOM - LIST_Y) / ROW_H;
    if (max_vis < 1) max_vis = 1;
    if (s_wp.selected < s_wp.scroll) s_wp.scroll = s_wp.selected;
    if (s_wp.selected >= s_wp.scroll + max_vis) s_wp.scroll = s_wp.selected - max_vis + 1;
    int y = LIST_Y;
    for (int i = 0; i < max_vis && (s_wp.scroll + i) < total; i++) {
        int idx = s_wp.scroll + i;
        /* 当前生效程序标 * 号 */
        char buf[72];
        snprintf(buf, sizeof(buf), "%s%s", (idx == 0 && s_wallpaper_program == 0) ||
                       (idx > 0 && idx <= WP_PROG_COUNT && s_wallpaper_program == idx) ? "* " : "",
                 wp_item_name(idx));
        if (idx == s_wp.selected)
            draw_hline(lcd, 2, UI_SCREEN_W - 4, y + ROW_H - 3, ST7305_COLOR_BLACK);
        draw_text(lcd, 12, y + (ROW_H - 16) / 2, buf, false);
        y += ROW_H;
    }
}

static void p_wp_action(ui_ctx_t *ctx, os_action_t a) {
    if (s_wp.preview) {
        /* 预览: 任意键退出 */
        s_wp.preview = false;
        ctx->fullscreen = false;
        ctx->needs_redraw = true;
        return;
    }
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
        int idx = s_wp.selected;
        if (idx == 0) {
            s_wallpaper_program = WP_PROG_STARS;
            ctx->needs_redraw = true;
        } else if (idx >= 1 && idx <= WP_PROG_COUNT) {
            s_wallpaper_program = idx;
            ctx->needs_redraw = true;
        } else if (idx == 1 + WP_PROG_COUNT) {
            /* 外置图片: 暂未接入 BMP 播放, 提示 */
            ESP_LOGI(TAG, "外置图片 (暂未接入)");
        } else if (idx == 2 + WP_PROG_COUNT) {
            /* 游戏壁纸: 暂未接入 */
            ESP_LOGI(TAG, "游戏壁纸 (暂未接入)");
        } else if (idx == 3 + WP_PROG_COUNT) {
            /* 壁纸测试: 全屏预览当前/选中程序 */
            s_wp.preview_prog = s_wallpaper_program;
            s_wp.preview = true;
            ctx->fullscreen = true;
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
    if (s_wp.preview) {
        s_wp.preview = false;
        ctx->fullscreen = false;
        ctx->needs_redraw = true;
        return true;
    }
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
