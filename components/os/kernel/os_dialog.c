/**
 * os_dialog.c — 通用弹窗 (modal 覆盖页) 内核模块.
 *
 * 供所有页面复用统一弹窗: 列表选择 / 确认框 / 提示 toast.
 * 以 OS_PAGE_DIALOG 压栈 (modal), 不退出底层页面; 关闭后自动恢复底层.
 * 私有 state 全部 static 留本文件.
 *
 * 使用示例:
 *   os_dialog_list(ctx, "键鼠布局", items, n, sel, on_sel, NULL);
 *   os_dialog_confirm(ctx, "确定退出?", on_res, NULL);
 *   os_dialog_toast(ctx, "已收藏");
 */
#include "os.h"
#include "ui_common.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#define TAG "DLG"

/* ---- 弹窗类型 ---- */
typedef enum { DLG_LIST, DLG_CONFIRM, DLG_TOAST } dlg_mode_t;

#define DLG_MAX_ITEMS 8
#define DLG_ITEM_LEN  30

/* 弹窗布局 */
#define DLG_X0     40
#define DLG_X1     (UI_SCREEN_W - 40)
#define DLG_ROW    30
#define DLG_TITLE_H 24

/* 私有状态 (单实例; 一次仅一个弹窗) */
typedef struct {
    dlg_mode_t mode;
    char title[24];
    char items[DLG_MAX_ITEMS][DLG_ITEM_LEN];
    int  count;
    int  sel;
    os_dialog_cb_t cb;
    void *ud;
    /* toast */
    char     toast_msg[24];
    uint32_t toast_ms;
} dlg_state_t;
static dlg_state_t s_dlg;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ============ 渲染 (不清屏, 覆盖在底层之上) ============ */
static void dlg_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;

    if (s_dlg.mode == DLG_TOAST) {
        /* 居中提示框, 1s 自动关闭 */
        int len = (int)strlen(s_dlg.toast_msg);
        int w = 16 + len * 16 + 16;
        if (w > UI_SCREEN_W - 40) w = UI_SCREEN_W - 40;
        int x0 = (UI_SCREEN_W - w) / 2;
        int y0 = (UI_SCREEN_H - 40) / 2;
        draw_dialog_frame(lcd, x0, y0, w, 40, 2);
        draw_text_centered(lcd, y0 + 10, s_dlg.toast_msg, false);
        return;
    }

    int h = DLG_TITLE_H + 4 + s_dlg.count * DLG_ROW + 8;
    int y0 = (UI_SCREEN_H - h) / 2;
    draw_dialog_frame(lcd, DLG_X0, y0, DLG_X1 - DLG_X0 + 1, h, 3);
    draw_text_centered(lcd, y0 + 2, s_dlg.title, false);
    for (int i = 0; i < s_dlg.count; i++) {
        int y = y0 + DLG_TITLE_H + 4 + i * DLG_ROW;
        bool sel = (i == s_dlg.sel);
        if (sel) fill_rect(lcd, DLG_X0 + 3, y, DLG_X1 - 3, y + DLG_ROW - 2, ST7305_COLOR_BLACK);
        draw_text(lcd, DLG_X0 + 14, y + (DLG_ROW - 16) / 2, s_dlg.items[i], sel);
    }
}

/* ============ 动作 ============ */
static void dlg_finish(ui_ctx_t *ctx, int result) {
    os_dialog_cb_t cb = s_dlg.cb;
    void *ud = s_dlg.ud;
    /* 先弹栈再回调: 回调内可再开新弹窗/导航 */
    os_pop(ctx);
    if (cb) cb(ctx, result, ud);
}

