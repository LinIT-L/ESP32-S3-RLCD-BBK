/**
 * page_wb.c — 白板 页面模块.
 *
 * 占位骨架: 仅保证模块契约完整 (注册/渲染/返回), 具体逻辑在后续阶段迁移.
 * 私有 state 一律 static 留在本文件, 不放进 ui_ctx.
 */
#include "os.h"
#include "ui_common.h"
#include <stdio.h>

/* ---- 占位渲染 (待迁移) ---- */
static void p_wb_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    draw_text(lcd, 140, 140, "wb (待迁移)", false);
}

static void p_wb_action(ui_ctx_t *ctx, os_action_t a) {
    (void)ctx;
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

static const os_module_t s_mod_wb = {
    .name       = "wb",
    .page_id    = OS_PAGE_WHITEBOARD,
    .render     = p_wb_render,
    .action     = p_wb_action,
    .fullscreen = true,
};

void os_page_wb_register(void) { os_register(&s_mod_wb); }
