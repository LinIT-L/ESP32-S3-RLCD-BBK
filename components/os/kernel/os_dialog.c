/**
 * os_dialog.c — 通用弹窗 (modal 覆盖页) 内核模块 + 弹窗栈.
 *
 * 供所有页面复用统一弹窗: 列表选择 / 确认框 / 提示 toast.
 * 以 OS_PAGE_DIALOG 压栈 (modal), 不退出底层页面; 关闭后自动恢复底层.
 * 内部维护弹窗栈 (settings/list_dialog 嵌套): 开子弹窗压栈, 返回弹栈恢复父弹窗.
 * 复杂弹窗 (时间编辑等) 通过 os_dlg_stack 的 on_render/on_key 自定义.
 * 私有 state 全部 static 留本文件.
 *
 * 使用示例:
 *   os_dialog_list(ctx, "键鼠布局", items, n, sel, on_sel, NULL);
 *   os_dialog_confirm(ctx, "确定退出?", on_res, NULL);
 *   os_dialog_toast(ctx, "已收藏");
 *   os_dialog_push(ctx, &dlg);   [子弹窗 - 栈内嵌套]
 *   os_dialog_pop(ctx);          [返回父弹窗]
 */
#include "os.h"
#include "ui_common.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#define TAG "DLG"

#define DLG_MAX_DEPTH 4
#define DLG_MAX_ITEMS 8
#define DLG_ITEM_LEN  30

/* 弹窗布局 */
#define DLG_X0     40
#define DLG_X1     (UI_SCREEN_W - 40)
#define DLG_ROW    30
#define DLG_TITLE_H 24

/* 弹窗栈 (每层一份完整状态, 弹栈时恢复父层) */
static os_dlg_stack_t s_stack[DLG_MAX_DEPTH];
static int s_top = -1;

/* toast (不占弹窗栈: 瞬时提示) */
static char     s_toast_msg[24];
static uint32_t s_toast_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static os_dlg_stack_t *top(void) { return (s_top >= 0) ? &s_stack[s_top] : NULL; }

/* 是否需要以 toast 模式渲染 (栈空且有 toast 消息) */
static bool toast_active(void) {
    return (s_top < 0) && (s_toast_msg[0] != 0) &&
           (now_ms() - s_toast_ms < 1000);
}

/* ============ 渲染 (不清屏, 覆盖在底层之上) ============ */
static void dlg_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;

    if (toast_active()) {
        int len = (int)strlen(s_toast_msg);
        int w = 16 + len * 16 + 16;
        if (w > UI_SCREEN_W - 40) w = UI_SCREEN_W - 40;
        int x0 = (UI_SCREEN_W - w) / 2;
        int y0 = (UI_SCREEN_H - 40) / 2;
        draw_dialog_frame(lcd, x0, y0, w, 40, 2);
        draw_text_centered(lcd, y0 + 10, s_toast_msg, false);
        return;
    }

    os_dlg_stack_t *d = top();
    if (!d) return;

    /* 自定义渲染: 若消费则不再画默认列表 */
    if (d->on_render && d->on_render(ctx, d, d->ud)) return;

    int h = DLG_TITLE_H + 4 + d->count * DLG_ROW + 8;
    int y0 = (UI_SCREEN_H - h) / 2;
    draw_dialog_frame(lcd, DLG_X0, y0, DLG_X1 - DLG_X0 + 1, h, 3);
    draw_text_centered(lcd, y0 + 2, d->title, false);
    for (int i = 0; i < d->count; i++) {
        int y = y0 + DLG_TITLE_H + 4 + i * DLG_ROW;
        bool sel = (i == d->sel);
        if (sel) fill_rect(lcd, DLG_X0 + 3, y, DLG_X1 - 3, y + DLG_ROW - 2, ST7305_COLOR_BLACK);
        draw_text(lcd, DLG_X0 + 14, y + (DLG_ROW - 16) / 2, d->items[i], sel);
    }
}

/* ============ 动作 ============ */
static void dlg_finish_top(ui_ctx_t *ctx, int result) {
    os_dlg_stack_t *d = top();
    if (!d) return;
    os_dialog_cb_t cb = d->cb;
    void *ud = d->ud;
    os_dialog_pop(ctx);   /* 先弹栈 (关闭本层/弹窗页), 再回调: 回调内可开新弹窗 */
    if (cb) cb(ctx, result, ud);
}

