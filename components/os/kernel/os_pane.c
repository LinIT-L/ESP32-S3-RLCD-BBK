/**
 * os_pane.c — 双栏选择界面公共模板实现 (3.3 游戏菜单样式复刻).
 *
 * 左栏: [引擎优化(可选首项)] [全部?] [子文件夹...]
 *   - 引擎优化: 选中后右栏内联显示设置项 (不弹窗, 连 API 直接切换)
 *   - 文字水平居中, 超 4 字截断; 选中项下方 2px 粗横线 (贴行底缘)
 * 右栏: 游戏/内容 居中显示, 选中整行反色 (黑底白字)
 * 从 page_select_game / page_book 抽取 (原重复逻辑归一).
 */
#include "os_pane.h"
#include "ui_common.h"
#include "input.h"
#include "favorites.h"
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "esp_log.h"

#define TAG "PANE"

#define ROW_H       32
#define LIST_Y      48   /* 默认起点 (有状态栏/按钮行时) */
#define LIST_BOTTOM (UI_SCREEN_H - 4)
#define LEFT_W      100
#define GAP         6

/* 扫描目录项上限: 防止巨目录 (数千文件) 全量遍历拖慢切换 */
#define PANE_SCAN_MAX 512

/* 注: ESP-IDF fatfs readdir 会正确填充 d_type (vfs_fat.c), 直接用 d_type 判断目录,
 * 不要对每个目录项 stat() (FATFS 上开销大, 大量文件时极慢). */

static void strip_ext(char *s) {
    char *dot = strrchr(s, '.');
    if (dot) *dot = '\0';
}

/* 名称截断为最多 max_chars 个字符 (UTF-8 安全, 中文按 1 字算) */
static void pane_name_short(char *dst, size_t dstsz, const char *src, int max_chars) {
    size_t w = 0;
    int n = 0;
    while (*src && n < max_chars) {
        unsigned char c = (unsigned char)*src;
        int clen = (c < 0x80) ? 1 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xE0) == 0xC0) ? 2 : 4;
        if (w + (size_t)clen + 1 > dstsz) break;
        for (int i = 0; i < clen && src[i]; i++) dst[w++] = src[i];
        src += clen;
        n++;
    }
    dst[w] = '\0';
}

/* 根目录是否有直接文件 (不含子目录): 无 → 隐藏左栏"全部"项 */
static bool pane_root_has_item(const os_pane_t *p) {
    DIR *d = opendir(p->root);
    if (!d) return false;
    bool found = false;
    int scanned = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && !found && ++scanned <= PANE_SCAN_MAX) {
        if (e->d_name[0] == '.') continue;
        if (e->d_type == DT_DIR) continue;
        if (p->is_item && p->is_item(e->d_name)) found = true;
    }
    closedir(d);
    return found;
}

/* 左栏来源: 0=引擎优化(设置) 1=收藏栏 2=全部 3=子目录; <0=越界 */
static int pane_src(const os_pane_t *p, int sel_folder) {
    int idx = sel_folder;
    if (p->settings_label) { if (idx-- == 0) return 0; }
    if (p->fav_enabled) { if (idx-- == 0) return 1; }
    if (p->hide_all) return (idx < p->folder_count) ? 3 : -1;
    if (idx == 0) return 2;                 /* 全部 */
    return (idx - 1 < p->folder_count) ? 3 : -1;
}

/* 左栏总项数 = [引擎优化?] + [收藏栏?] + [全部?] + 子文件夹 */
static int pane_total(const os_pane_t *p) {
    int n = p->folder_count;
    if (p->settings_label) n++;
    if (p->fav_enabled) n++;
    if (!p->hide_all) n++;
    return n;
}

/* 返回 sel_folder 对应的目录: NULL=设置, FAVSENTINEL=收藏栏, ""=根(全部), 其余=子目录名 */
static const char *pane_folder_of(const os_pane_t *p, int sel_folder) {
    int src = pane_src(p, sel_folder);
    if (src == 0) return NULL;                        /* 引擎优化/设置 */
    if (src == 1) return (const char *)(uintptr_t)&p->fav_engine; /* 收藏栏标记 */
    if (src == 2) return "";                          /* 全部 = 根 */
    if (src < 0) return "";
    /* 子目录: 重新映射回 folders[] 下标 */
    int idx = sel_folder;
    if (p->settings_label) idx--;
    if (p->fav_engine >= 0) idx--;
    if (!p->hide_all) idx--;                          /* 越过"全部"占位 */
    if (idx < 0 || idx >= p->folder_count) return "";
    return p->folders[idx];
}

