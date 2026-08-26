/**
 * page_book.c — 电子书 页面模块.
 *
 * 书架双栏 (左分类 / 右书籍) + 阅读器模式 (book_reader 组件, 运行时全屏).
 * 私有 state 一律 static 留在本文件, 不放进 ui_ctx.
 */
#include "os.h"
#include "ui_common.h"
#include "book_reader.h"
#include "esp_log.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

#define TAG "PBOOK"

#define BOOK_ROOT  "/sdcard/books"
#define MAX_FOLDERS 24
#define MAX_BOOKS   128
#define ROW_H       32
#define LIST_Y      30
#define LIST_BOTTOM (UI_SCREEN_H - 4)
#define LEFT_W      100
#define GAP         6

/* 私有 state (模块私有) */
typedef struct {
    char  folders[MAX_FOLDERS][64];
    int   folder_count;
    char  books[MAX_BOOKS][64];      /* 显示名 (去 .txt) */
    char  book_paths[MAX_BOOKS][160];
    int   book_count;
    int   sel_folder;                /* 0=全部, 1..=子目录 */
    int   sel_book;
    int   folder_scroll;
    int   book_scroll;
    int   focus;                     /* 0=左栏, 1=右栏 */
    bool  loaded;
    bool  reader_open;               /* 阅读器模式 (运行时全屏) */
} book_state_t;
static book_state_t s_b;

static void strip_ext(char *s) {
    char *dot = strrchr(s, '.');
    if (dot) *dot = '\0';
}
static bool is_txt(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".txt") == 0);
}

static void scan_folders(void) {
    DIR *dir = opendir(BOOK_ROOT);
    if (!dir) { s_b.folder_count = 0; return; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < MAX_FOLDERS) {
        if (e->d_name[0] == '.') continue;
        if (e->d_type != DT_DIR) continue;
        snprintf(s_b.folders[n], sizeof(s_b.folders[n]), "%.63s", e->d_name);
        n++;
    }
    closedir(dir);
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(s_b.folders[i], s_b.folders[j]) > 0) {
                char tmp[64];
                strcpy(tmp, s_b.folders[i]); strcpy(s_b.folders[i], s_b.folders[j]); strcpy(s_b.folders[j], tmp);
            }
    s_b.folder_count = n;
}

static void scan_books_in(const char *dir, int max) {
    int n = 0;
    char path[128];
    if (dir && dir[0]) snprintf(path, sizeof(path), "%s/%s", BOOK_ROOT, dir);
    else               snprintf(path, sizeof(path), "%s", BOOK_ROOT);
    DIR *d = opendir(path);
    if (!d) { s_b.book_count = 0; return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_name[0] == '.') continue;
        if (!is_txt(e->d_name)) continue;
        snprintf(s_b.books[n], sizeof(s_b.books[n]), "%.63s", e->d_name);
        strip_ext(s_b.books[n]);
        if (dir && dir[0])
            snprintf(s_b.book_paths[n], sizeof(s_b.book_paths[n]), "%s/%.63s/%.63s", BOOK_ROOT, dir, e->d_name);
        else
            snprintf(s_b.book_paths[n], sizeof(s_b.book_paths[n]), "%s/%.63s", BOOK_ROOT, e->d_name);
        n++;
    }
    closedir(d);
    s_b.book_count = n;
}

static void build_right(void) {
    const char *dir = (s_b.sel_folder == 0) ? "" : s_b.folders[s_b.sel_folder - 1];
    scan_books_in(dir, MAX_BOOKS);
    if (s_b.sel_book >= s_b.book_count) s_b.sel_book = 0;
    if (s_b.book_scroll > s_b.sel_book) s_b.book_scroll = s_b.sel_book;
}

/* 阅读器默认设置 (后续接入设置页后再改) */
static void apply_reader_defaults(void) {
    book_reader_set_settings(false, 1, false, true, 0, 0, 1, 1, 1, 0);
}

static void p_book_enter(ui_ctx_t *ctx) {
    (void)ctx;
    if (!s_b.loaded) {
        scan_folders();
        s_b.sel_folder = 0;
        s_b.focus = 0;
        s_b.sel_book = 0;
        s_b.loaded = true;
    }
    build_right();
}

