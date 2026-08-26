/**
 * os_pane.c — 双栏选择界面公共模板实现.
 * 从 page_select_game / page_book 抽取 (原重复逻辑归一).
 */
#include "os_pane.h"
#include "ui_common.h"
#include <dirent.h>
#include <string.h>

#define ROW_H       32
#define LIST_Y      30
#define LIST_BOTTOM (UI_SCREEN_H - 4)
#define LEFT_W      100
#define GAP         6

static void strip_ext(char *s) {
    char *dot = strrchr(s, '.');
    if (dot) *dot = '\0';
}

void os_pane_reset(os_pane_t *p) {
    DIR *dir = opendir(p->root);
    p->folder_count = 0;
    if (dir) {
        int n = 0;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && n < 24) {
            if (e->d_name[0] == '.') continue;
            if (e->d_type != DT_DIR) continue;
            snprintf(p->folders[n], sizeof(p->folders[n]), "%.63s", e->d_name);
            n++;
        }
        closedir(dir);
        /* 排序 */
        for (int i = 0; i < n - 1; i++)
            for (int j = i + 1; j < n; j++)
                if (strcmp(p->folders[i], p->folders[j]) > 0) {
                    char tmp[64];
                    strcpy(tmp, p->folders[i]);
                    strcpy(p->folders[i], p->folders[j]);
                    strcpy(p->folders[j], tmp);
                }
        p->folder_count = n;
    }
    p->sel_folder = 0;
    p->focus = 0;
    p->sel_item = 0;
    p->loaded = true;
}

void os_pane_build_right(os_pane_t *p) {
    const char *dir = (p->sel_folder == 0) ? "" : p->folders[p->sel_folder - 1];
    char path[128];
    if (dir && dir[0]) snprintf(path, sizeof(path), "%s/%s", p->root, dir);
    else               snprintf(path, sizeof(path), "%s", p->root);
    DIR *d = opendir(path);
    p->item_count = 0;
    if (d) {
        struct dirent *e;
        int n = 0;
        while ((e = readdir(d)) != NULL && n < 128) {
            if (e->d_name[0] == '.') continue;
            if (p->is_item && !p->is_item(e->d_name)) continue;
            snprintf(p->items[n], sizeof(p->items[n]), "%.63s", e->d_name);
            strip_ext(p->items[n]);
            if (dir && dir[0])
                snprintf(p->item_paths[n], sizeof(p->item_paths[n]), "%s/%.63s/%.63s", p->root, dir, e->d_name);
            else
                snprintf(p->item_paths[n], sizeof(p->item_paths[n]), "%s/%.63s", p->root, e->d_name);
            n++;
        }
        closedir(d);
        p->item_count = n;
    }
    if (p->sel_item >= p->item_count) p->sel_item = 0;
    if (p->item_scroll > p->sel_item) p->item_scroll = p->sel_item;
}

void os_pane_render(ui_ctx_t *ctx, os_pane_t *p) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    int lx0 = 2;
    int lx1 = lx0 + LEFT_W - 4;
    int rx0 = lx1 + GAP + 4;

    /* 左栏 */
    int total = 1 + p->folder_count;
    int max_vis = (LIST_BOTTOM - LIST_Y) / ROW_H;
    if (max_vis < 1) max_vis = 1;
    if (p->sel_folder < p->folder_scroll) p->folder_scroll = p->sel_folder;
    if (p->sel_folder >= p->folder_scroll + max_vis) p->folder_scroll = p->sel_folder - max_vis + 1;
    int y = LIST_Y;
    for (int i = 0; i < max_vis && (p->folder_scroll + i) < total; i++) {
        int idx = p->folder_scroll + i;
        const char *name = (idx == 0)
            ? (p->all_label ? p->all_label : "\xe5\x85\xa8\xe9\x83\xa8") /* 全部 */
            : p->folders[idx - 1];
        if (p->focus == 0 && idx == p->sel_folder)
            draw_hline(lcd, lx0, lx1, y + ROW_H - 3, ST7305_COLOR_BLACK);
        draw_text(lcd, lx0 + 4, y + (ROW_H - 16) / 2, name, false);
        y += ROW_H;
    }
    draw_vline(lcd, lx1 + GAP / 2, LIST_Y, LIST_BOTTOM, ST7305_COLOR_BLACK);

    /* 右栏 */
    max_vis = (LIST_BOTTOM - LIST_Y) / ROW_H;
    if (p->sel_item < p->item_scroll) p->item_scroll = p->sel_item;
    if (p->sel_item >= p->item_scroll + max_vis) p->item_scroll = p->sel_item - max_vis + 1;
    y = LIST_Y;
    for (int i = 0; i < max_vis && (p->item_scroll + i) < p->item_count; i++) {
        int idx = p->item_scroll + i;
        if (p->focus == 1 && idx == p->sel_item)
            draw_hline(lcd, rx0, UI_SCREEN_W - 4, y + ROW_H - 3, ST7305_COLOR_BLACK);
        draw_text(lcd, rx0 + 4, y + (ROW_H - 16) / 2, p->items[idx], false);
        y += ROW_H;
    }
}

bool os_pane_action(ui_ctx_t *ctx, os_pane_t *p, os_action_t a) {
    int total = 1 + p->folder_count;
    switch (a) {
    case OS_ACTION_LEFT:
        if (p->focus != 0) { p->focus = 0; ctx->needs_redraw = true; }
        return true;
    case OS_ACTION_RIGHT:
        if (p->focus != 1) { p->focus = 1; ctx->needs_redraw = true; }
        return true;
    case OS_ACTION_UP:
        if (p->focus == 0) { if (p->sel_folder > 0) p->sel_folder--; }
        else               { if (p->sel_item > 0) p->sel_item--; }
        ctx->needs_redraw = true;
        return true;
    case OS_ACTION_DOWN:
        if (p->focus == 0) { if (p->sel_folder < total - 1) p->sel_folder++; }
        else               { if (p->sel_item < p->item_count - 1) p->sel_item++; }
        ctx->needs_redraw = true;
        return true;
    case OS_ACTION_CONFIRM:
        if (p->focus == 0) {
            if (p->sel_folder > 0) { os_pane_build_right(p); ctx->needs_redraw = true; }
        } else {
            if (p->sel_item >= 0 && p->sel_item < p->item_count && p->on_select) {
                p->on_select(ctx, p, p->item_paths[p->sel_item], p->items[p->sel_item]);
                ctx->needs_redraw = true;
            }
        }
        return true;
    default:
        return false;
    }
}

bool os_pane_touch(ui_ctx_t *ctx, os_pane_t *p, int x, int y) {
    (void)ctx;
    if (y < LIST_Y || y >= LIST_BOTTOM) return false;
    /* 左栏: 切目录 */
    if (x < LEFT_W) {
        int idx = p->folder_scroll + (y - LIST_Y) / ROW_H;
        if (idx < 1 + p->folder_count) {
            p->sel_folder = idx;
            p->focus = 0;
            os_pane_build_right(p);
            return true;
        }
        return false;
    }
    /* 右栏: 选中项 → 回调 */
    int idx = p->item_scroll + (y - LIST_Y) / ROW_H;
    if (idx >= p->item_count) return false;
    p->sel_item = idx;
    p->focus = 1;
    if (p->on_select) p->on_select(ctx, p, p->item_paths[idx], p->items[idx]);
    return true;
}

const char *os_pane_selected_path(os_pane_t *p) {
    if (p->sel_item < 0 || p->sel_item >= p->item_count) return NULL;
    return p->item_paths[p->sel_item];
}