static void dlg_action(ui_ctx_t *ctx, os_action_t a) {
    if (s_dlg.mode == DLG_TOAST) {
        /* 任意键关闭 */
        os_pop(ctx);
        return;
    }
    switch (a) {
    case OS_ACTION_UP:
        if (s_dlg.sel > 0) s_dlg.sel--;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_DOWN:
        if (s_dlg.sel < s_dlg.count - 1) s_dlg.sel++;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM:
        dlg_finish(ctx, s_dlg.sel);
        break;
    case OS_ACTION_BACK:
        dlg_finish(ctx, -1);
        break;
    default:
        break;
    }
}

static bool dlg_touch(ui_ctx_t *ctx, int x, int y) {
    if (s_dlg.mode == DLG_TOAST) {
        os_pop(ctx);
        return true;
    }
    if (x < DLG_X0 || x > DLG_X1) return false;
    int h = DLG_TITLE_H + 4 + s_dlg.count * DLG_ROW + 8;
    int y0 = (UI_SCREEN_H - h) / 2;
    int rel = y - (y0 + DLG_TITLE_H + 4);
    if (rel < 0) return false;
    int idx = rel / DLG_ROW;
    if (idx >= 0 && idx < s_dlg.count) {
        s_dlg.sel = idx;
        dlg_finish(ctx, idx);
        return true;
    }
    return false;
}

static void dlg_poll(ui_ctx_t *ctx) {
    if (s_dlg.mode == DLG_TOAST && now_ms() - s_dlg.toast_ms > 1000) {
        os_pop(ctx);
    }
}

static const os_module_t s_mod_dialog = {
    .name       = "dialog",
    .page_id    = OS_PAGE_DIALOG,
    .render     = dlg_render,
    .action     = dlg_action,
    .touch      = dlg_touch,
    .poll       = dlg_poll,
    .modal      = true,
};

void os_page_dialog_register(void) { os_register(&s_mod_dialog); }

/* ============ 打开弹窗 ============ */
void os_dialog_list(ui_ctx_t *ctx, const char *title,
                    const char *const *items, int count, int sel,
                    os_dialog_cb_t cb, void *ud) {
    s_dlg.mode = DLG_LIST;
    snprintf(s_dlg.title, sizeof(s_dlg.title), "%s", title ? title : "");
    s_dlg.count = count < DLG_MAX_ITEMS ? count : DLG_MAX_ITEMS;
    if (s_dlg.count < 0) s_dlg.count = 0;
    for (int i = 0; i < s_dlg.count; i++)
        snprintf(s_dlg.items[i], DLG_ITEM_LEN, "%s", items[i]);
    s_dlg.sel = sel < 0 ? 0 : sel;
    if (s_dlg.sel >= s_dlg.count) s_dlg.sel = 0;
    s_dlg.cb = cb;
    s_dlg.ud = ud;
    os_push(ctx, OS_PAGE_DIALOG);
}

void os_dialog_confirm(ui_ctx_t *ctx, const char *msg, os_dialog_cb_t cb, void *ud) {
    const char *items[2] = { "\xe7\xa1\xae\xe5\xae\x9a", "\xe5\x8f\x96\xe6\xb6\x88" }; /* 确定/取消 */
    s_dlg.mode = DLG_CONFIRM;
    snprintf(s_dlg.title, sizeof(s_dlg.title), "%s", msg ? msg : "");
    s_dlg.count = 2;
    snprintf(s_dlg.items[0], DLG_ITEM_LEN, "%s", items[0]);
    snprintf(s_dlg.items[1], DLG_ITEM_LEN, "%s", items[1]);
    s_dlg.sel = 0;
    s_dlg.cb = cb;
    s_dlg.ud = ud;
    os_push(ctx, OS_PAGE_DIALOG);
}

void os_dialog_toast(ui_ctx_t *ctx, const char *msg) {
    s_dlg.mode = DLG_TOAST;
    snprintf(s_dlg.toast_msg, sizeof(s_dlg.toast_msg), "%s", msg ? msg : "");
    s_dlg.toast_ms = now_ms();
    s_dlg.cb = NULL;
    s_dlg.ud = NULL;
    os_push(ctx, OS_PAGE_DIALOG);
}
