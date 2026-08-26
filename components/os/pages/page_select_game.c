/**
 * page_select_game.c — 游戏 页面模块 (双栏: 左目录 / 右游戏).
 *
 * 使用统一双栏模板 os_pane (components/os/kernel/os_pane.c):
 *   目录 /sdcard/gam 下的子文件夹 + 根目录 .gam 游戏.
 *   交互: 左右切栏, 上下选行, 确认键启动游戏 (gam4980 引擎, 阻塞运行).
 * 私有 state 一律 static 留在本文件, 不放进 ui_ctx.
 */
#include "os.h"
#include "os_pane.h"
#include "gam4980_emu.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define TAG "PGAME"

#define GAM_ROOT "/sdcard/gam"

static bool is_gam(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".gam") == 0);
}

/* 右栏确认: 启动游戏 (加载 → 恢复存档 → 阻塞运行 → 保存存档) */
static void on_game_select(ui_ctx_t *ctx, os_pane_t *p,
                           const char *path, const char *name) {
    (void)p; (void)name;
    ESP_LOGI(TAG, "启动: %s", path);
    if (gam4980_emu_load(path) == 0) {
        gam4980_emu_load_state(path);
        gam4980_emu_run();
        gam4980_emu_save_state(path);
    }
    ctx->needs_redraw = true;   /* 游戏退出后重绘列表 */
}

static os_pane_t s_gp;   /* 双栏模板状态 */

static void p_select_game_enter(ui_ctx_t *ctx) {
    (void)ctx;
    if (!s_gp.loaded) {
        s_gp.root = GAM_ROOT;
        s_gp.is_item = is_gam;
        s_gp.on_select = on_game_select;
        s_gp.all_label = NULL;
        os_pane_reset(&s_gp);
    }
    os_pane_build_right(&s_gp);
}

static void p_select_game_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    os_pane_render(ctx, &s_gp);
}

static void p_select_game_action(ui_ctx_t *ctx, os_action_t a) {
    if (os_pane_action(ctx, &s_gp, a)) return;
    if (a == OS_ACTION_BACK) os_pop(ctx);
}

static const os_module_t s_mod_select_game = {
    .name       = "select_game",
    .page_id    = OS_PAGE_SELECT_GAME,
    .on_enter   = p_select_game_enter,
    .render     = p_select_game_render,
    .action     = p_select_game_action,
    .fullscreen = false,
};

void os_page_select_game_register(void) { os_register(&s_mod_select_game); }
