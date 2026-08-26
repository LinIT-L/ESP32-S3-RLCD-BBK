/**
 * page_book.c — 电子书 页面模块.
 *
 * 书架双栏 (左分类 / 右书籍) 使用统一模板 os_pane + 阅读器模式 (book_reader 组件, 运行时全屏).
 * 私有 state 一律 static 留在本文件, 不放进 ui_ctx.
 */
#include "os.h"
#include "os_pane.h"
#include "book_reader.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define TAG "PBOOK"

#define BOOK_ROOT "/sdcard/books"

static bool is_txt(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".txt") == 0);
}

static os_pane_t s_bp;   /* 双栏模板状态 */

/* 阅读器模式 (运行时全屏) */
static bool s_reader_open = false;

/* 阅读器默认设置 (后续接入设置页后再改) */
static void apply_reader_defaults(void) {
    book_reader_set_settings(false, 1, false, true, 0, 0, 1, 1, 1, 0);
}

/* 右栏确认: 打开书籍 */
static void on_book_select(ui_ctx_t *ctx, os_pane_t *p,
                           const char *path, const char *name) {
    (void)p; (void)name;
    apply_reader_defaults();
    if (book_reader_open(path)) {
        s_reader_open = true;
        ctx->fullscreen = true;
        ESP_LOGI(TAG, "打开: %s", path);
    }
}

static void p_book_enter(ui_ctx_t *ctx) {
    (void)ctx;
    if (!s_bp.loaded) {
        s_bp.root = BOOK_ROOT;
        s_bp.is_item = is_txt;
        s_bp.on_select = on_book_select;
        s_bp.all_label = NULL;
        os_pane_reset(&s_bp);
    }
    os_pane_build_right(&s_bp);
}

static void p_book_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    if (s_reader_open && book_reader_is_open()) {
        book_reader_render(lcd);
        ctx->fullscreen = true;
        return;
    }
    ctx->fullscreen = false;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    os_pane_render(ctx, &s_bp);
}

static void p_book_action(ui_ctx_t *ctx, os_action_t a) {
    if (s_reader_open) {
        if (a == OS_ACTION_BACK) {
            book_reader_close();
            s_reader_open = false;
            ctx->fullscreen = false;
            ctx->needs_redraw = true;
            return;
        }
        if (book_reader_handle_action((int)a)) ctx->needs_redraw = true;
        return;
    }
    if (os_pane_action(ctx, &s_bp, a)) return;
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

static bool p_book_touch(ui_ctx_t *ctx, int x, int y) {
    if (s_reader_open) {
        if (book_reader_handle_touch(x, y)) ctx->needs_redraw = true;
        return true;   /* 阅读器内全部消费 */
    }
    return os_pane_touch(ctx, &s_bp, x, y);
}

static const os_module_t s_mod_book = {
    .name       = "book",
    .page_id    = OS_PAGE_BOOK,
    .on_enter   = p_book_enter,
    .render     = p_book_render,
    .action     = p_book_action,
    .touch      = p_book_touch,
    .fullscreen = false,
};

void os_page_book_register(void) { os_register(&s_mod_book); }