static void dlg_action(ui_ctx_t *ctx, os_action_t a) {
    if (toast_active()) { os_pop(ctx); s_toast_msg[0] = 0; return; }
    os_dlg_stack_t *d = top();
    if (!d) return;
    /* 自定义按键: 消费则不再走默认 */
    if (d->on_key && d->on_key(ctx, d, a, d->ud)) return;
    switch (a) {
    case OS_ACTION_UP:
        if (d->sel > 0) d->sel--;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_DOWN:
        if (d->sel < d->count - 1) d->sel++;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM:
        dlg_finish_top(ctx, d->sel);
        break;
    case OS_ACTION_BACK:
        dlg_finish_top(ctx, -1);
        break;
    default:
        break;
    }
}

static bool dlg_touch(ui_ctx_t *ctx, int x, int y) {
    if (toast_active()) { os_pop(ctx); s_toast_msg[0] = 0; return true; }
    os_dlg_stack_t *d = top();
    if (!d) return false;
    if (x < DLG_X0 || x > DLG_X1) return false;
    int h = DLG_TITLE_H + 4 + d->count * DLG_ROW + 8;
    int y0 = (UI_SCREEN_H - h) / 2;
    int rel = y - (y0 + DLG_TITLE_H + 4);
    if (rel < 0) return false;
    int idx = rel / DLG_ROW;
    if (idx >= 0 && idx < d->count) {
        d->sel = idx;
        dlg_finish_top(ctx, idx);
        return true;
    }
    return false;
}

static void dlg_poll(ui_ctx_t *ctx) {
    if (toast_active() && now_ms() - s_toast_ms > 1000) {
        os_pop(ctx);
        s_toast_msg[0] = 0;
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

/* ============ 弹窗栈 API ============ */
void os_dialog_push(ui_ctx_t *ctx, const os_dlg_stack_t *dlg) {
    if (!dlg) return;
    if (s_top + 1 >= DLG_MAX_DEPTH) return;   /* 栈满: 拒绝 */
    if (s_top < 0) os_push(ctx, OS_PAGE_DIALOG);   /* 首层: 打开弹窗页 */
    s_stack[++s_top] = *dlg;                       /* 整体拷贝, 含回调/ud/自定义 */
}

void os_dialog_pop(ui_ctx_t *ctx) {
    if (s_top > 0) {
        s_top--;   /* 弹栈, 恢复父弹窗 (sel 已在各自层保存) */
    } else if (s_top == 0) {
        s_top = -1;
        os_pop(ctx);   /* 关闭弹窗页, 恢复底层 */
    }
    ctx->needs_redraw = true;
}

int os_dialog_depth(void) { return s_top + 1; }
const os_dlg_stack_t *os_dialog_top(void) { return top(); }

/* ============ 旧 API (单层快捷方式, 复用弹窗栈) ============ */
void os_dialog_list(ui_ctx_t *ctx, const char *title,
                    const char *const *items, int count, int sel,
                    os_dialog_cb_t cb, void *ud) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.title, sizeof(dlg.title), "%s", title ? title : "");
    dlg.count = count < DLG_MAX_ITEMS ? count : DLG_MAX_ITEMS;
    if (dlg.count < 0) dlg.count = 0;
    for (int i = 0; i < dlg.count; i++)
        snprintf(dlg.items[i], DLG_ITEM_LEN, "%s", items[i]);
    dlg.sel = sel < 0 ? 0 : sel;
    if (dlg.sel >= dlg.count) dlg.sel = 0;
    dlg.cb = cb;
    dlg.ud = ud;
    os_dialog_push(ctx, &dlg);
}

void os_dialog_confirm(ui_ctx_t *ctx, const char *msg, os_dialog_cb_t cb, void *ud) {
    const char *items[2] = { "\xe7\xa1\xae\xe5\xae\x9a", "\xe5\x8f\x96\xe6\xb6\x88" }; /* 确定/取消 */
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.title, sizeof(dlg.title), "%s", msg ? msg : "");
    dlg.count = 2;
    snprintf(dlg.items[0], DLG_ITEM_LEN, "%s", items[0]);
    snprintf(dlg.items[1], DLG_ITEM_LEN, "%s", items[1]);
    dlg.sel = 0;
    dlg.cb = cb;
    dlg.ud = ud;
    os_dialog_push(ctx, &dlg);
}

void os_dialog_toast(ui_ctx_t *ctx, const char *msg) {
    snprintf(s_toast_msg, sizeof(s_toast_msg), "%s", msg ? msg : "");
    s_toast_ms = now_ms();
    if (s_top < 0) os_push(ctx, OS_PAGE_DIALOG);   /* 无弹窗栈时需打开弹窗页 */
}