/* 左栏显示名 (disp 为显示索引, 0 起) */
static void pane_left_name(const os_pane_t *p, int disp, char *out, size_t n) {
    int idx = disp;
    if (p->settings_label && idx-- == 0) { pane_name_short(out, n, p->settings_label, 4); return; }
    if (p->fav_enabled && idx-- == 0) { pane_name_short(out, n, "\xe6\x94\xb6\xe8\x97\x8f\xe6\xa0\x8f", 4); return; } /* 收藏栏 */
    if (p->hide_all) { pane_name_short(out, n, p->folders[idx], 4); return; }
    if (idx == 0) pane_name_short(out, n, (p->all_label ? p->all_label : "\xe5\x85\xa8\xe9\x83\xa8"), 4); /* 全部 */
    else pane_name_short(out, n, p->folders[idx - 1], 4);
}

void os_pane_reset(os_pane_t *p) {
    uint32_t t0 = esp_timer_get_time();
    DIR *dir = opendir(p->root);
    p->folder_count = 0;
    if (dir) {
        int n = 0;
        int scanned = 0;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && n < 24 && ++scanned <= PANE_SCAN_MAX) {
            if (e->d_name[0] == '.') continue;
            if (e->d_type != DT_DIR) continue;
            snprintf(p->folders[n], sizeof(p->folders[n]), "%.63s", e->d_name);
            n++;
        }
        closedir(dir);
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
    p->hide_all = !pane_root_has_item(p);
    p->settings_mode = false;
    p->fav_mode = false;
    /* 默认选中: 有收藏栏则默认收藏栏, 其次 设置后的 全部/首个文件夹 */
    p->sel_folder = (p->settings_label ? 1 : 0) + (p->fav_enabled ? 1 : 0);
    if (p->sel_folder >= pane_total(p)) p->sel_folder = 0;
    p->focus = 0;
    p->sel_item = 0;
    p->built_folder = -1;   /* 强制首次 build */
    p->loaded = true;
    ESP_LOGI(TAG, "reset: %s folders=%d hide_all=%d sel=%d 耗时%luus", p->root,
             p->folder_count, (int)p->hide_all, p->sel_folder,
             (unsigned long)(esp_timer_get_time() - t0));
}

