/**
 * page_select_game.c — 游戏 页面模块 (双栏: 左目录 / 右游戏).
 *
 * 目录: /sdcard/gam 下的子文件夹 + 根目录 .gam 游戏.
 * 交互: 左右键切栏, 上下键选行, 确认键启动游戏 (gam4980 引擎, 阻塞运行).
 * 私有 state 一律 static 留在本文件, 不放进 ui_ctx.
 */
#include "os.h"
#include "ui_common.h"
#include "gam4980_emu.h"
#include "esp_log.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

#define TAG "PGAME"

#define GAM_ROOT   "/sdcard/gam"
#define MAX_FOLDERS 24
#define MAX_GAMES   128
#define ROW_H       32          /* 每行高度 */
#define LIST_Y      30          /* 列表起点 (状态栏 24px + 6px 间距) */
#define LIST_BOTTOM (UI_SCREEN_H - 4)
#define LEFT_W      100
#define GAP         6

/* 私有 state (模块私有, 不放进 ui_ctx) */
typedef struct {
    char  folders[MAX_FOLDERS][64];
    int   folder_count;
    char  games[MAX_GAMES][64];      /* 显示名 (去扩展名) */
    char  game_paths[MAX_GAMES][160];/* 完整路径 (启动用) */
    int   game_count;
    int   sel_folder;                /* 左栏: 0=全部, 1..=子文件夹 */
    int   sel_game;                  /* 右栏 */
    int   folder_scroll;
    int   game_scroll;
    int   focus;                     /* 0=左栏, 1=右栏 */
    bool  loaded;
} game_state_t;
static game_state_t s_g;

/* ---- 扫描: /sdcard/gam ---- */
static void strip_ext(char *s) {
    char *dot = strrchr(s, '.');
    if (dot) *dot = '\0';
}
static bool is_gam(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".gam") == 0);
}

static int scan_folders(void) {
    DIR *dir = opendir(GAM_ROOT);
    if (!dir) { s_g.folder_count = 0; return 0; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < MAX_FOLDERS) {
        if (e->d_name[0] == '.') continue;
        if (e->d_type != DT_DIR) continue;
        snprintf(s_g.folders[n], sizeof(s_g.folders[n]), "%.63s", e->d_name);
        n++;
    }
    closedir(dir);
    /* 排序: 简单按文件名 (as-is, 足够) */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(s_g.folders[i], s_g.folders[j]) > 0) {
                char tmp[64];
                strcpy(tmp, s_g.folders[i]);
                strcpy(s_g.folders[i], s_g.folders[j]);
                strcpy(s_g.folders[j], tmp);
            }
        }
    }
    s_g.folder_count = n;
    return n;
}

/* 扫描某目录下的 .gam (dir 为空=根目录), 填充 games/game_paths */
static int scan_games_in(const char *dir, int max) {
    int n = 0;
    char path[128];
    if (dir && dir[0])
        snprintf(path, sizeof(path), "%s/%s", GAM_ROOT, dir);
    else
        snprintf(path, sizeof(path), "%s", GAM_ROOT);
    DIR *d = opendir(path);
    if (!d) { s_g.game_count = 0; return 0; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_name[0] == '.') continue;
        if (!is_gam(e->d_name)) continue;
        snprintf(s_g.games[n], sizeof(s_g.games[n]), "%.63s", e->d_name);
        strip_ext(s_g.games[n]);
        if (dir && dir[0])
            snprintf(s_g.game_paths[n], sizeof(s_g.game_paths[n]), "%s/%.63s/%.63s", GAM_ROOT, dir, e->d_name);
        else
            snprintf(s_g.game_paths[n], sizeof(s_g.game_paths[n]), "%s/%.63s", GAM_ROOT, e->d_name);
        n++;
    }
    closedir(d);
    s_g.game_count = n;
    return n;
}

static void build_right(void) {
    const char *dir = (s_g.sel_folder == 0) ? "" : s_g.folders[s_g.sel_folder - 1];
    scan_games_in(dir, MAX_GAMES);
    if (s_g.sel_game >= s_g.game_count) s_g.sel_game = 0;
    if (s_g.game_scroll > s_g.sel_game) s_g.game_scroll = s_g.sel_game;
}

