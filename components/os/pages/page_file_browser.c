/**
 * page_file_browser.c — 文件浏览 页面模块 (存储→浏览文件).
 *
 * 复用 os_pane 双栏模板: 左栏 = /sdcard 顶层目录, 右栏 = 目录内文件/根目录文件.
 * 点选文件显示其完整路径 (浏览用途); 返回退出.
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "os_pane.h"
#include "ui_common.h"
#include "sd_scan.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <string.h>
#include <stdio.h>

#define TAG "PFB"

static bool is_any(const char *name) { (void)name; return true; }

static void on_file_select(ui_ctx_t *ctx, os_pane_t *p,
                           const char *path, const char *name) {
    (void)p; (void)name;
    os_dialog_toast(ctx, path);   /* 显示文件完整路径 */
}

static EXT_RAM_BSS_ATTR os_pane_t s_fb;

static void p_fb_enter(ui_ctx_t *ctx) {
    if (!s_fb.loaded) {
        s_fb.root = "/sdcard";
        s_fb.is_item = is_any;
        s_fb.on_select = on_file_select;
        s_fb.all_label = NULL;
        s_fb.list_y = 30;
        os_pane_reset(&s_fb);
    }
    os_pane_build_right(&s_fb);
}

static void p_fb_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    os_pane_render(ctx, &s_fb);
}

static void p_fb_action(ui_ctx_t *ctx, os_action_t a) {
    if (os_pane_action(ctx, &s_fb, a)) return;
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

static bool p_fb_touch(ui_ctx_t *ctx, int x, int y) {
    return os_pane_touch(ctx, &s_fb, x, y);
}

static void p_fb_poll(ui_ctx_t *ctx) {
    os_pane_poll(ctx, &s_fb);   /* 右栏触摸拖动滚动 */
}

static const os_module_t s_mod_file_browser = {
    .name       = "file_browser",
    .page_id    = OS_PAGE_FILE_BROWSER,
    .on_enter   = p_fb_enter,
    .render     = p_fb_render,
    .action     = p_fb_action,
    .touch      = p_fb_touch,
    .poll       = p_fb_poll,
    .fullscreen = false,
};

void os_page_file_browser_register(void) { os_register(&s_mod_file_browser); }
