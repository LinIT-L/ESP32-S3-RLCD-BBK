/**
 * page_icon_editor.c — 图标编辑 页面模块.
 *
 * 旧版 menu 中 MENU_PAGE_ICON_EDITOR 仅有枚举占位, 无实际实现.
 * 此处保持模块契约完整并给出开发中提示, 待后续实现"编辑内置 64x64 图标 / 自定义图标仓库(SD)".
 * 私有 state 一律 static 留在本文件.
 */
#include "os.h"
#include "ui_common.h"
#include <stdio.h>

static void p_icon_editor_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    draw_text_centered(lcd, 130, "\xe5\x9b\xbe\xe6\xa0\x87\xe7\xbc\x96\xe8\xbe\x91\xe5\x99\xa8", false); /* 图标编辑器 */
    draw_text_centered(lcd, 160, "\xe5\xbc\x80\xe5\x8f\x91\xe4\xb8\xad", false); /* 开发中 */
}

static void p_icon_editor_action(ui_ctx_t *ctx, os_action_t a) {
    (void)ctx;
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

static const os_module_t s_mod_icon_editor = {
    .name       = "icon_editor",
    .page_id    = OS_PAGE_ICON_EDITOR,
    .render     = p_icon_editor_render,
    .action     = p_icon_editor_action,
    .fullscreen = false,
};

void os_page_icon_editor_register(void) { os_register(&s_mod_icon_editor); }