static void p_book_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    /* 阅读器模式: 全屏 (隐藏状态栏服务) */
    if (s_b.reader_open && book_reader_is_open()) {
        book_reader_render(lcd);
        ctx->fullscreen = true;
        return;
    }
    ctx->fullscreen = false;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    int lx0 = 2;
    int lx1 = lx0 + LEFT_W - 4;
    int rx0 = lx1 + GAP + 4;

    /* 左栏 */
    int total = 1 + s_b.folder_count;
    int max_vis = (LIST_BOTTOM - LIST_Y) / ROW_H;
    if (max_vis < 1) max_vis = 1;
    if (s_b.sel_folder < s_b.folder_scroll) s_b.folder_scroll = s_b.sel_folder;
    if (s_b.sel_folder >= s_b.folder_scroll + max_vis) s_b.folder_scroll = s_b.sel_folder - max_vis + 1;
    int y = LIST_Y;
    for (int i = 0; i < max_vis && (s_b.folder_scroll + i) < total; i++) {
        int idx = s_b.folder_scroll + i;
        const char *name = (idx == 0) ? "\xe5\x85\xa8\xe9\x83\xa8" : s_b.folders[idx - 1]; /* 全部 */
        if (s_b.focus == 0 && idx == s_b.sel_folder)
            draw_hline(lcd, lx0, lx1, y + ROW_H - 3, ST7305_COLOR_BLACK);
        draw_text(lcd, lx0 + 4, y + (ROW_H - 16) / 2, name, false);
        y += ROW_H;
    }
    draw_vline(lcd, lx1 + GAP / 2, LIST_Y, LIST_BOTTOM, ST7305_COLOR_BLACK);

    /* 右栏 */
    max_vis = (LIST_BOTTOM - LIST_Y) / ROW_H;
    if (s_b.sel_book < s_b.book_scroll) s_b.book_scroll = s_b.sel_book;
    if (s_b.sel_book >= s_b.book_scroll + max_vis) s_b.book_scroll = s_b.sel_book - max_vis + 1;
    y = LIST_Y;
    for (int i = 0; i < max_vis && (s_b.book_scroll + i) < s_b.book_count; i++) {
        int idx = s_b.book_scroll + i;
        if (s_b.focus == 1 && idx == s_b.sel_book)
            draw_hline(lcd, rx0, UI_SCREEN_W - 4, y + ROW_H - 3, ST7305_COLOR_BLACK);
        draw_text(lcd, rx0 + 4, y + (ROW_H - 16) / 2, s_b.books[idx], false);
        y += ROW_H;
    }
}

static void open_book(ui_ctx_t *ctx) {
    if (s_b.sel_book < 0 || s_b.sel_book >= s_b.book_count) return;
    apply_reader_defaults();
    if (book_reader_open(s_b.book_paths[s_b.sel_book])) {
        s_b.reader_open = true;
        ctx->fullscreen = true;
        ESP_LOGI(TAG, "打开: %s", s_b.book_paths[s_b.sel_book]);
    }
    ctx->needs_redraw = true;
}

static void p_book_action(ui_ctx_t *ctx, os_action_t a) {
    /* 阅读器模式 */
    if (s_b.reader_open) {
        if (a == OS_ACTION_BACK) {
            book_reader_close();
            s_b.reader_open = false;
            ctx->fullscreen = false;
            ctx->needs_redraw = true;
            return;
        }
        if (book_reader_handle_action((int)a)) {   /* os_action 与 menu_action 值一致 */
            ctx->needs_redraw = true;
        }
        return;
    }

    int total = 1 + s_b.folder_count;
    switch (a) {
    case OS_ACTION_LEFT:  if (s_b.focus != 0) { s_b.focus = 0; ctx->needs_redraw = true; } break;
    case OS_ACTION_RIGHT: if (s_b.focus != 1) { s_b.focus = 1; ctx->needs_redraw = true; } break;
    case OS_ACTION_UP:
        if (s_b.focus == 0) { if (s_b.sel_folder > 0) s_b.sel_folder--; }
        else                { if (s_b.sel_book > 0) s_b.sel_book--; }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_DOWN:
        if (s_b.focus == 0) { if (s_b.sel_folder < total - 1) s_b.sel_folder++; }
        else                { if (s_b.sel_book < s_b.book_count - 1) s_b.sel_book++; }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM:
        if (s_b.focus == 0) {
            if (s_b.sel_folder > 0) { build_right(); ctx->needs_redraw = true; }
        } else {
            open_book(ctx);
        }
        break;
    case OS_ACTION_BACK:
        os_pop(ctx);
        break;
    default:
        break;
    }
}

static bool p_book_touch(ui_ctx_t *ctx, int x, int y) {
    /* 阅读器模式 */
    if (s_b.reader_open) {
        if (book_reader_handle_touch(x, y)) { ctx->needs_redraw = true; return true; }
        return true;   /* 阅读器内全部消费 */
    }
    /* 书架: 右栏点中书籍 → 打开 */
    if (y < LIST_Y || y >= LIST_BOTTOM) return false;
    int idx = s_b.book_scroll + (y - LIST_Y) / ROW_H;
    if (idx >= s_b.book_count) return false;
    s_b.sel_book = idx;
    s_b.focus = 1;
    open_book(ctx);
    return true;
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