void os_pane_build_right(os_pane_t *p) {
    /* 同一分类反复点击/确认不重扫 (触摸左栏每次点击都会调用) */
    if (p->built_folder == p->sel_folder) return;
    p->built_folder = p->sel_folder;
    uint32_t t0 = esp_timer_get_time();
    const char *dir = pane_folder_of(p, p->sel_folder);
    /* 收藏栏: 右栏列出该引擎的收藏 */
    if (p->fav_enabled && dir == (const char *)(uintptr_t)&p->fav_engine) {
        p->fav_mode = true;
        p->item_count = 0;
        int cnt = 0;
        const char *const *favs = favorites_list((fav_engine_t)p->fav_engine, &cnt);
        int n = 0;
        for (int i = 0; i < cnt && n < 128; i++) {
            const char *fp = favs[i];
            if (!fp || !fp[0]) continue;
            char tmp[160];
            snprintf(tmp, sizeof(tmp), "%.63s", fp);
            /* 取文件名去后缀显示 */
            char *base = strrchr(tmp, '/');
            if (base) memmove(tmp, base + 1, strlen(base));
            strip_ext(tmp);
            snprintf(p->items[n], sizeof(p->items[n]), "%s", tmp);
            snprintf(p->item_paths[n], sizeof(p->item_paths[n]), "%s", fp);
            n++;
        }
        p->item_count = n;
        if (p->sel_item >= p->item_count) p->sel_item = 0;
        if (p->item_scroll > p->sel_item) p->item_scroll = p->sel_item;
        return;
    }
    if (!dir) {
        /* 引擎优化/设置: 右栏显示设置项 */
        p->settings_mode = true;
        p->item_count = 0;
        if (p->sel_item >= p->settings_count) p->sel_item = 0;
        if (p->item_scroll > p->sel_item) p->item_scroll = p->sel_item;
        return;
    }
    p->settings_mode = false;
    p->fav_mode = false;
    char path[128];
    if (dir[0]) snprintf(path, sizeof(path), "%s/%s", p->root, dir);
    else        snprintf(path, sizeof(path), "%s", p->root);
    DIR *d = opendir(path);
    p->item_count = 0;
    if (d) {
        struct dirent *e;
        int n = 0;
        int scanned = 0;
        while ((e = readdir(d)) != NULL && n < 128 && ++scanned <= PANE_SCAN_MAX) {
            if (e->d_name[0] == '.') continue;
            /* 带路径过滤优先 (文曲星把游戏文件夹当一项) */
            if (p->is_item_d) {
                if (dir[0])
                    snprintf(path, sizeof(path), "%s/%s", p->root, dir);
                else
                    snprintf(path, sizeof(path), "%s", p->root);
                if (!p->is_item_d(path, e->d_name)) continue;
            } else if (p->is_item && !p->is_item(e->d_name)) continue;
            snprintf(p->items[n], sizeof(p->items[n]), "%.63s", e->d_name);
            strip_ext(p->items[n]);
            if (dir[0])
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
    ESP_LOGI(TAG, "build_right: %s items=%d 耗时%luus", path, p->item_count,
             (unsigned long)(esp_timer_get_time() - t0));
}

void os_pane_render(ui_ctx_t *ctx, os_pane_t *p) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    const int list_y = p->list_y > 0 ? p->list_y : LIST_Y;
    int lx0 = 2;
    int lx1 = lx0 + LEFT_W - 4;
    int rx0 = lx1 + GAP + 4;

    /* 左栏: 文字顶 y+8, 水平居中, 超4字截断; 选中项底部 2px 粗横线 */
    int total = pane_total(p);
    int max_vis = (LIST_BOTTOM - list_y) / ROW_H;
    if (max_vis < 1) max_vis = 1;
    if (p->sel_folder < p->folder_scroll) p->folder_scroll = p->sel_folder;
    if (p->sel_folder >= p->folder_scroll + max_vis) p->folder_scroll = p->sel_folder - max_vis + 1;
    int y = list_y;
    for (int i = 0; i < max_vis && (p->folder_scroll + i) < total; i++) {
        int idx = p->folder_scroll + i;
        char short_name[64];
        pane_left_name(p, idx, short_name, sizeof(short_name));
        int tw = text_width(short_name);
        int tx = lx0 + (LEFT_W - tw) / 2;
        if (tx < lx0 + 2) tx = lx0 + 2;
        draw_text(lcd, tx, y + 4, short_name, false);   /* 24px 字在 32px 行垂直居中 */
        if (p->focus == 0 && idx == p->sel_folder) {
            draw_hline(lcd, lx0, lx1, y + ROW_H - 1, ST7305_COLOR_BLACK);
            draw_hline(lcd, lx0, lx1, y + ROW_H,     ST7305_COLOR_BLACK);
        }
        y += ROW_H;
    }
    draw_vline(lcd, lx1 + GAP / 2, list_y, LIST_BOTTOM, ST7305_COLOR_BLACK);

    /* 右栏: 设置模式走调用方自定义渲染 (内联设置项) */
    if (p->settings_mode) {
        if (p->settings_render)
            p->settings_render(ctx, p, rx0, UI_SCREEN_W - 4, list_y);
        return;
    }

    /* 右栏: 游戏/内容, 居中, 选中整行反色 */
    max_vis = (LIST_BOTTOM - list_y) / ROW_H;
    if (p->sel_item < p->item_scroll) p->item_scroll = p->sel_item;
    if (p->sel_item >= p->item_scroll + max_vis) p->item_scroll = p->sel_item - max_vis + 1;
    y = list_y;
    for (int i = 0; i < max_vis && (p->item_scroll + i) < p->item_count; i++) {
        int idx = p->item_scroll + i;
        bool sel = (p->focus == 1 && idx == p->sel_item);
        int tw = text_width(p->items[idx]);
        int tx = rx0 + ((UI_SCREEN_W - 4 - rx0) - tw) / 2;   /* 右栏水平居中 */
        if (tx < rx0) tx = rx0;
        if (sel) fill_rect(lcd, rx0, y, UI_SCREEN_W - 4, y + ROW_H - 2, ST7305_COLOR_BLACK);
        draw_text(lcd, tx, y + 4, p->items[idx], sel);   /* 24px 字在 32px 行垂直居中 */
        y += ROW_H;
    }
    /* 收藏栏为空提示 */
    if (p->fav_mode && p->item_count == 0) {
        draw_text_centered(lcd, list_y + ROW_H + 2,
                   "\xe6\x9a\x82\xe6\x97\xa0\xe6\x94\xb6\xe8\x97\x8f\xef\xbc\x8c\xe9\x95\xbf\xe6\x8c\x89\xe6\xb8\xb8\xe6\x88\x8f\xe5\x8a\xa0\xe5\x85\xa5", false); /* 暂无收藏，长按游戏加入 */
    }
}

bool os_pane_action(ui_ctx_t *ctx, os_pane_t *p, os_action_t a) {
    int total = pane_total(p);
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
        if (p->focus == 0) {
            if (p->sel_folder < total - 1) p->sel_folder++;
        } else {
            int mx = p->settings_mode ? (p->settings_count - 1) : (p->item_count - 1);
            if (p->sel_item < mx) p->sel_item++;
        }
        ctx->needs_redraw = true;
        return true;
    case OS_ACTION_CONFIRM:
        if (p->focus == 0) {
            /* 左栏任意项确认 → 切换右栏 (设置/全部/子目录) */
            os_pane_build_right(p);
            ctx->needs_redraw = true;
        } else {
            if (p->settings_mode) {
                if (p->sel_item >= 0 && p->sel_item < p->settings_count && p->on_settings_select) {
                    p->on_settings_select(ctx, p, p->sel_item);
                    ctx->needs_redraw = true;
                }
            } else if (p->sel_item >= 0 && p->sel_item < p->item_count && p->on_select) {
                p->on_select(ctx, p, p->item_paths[p->sel_item], p->items[p->sel_item]);
                ctx->needs_redraw = true;
            }
        }
        return true;
    case OS_ACTION_LONG_PRESS:
        /* 长按(约1s,按住即弹提示,不必松手) 收藏/取消收藏. 仅收藏支持页面生效 */
        if (p->focus == 1 && !p->settings_mode && p->fav_enabled) {
            const char *path = os_pane_selected_path(p);
            if (path && path[0]) {
                if (p->fav_mode) {
                    favorites_remove((fav_engine_t)p->fav_engine, path);
                    os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x8f\x96\xe6\xb6\x88\xe6\x94\xb6\xe8\x97\x8f"); /* 已取消收藏 */
                } else {
                    bool added = favorites_add(favorites_engine_for_path(path), path);
                    os_dialog_toast(ctx, added ? "\xe5\xb7\xb2\xe6\x94\xb6\xe8\x97\x8f"   /* 已收藏 */
                                                : "\xe6\x94\xb6\xe8\x97\x8f\xe5\xa4\xb1\xe8\xb4\xa5"); /* 收藏失败 */
                }
                if (p->fav_mode) {   /* 收藏栏取消后重扫, 立即移除该项 */
                    favorites_prune_missing();
                    os_pane_build_right(p);
                }
                ctx->needs_redraw = true;
            }
            return true;
        }
        return false;   /* 非收藏页不消费, 交调用方 */
    default:
        return false;
    }
}