static void p_select_game_enter(ui_ctx_t *ctx) {
    (void)ctx;
    if (!s_g.loaded) {
        scan_folders();
        s_g.sel_folder = 0;
        s_g.focus = 0;
        s_g.sel_game = 0;
        s_g.loaded = true;
    }
    build_right();
}

static void p_select_game_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    int lx0 = 2;
    int lx1 = lx0 + LEFT_W - 4;
    int rx0 = lx1 + GAP + 4;

    /* ---- 左栏: 全部 + 子文件夹 (文字居中, 选中=下方 2px 横线) ---- */
    int total = 1 + s_g.folder_count;
    int max_vis = (LIST_BOTTOM - LIST_Y) / ROW_H;
    if (max_vis < 1) max_vis = 1;
    int sel = s_g.sel_folder;
    if (sel < s_g.folder_scroll) s_g.folder_scroll = sel;
    if (sel >= s_g.folder_scroll + max_vis) s_g.folder_scroll = sel - max_vis + 1;
    int y = LIST_Y;
    for (int i = 0; i < max_vis && (s_g.folder_scroll + i) < total; i++) {
        int idx = s_g.folder_scroll + i;
        const char *name = (idx == 0) ? "\xe5\x85\xa8\xe9\x83\xa8" : s_g.folders[idx - 1]; /* 全部 */
        if (s_g.focus == 0 && idx == s_g.sel_folder)
            draw_hline(lcd, lx0, lx1, y + ROW_H - 3, ST7305_COLOR_BLACK);
        draw_text(lcd, lx0 + 4, y + (ROW_H - 16) / 2, name, false);
        y += ROW_H;
    }

    /* 分隔线 */
    draw_vline(lcd, lx1 + GAP / 2, LIST_Y, LIST_BOTTOM, ST7305_COLOR_BLACK);

    /* ---- 右栏: 游戏 ---- */
    max_vis = (LIST_BOTTOM - LIST_Y) / ROW_H;
    if (s_g.sel_game < s_g.game_scroll) s_g.game_scroll = s_g.sel_game;
    if (s_g.sel_game >= s_g.game_scroll + max_vis) s_g.game_scroll = s_g.sel_game - max_vis + 1;
    y = LIST_Y;
    for (int i = 0; i < max_vis && (s_g.game_scroll + i) < s_g.game_count; i++) {
        int idx = s_g.game_scroll + i;
        if (s_g.focus == 1 && idx == s_g.sel_game)
            draw_hline(lcd, rx0, UI_SCREEN_W - 4, y + ROW_H - 3, ST7305_COLOR_BLACK);
        draw_text(lcd, rx0 + 4, y + (ROW_H - 16) / 2, s_g.games[idx], false);
        y += ROW_H;
    }
}

static void p_select_game_action(ui_ctx_t *ctx, os_action_t a) {
    int total = 1 + s_g.folder_count;
    switch (a) {
    case OS_ACTION_LEFT:
        if (s_g.focus != 0) { s_g.focus = 0; ctx->needs_redraw = true; }
        break;
    case OS_ACTION_RIGHT:
        if (s_g.focus != 1) { s_g.focus = 1; ctx->needs_redraw = true; }
        break;
    case OS_ACTION_UP:
        if (s_g.focus == 0) {
            if (s_g.sel_folder > 0) s_g.sel_folder--;
        } else {
            if (s_g.sel_game > 0) s_g.sel_game--;
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_DOWN:
        if (s_g.focus == 0) {
            if (s_g.sel_folder < total - 1) s_g.sel_folder++;
        } else {
            if (s_g.sel_game < s_g.game_count - 1) s_g.sel_game++;
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM:
        if (s_g.focus == 0) {
            if (s_g.sel_folder > 0) { build_right(); ctx->needs_redraw = true; }
        } else {
            if (s_g.sel_game >= 0 && s_g.sel_game < s_g.game_count) {
                /* 启动游戏: 加载 → 恢复存档 → 阻塞运行 → 保存存档 */
                const char *path = s_g.game_paths[s_g.sel_game];
                ESP_LOGI(TAG, "启动: %s", path);
                if (gam4980_emu_load(path) == 0) {
                    gam4980_emu_load_state(path);
                    gam4980_emu_run();
                    gam4980_emu_save_state(path);
                }
                ctx->needs_redraw = true;   /* 游戏退出后重绘列表 */
            }
        }
        break;
    case OS_ACTION_BACK:
        os_pop(ctx);
        break;
    default:
        break;
    }
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
