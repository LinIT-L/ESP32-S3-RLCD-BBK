/**
 * page_app_manager.c — 应用管理 页面模块 (P3 迁移 + 商店预留).
 *
 * 统一应用注册表 (os_app) 的浏览/启动页:
 *   - 列出所有非 hidden 应用 (图标 + 名称), 选中启动 (os_push 对应页)
 *   - 底部固定「应用商店」入口占位 (未来 WASM 商店: catalog/下载/安装)
 *   - 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "ui_common.h"
#include "esp_log.h"
#include <stdio.h>

#define TAG "APPM"

#define ROW_H       38
#define LIST_Y      34
#define LIST_BOTTOM (UI_SCREEN_H - 4)
#define ICON_SZ     32

/* 私有 state */
typedef struct {
    int selected;   /* 选中项索引 (0..item_count-1) */
    int scroll;
} am_state_t;
static am_state_t s_am;

/* 商店入口行 (恒在列表末尾) */
#define ITEM_STORE  1

/* 可显示项数 = 非 hidden 应用数 + 商店行 */
static int am_item_count(void) {
    int n = 0;
    for (int i = 0; i < os_app_count(); i++)
        if (!os_app_get(i)->hidden) n++;
    return n + ITEM_STORE;
}

/* 第 idx 项: 0..n-1 = 应用, 最后 = 商店. 返回应用指针或 NULL(商店) */
static const os_app_t *am_app_of(int idx, int *store_out) {
    int n = 0;
    for (int i = 0; i < os_app_count(); i++) {
        const os_app_t *a = os_app_get(i);
        if (a->hidden) continue;
        if (n == idx) { *store_out = 0; return a; }
        n++;
    }
    *store_out = 1;   /* 商店行 */
    return NULL;
}

static void p_am_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    int total = am_item_count();
    int max_vis = (LIST_BOTTOM - LIST_Y) / ROW_H;
    if (max_vis < 1) max_vis = 1;
    if (s_am.selected < s_am.scroll) s_am.scroll = s_am.selected;
    if (s_am.selected >= s_am.scroll + max_vis) s_am.scroll = s_am.selected - max_vis + 1;
    if (s_am.scroll < 0) s_am.scroll = 0;
    if (s_am.scroll + max_vis > total) s_am.scroll = total - max_vis;
    if (s_am.scroll < 0) s_am.scroll = 0;

    int y = LIST_Y;
    for (int i = 0; i < max_vis && (s_am.scroll + i) < total; i++) {
        int idx = s_am.scroll + i;
        bool sel = (idx == s_am.selected);
        int store = 0;
        const os_app_t *a = am_app_of(idx, &store);

        if (sel) fill_rect(lcd, 2, y, UI_SCREEN_W - 4, y + ROW_H - 3, ST7305_COLOR_BLACK);

        if (store) {
            /* 商店入口行: 下载图标 + 文案 */
            int ix = 12 + ICON_SZ / 2;
            draw_main_icon_stretched(lcd, ix, y + ROW_H / 2, 28, 28, 0);
            draw_text(lcd, 12 + ICON_SZ + 10, y + (ROW_H - 16) / 2,
                      "\xe5\xba\x94\xe7\x94\xa8\xe5\x95\x86\xe5\xba\x97" /* 应用商店 */
                      " (\xe5\x8d\xb3\xe5\xb0\x86\xe6\x8e\xa8\xe5\x87\xba)", sel); /* (即将推出) */
        } else if (a) {
            int ix = 12 + ICON_SZ / 2;
            draw_main_icon_stretched(lcd, ix, y + ROW_H / 2, ICON_SZ, ICON_SZ, a->icon_idx);
            draw_text(lcd, 12 + ICON_SZ + 10, y + (ROW_H - 16) / 2, a->label, sel);
        }
        y += ROW_H;
    }
}

static void p_am_launch(ui_ctx_t *ctx, int idx) {
    int store = 0;
    const os_app_t *a = am_app_of(idx, &store);
    if (store) {
        /* 商店占位: 提示 (WASM 商店接入后在此打开商店页) */
        os_dialog_toast(ctx, "\xe5\x95\x86\xe5\xba\x97\xe5\xbc\x80\xe5\x8f\x91\xe4\xb8\xad"); /* 商店开发中 */
        return;
    }
    if (a) os_push(ctx, a->page);
}

static void p_am_action(ui_ctx_t *ctx, os_action_t act) {
    int total = am_item_count();
    switch (act) {
    case OS_ACTION_UP:
        if (s_am.selected > 0) s_am.selected--;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_DOWN:
        if (s_am.selected < total - 1) s_am.selected++;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM:
        p_am_launch(ctx, s_am.selected);
        break;
    case OS_ACTION_BACK:
        os_pop(ctx);
        break;
    default:
        break;
    }
}

static bool p_am_touch(ui_ctx_t *ctx, int x, int y) {
    (void)x;
    if (y < LIST_Y || y >= LIST_BOTTOM) return false;
    int idx = s_am.scroll + (y - LIST_Y) / ROW_H;
    if (idx >= am_item_count()) return false;
    s_am.selected = idx;
    p_am_launch(ctx, idx);
    return true;
}

static const os_module_t s_mod_app_manager = {
    .name       = "app_manager",
    .page_id    = OS_PAGE_APP_MANAGER,
    .render     = p_am_render,
    .action     = p_am_action,
    .touch      = p_am_touch,
    .fullscreen = false,
};

void os_page_app_manager_register(void) { os_register(&s_mod_app_manager); }
