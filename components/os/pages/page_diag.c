/**
 * page_diag.c — 诊断 页面模块.
 *
 * 占位骨架: 仅保证模块契约完整 (注册/渲染/返回), 具体逻辑在后续阶段迁移.
 * 私有 state 一律 static 留在本文件, 不放进 ui_ctx.
 */
#include "os.h"
#include "ui_common.h"
#include <stdio.h>

/* ---- 占位渲染 (待迁移) ---- */
static void p_diag_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    draw_text(lcd, 140, 140, "diag (待迁移)", false);
}

static void p_diag_action(ui_ctx_t *ctx, os_action_t a) {
    (void)ctx;
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

static const os_module_t s_mod_diag = {
    .name       = "diag",
    .page_id    = OS_PAGE_DIAGNOSIS,
    .render     = p_diag_render,
    .action     = p_diag_action,
    .fullscreen = true,
};

void os_page_diag_register(void) { os_register(&s_mod_diag); }