bool os_pane_touch(ui_ctx_t *ctx, os_pane_t *p, int x, int y) {
    const int list_y = p->list_y > 0 ? p->list_y : LIST_Y;
    if (y < list_y || y >= LIST_BOTTOM) return false;
    /* 左栏: 切目录/设置 */
    if (x < LEFT_W) {
        int idx = p->folder_scroll + (y - list_y) / ROW_H;
        if (idx < pane_total(p)) {
            if (p->sel_folder != idx) {   /* 同项点击不重扫 */
                p->sel_folder = idx;
                p->focus = 0;
                os_pane_build_right(p);
            }
            if (ctx) ctx->needs_redraw = true;
            return true;
        }
        return false;
    }
    /* 右栏: 设置项或内容项 */
    if (p->settings_mode) {
        int idx = (y - list_y) / ROW_H;
        if (idx >= p->settings_count) return false;
        p->sel_item = idx;
        p->focus = 1;
        if (ctx) ctx->needs_redraw = true;
        if (p->on_settings_select) p->on_settings_select(ctx, p, idx);
        return true;
    }
    int idx = p->item_scroll + (y - list_y) / ROW_H;
    if (idx >= p->item_count) return false;
    p->sel_item = idx;
    p->focus = 1;
    if (ctx) ctx->needs_redraw = true;
    if (p->on_select) p->on_select(ctx, p, p->item_paths[idx], p->items[idx]);
    return true;
}

const char *os_pane_selected_path(os_pane_t *p) {
    if (p->settings_mode) return NULL;
    if (p->sel_item < 0 || p->sel_item >= p->item_count) return NULL;
    return p->item_paths[p->sel_item];
}

/* 每帧: 右栏触摸拖动滚动 (3.3 行为 — 拖动查看更多游戏/内容).
 * 只处理"右栏内容区"的垂直拖动: 位移≥1行(32px)滚一行.
 * 输入层已区分: 大位移拖动手势不投递 tap, 不会误触发选中/启动. */
void os_pane_poll(ui_ctx_t *ctx, os_pane_t *p) {
    int tx, ty;
    static int s_last_y = -1;
    static int s_acc = 0;
    bool down = input_get_touch_pos(&tx, &ty);
    if (down) {
        if (s_last_y < 0) { s_last_y = ty; s_acc = 0; return; }
        int dy = ty - s_last_y;
        s_last_y = ty;
        if (tx >= LEFT_W && !p->settings_mode) {   /* 右栏内容区 (设置模式不滚) */
            s_acc += dy;
            bool moved = false;
            int max_scroll = p->item_count > 0 ? p->item_count - 1 : 0;
            while (s_acc >= ROW_H && p->item_scroll < max_scroll) { p->item_scroll++; s_acc -= ROW_H; moved = true; }
            while (s_acc <= -ROW_H && p->item_scroll > 0)         { p->item_scroll--; s_acc += ROW_H; moved = true; }
            if (moved) ctx->needs_redraw = true;
        }
    } else {
        s_last_y = -1;
        s_acc = 0;
    }
}
