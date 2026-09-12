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
#include <math.h>

#define TAG "PANE"

#define ROW_H       32
#define LIST_Y      48   /* 默认起点 (有状态栏/按钮行时) */
#define LIST_BOTTOM (ui_screen_h() - 4)   /* 随书库竖屏旋转变化 (400/300 → 300/400 时=396) */
#define LEFT_W      100
#define GAP         6
#define RIGHT_EDGE  (ui_screen_w() - 4)   /* 右栏右缘 (横屏 396 / 竖屏 296) */

/* 列表底缘 (内存数据源页面可设 list_bottom 给底部操作行留空, 默认到屏底-4) */
static int pane_bottom(const os_pane_t *p){ return (p->list_bottom>0) ? p->list_bottom : LIST_BOTTOM; }
/* 左栏可配宽 (页面可设 left_w 收缩, 默认 LEFT_W) */
static int pane_left_w(const os_pane_t *p){ return (p->left_w>0) ? p->left_w : LEFT_W; }

/* V1.5.x: 跟手滚动"防误开"标志 — 本手势发生过滚动时置位, 松手被输入层误判为点击
 * (位移 <40px 判 CONFIRM) 的 tap 在 os_pane_touch 入口吞掉, 防止"浏览游戏/暴龙机
 * 列表轻滑误打开游戏" (自动打开根因). 新手势起始清零, os_pane_touch 消费一次复位. */
static bool s_scroll_suppress_tap = false;

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

/* 左栏来源: 0=引擎优化(设置) 1=收藏栏 2=全部 3=子目录 4=最近阅读; <0=越界 */
static int pane_src(const os_pane_t *p, int sel_folder) {
    int idx = sel_folder;
    if (p->settings_label) { if (idx-- == 0) return 0; }
    if (p->recent_label)   { if (idx-- == 0) return 4; }
    if (p->fav_enabled)    { if (idx-- == 0) return 1; }
    if (p->hide_all) return (idx < p->folder_count) ? 3 : -1;
    if (idx == 0) return 2;                 /* 全部 */
    return (idx - 1 < p->folder_count) ? 3 : -1;
}

/* 左栏总项数 = [引擎优化?] + [最近阅读?] + [收藏栏?] + [全部?] + 子文件夹 */
static int pane_total(const os_pane_t *p) {
    int n = p->folder_count;
    if (p->settings_label) n++;
    if (p->recent_label) n++;
    if (p->fav_enabled) n++;
    if (!p->hide_all) n++;
    return n;
}

/* 返回 sel_folder 对应的目录: NULL=设置, FAVSENTINEL=收藏栏, ""=根(全部), 其余=子目录名 */
static const char *pane_folder_of(const os_pane_t *p, int sel_folder) {
    int src = pane_src(p, sel_folder);
    if (src == 0) return NULL;                        /* 引擎优化/设置 */
    if (src == 4) return (const char *)(uintptr_t)&p->recent_label; /* 最近阅读哨兵 */
    if (src == 1) return (const char *)(uintptr_t)&p->fav_engine; /* 收藏栏标记 */
    if (src == 2) return "";                          /* 全部 = 根 */
    if (src < 0) return "";
    /* 子目录: 重新映射回 folders[] 下标 (收藏栏按 fav_enabled 判定, 未启用时 fav_engine=0 不能误减) */
    int idx = sel_folder;
    if (p->settings_label) idx--;
    if (p->recent_label) idx--;
    if (p->fav_enabled) idx--;
    if (!p->hide_all) idx--;                          /* 越过"全部"占位 */
    if (idx < 0 || idx >= p->folder_count) return "";
    return p->folders[idx];
}

/* 左栏显示名 (disp 为显示索引, 0 起) */
static void pane_left_name(const os_pane_t *p, int disp, char *out, size_t n) {
    int idx = disp;
    if (p->settings_label && idx-- == 0) { pane_name_short(out, n, p->settings_label, 4); return; }
    if (p->recent_label && idx-- == 0) { pane_name_short(out, n, p->recent_label, 4); return; }
    if (p->fav_enabled && idx-- == 0) { pane_name_short(out, n, "\xe6\x94\xb6\xe8\x97\x8f\xe6\xa0\x8f", 4); return; } /* 收藏栏 */
    if (p->hide_all) { pane_name_short(out, n, p->folders[idx], 4); return; }
    if (idx == 0) pane_name_short(out, n, (p->all_label ? p->all_label : "\xe5\x85\xa8\xe9\x83\xa8"), 4); /* 全部 */
    else pane_name_short(out, n, p->folders[idx - 1], 4);
}

void os_pane_reset(os_pane_t *p) {
    uint32_t t0 = esp_timer_get_time();
    if (p->mem_folders) {
        /* 内存数据源: 回调填充 folders[] / folder_count, 并决定 hide_all */
        p->folder_count = p->mem_folders(p);
    } else {
    DIR *dir = opendir(p->root);
    p->folder_count = 0;
    if (dir) {
        int n = 0;
        int scanned = 0;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && n < 24 && ++scanned <= PANE_SCAN_MAX) {
            if (e->d_name[0] == '.') continue;
            if (e->d_type != DT_DIR) continue;
            /* 排除数据目录 (如文曲星 Lava), 不显示为分类 */
            if (p->exclude_dir && p->exclude_dir(e->d_name)) continue;
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
    }

    p->settings_mode = false;
    p->fav_mode = false;
    p->recent_mode = false;
    /* 默认选中: 有收藏栏则默认收藏栏, 其次 设置后的 全部/首个文件夹 */
    p->sel_folder = (p->settings_label ? 1 : 0) + (p->recent_label ? 1 : 0) + (p->fav_enabled ? 1 : 0);
    if (p->sel_folder >= pane_total(p)) p->sel_folder = 0;
    p->focus = 0;
    p->sel_item = 0;
    p->item_off = 0;
    p->folder_scroll = 0;
    p->item_scroll = 0;
    p->built_folder = -1;   /* 强制首次 build */
    p->loaded = true;
    ESP_LOGI(TAG, "reset: %s folders=%d hide_all=%d sel=%d 耗时%luus",
             p->root ? p->root : "/",
             p->folder_count, (int)p->hide_all, p->sel_folder,
             (unsigned long)(esp_timer_get_time() - t0));
}

void os_pane_build_right(os_pane_t *p) {
    /* 同一分类反复点击/确认不重扫 (触摸左栏每次点击都会调用) */
    if (p->built_folder == p->sel_folder) return;
    p->built_folder = p->sel_folder;
    uint32_t t0 = esp_timer_get_time();
    /* 内存数据源: 由回调填充 items, 不走文件扫描 */
    if (p->mem_items) {
        p->settings_mode = false;
        p->fav_mode = false;
        p->recent_mode = false;
        p->item_off = 0;
        p->item_count = p->mem_items(p);
        if (p->sel_item >= p->item_count) p->sel_item = 0;
        if (p->item_scroll > p->sel_item) p->item_scroll = p->sel_item;
        return;
    }
    const char *dir = pane_folder_of(p, p->sel_folder);
    /* 收藏栏: 右栏列出该引擎的收藏 */
    if (p->fav_enabled && dir == (const char *)(uintptr_t)&p->fav_engine) {
        p->fav_mode = true;
        p->settings_mode = false;   /* 关键: 从"引擎优化/设置"切到收藏栏必须退出设置态, 否则右栏仍渲染设置 */
        p->recent_mode = false;
        p->item_count = 0;
        p->item_off = 0;
        int cnt = 0;
        const char *const *favs = favorites_list((fav_engine_t)p->fav_engine, &cnt);
        int n = 0;
        for (int i = 0; i < cnt && n < OS_PANE_ITEM_MAX; i++) {
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
    /* 最近阅读: 右栏显示最近打开的书 (设备级兜底, NVS 记录) */
    if (p->recent_label && dir == (const char *)(uintptr_t)&p->recent_label) {
        p->recent_mode = true;
        p->settings_mode = false;
        p->fav_mode = false;
        p->item_count = 0;
        p->item_off = 0;
        if (p->recent_path[0]) {
            char tmp[160];
            snprintf(tmp, sizeof(tmp), "%.63s", p->recent_path);
            char *base = strrchr(tmp, '/');
            if (base) memmove(tmp, base + 1, strlen(base));
            strip_ext(tmp);
            snprintf(p->items[0], sizeof(p->items[0]), "%.63s", tmp);
            snprintf(p->item_paths[0], sizeof(p->item_paths[0]), "%s", p->recent_path);
            p->item_count = 1;
        }
        if (p->sel_item >= p->item_count) p->sel_item = 0;
        if (p->item_scroll > p->sel_item) p->item_scroll = p->sel_item;
        return;
    }
    if (!dir) {
        /* 引擎优化/设置: 右栏显示设置项 */
        p->settings_mode = true;
        p->fav_mode = false;
        p->recent_mode = false;
        p->item_count = 0;
        p->item_off = 0;
        if (p->sel_item >= p->settings_count) p->sel_item = 0;
        if (p->item_scroll > p->sel_item) p->item_scroll = p->sel_item;
        return;
    }
    p->settings_mode = false;
    p->fav_mode = false;
    p->recent_mode = false;
    char path[128];
    if (dir[0]) snprintf(path, sizeof(path), "%s/%s", p->root, dir);
    else        snprintf(path, sizeof(path), "%s", p->root);
    DIR *d = opendir(path);
    p->item_count = 0;
    if (d) {
        struct dirent *e;
        int n = 0;
        int scanned = 0;
        while ((e = readdir(d)) != NULL && n < OS_PANE_ITEM_MAX && ++scanned <= PANE_SCAN_MAX) {
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
    p->item_off = 0;
    ESP_LOGI(TAG, "build_right: %s items=%d 耗时%luus", path, p->item_count,
             (unsigned long)(esp_timer_get_time() - t0));
}

/* 90° 外凸圆弧弯头 (水管拐弯, 选中的圆角效果, 同软件管家): 拐点 (cx,cy), 半径 r.
 * kind 1=左上外凸, 2=左下外凸, 3=右上外凸, 4=右下外凸; 保留到圆心距离 d∈[r-2,r] 的环带像素. */
static void pane_elbow(st7305_handle_t *lcd, int cx, int cy, int r, int kind, st7305_color_t color) {
    int ox, oy, x0, x1, y0, y1, sx, sy;
    if (kind == 1)      { ox = cx + r; oy = cy + r; x0 = cx;    x1 = cx + r; y0 = cy;    y1 = cy + r; sx = -1; sy = -1; }
    else if (kind == 2) { ox = cx + r; oy = cy - r; x0 = cx;    x1 = cx + r; y0 = cy - r; y1 = cy;    sx = -1; sy =  1; }
    else if (kind == 3) { ox = cx - r; oy = cy + r; x0 = cx - r; x1 = cx;    y0 = cy;    y1 = cy + r; sx =  1; sy = -1; }
    else                { ox = cx - r; oy = cy - r; x0 = cx - r; x1 = cx;    y0 = cy - r; y1 = cy;    sx =  1; sy =  1; }
    for (int yy = y0; yy <= y1; yy++) {
        if (yy < 0 || yy >= ui_screen_h()) continue;
        for (int xx = x0; xx <= x1; xx++) {
            if (xx < 0 || xx >= ui_screen_w()) continue;
            float dx = (float)xx - ox, dy = (float)yy - oy;
            if (!((sx * dx >= 0) && (sy * dy >= 0))) continue;
            float d = sqrtf(dx * dx + dy * dy);
            /* 圆角环带 [r-1,r]=2px, 与水管线等宽: 保留外凸圆弧, 但不再在角外侧拖出"直走的线头" */
            if (d >= (r - 1) && d <= r) st7305_draw_pixel(lcd, xx, yy, color);
        }
    }
}

/* 复用左栏模板: 文字 + 水管分割线 + 选中框 (阅读/游戏/密码机等自包含渲染页可调用, 保证左栏样式统一) */
void os_pane_draw_left(st7305_handle_t *lcd, os_pane_t *p) {
    if (!lcd || !p) return;
    const int list_y = p->list_y > 0 ? p->list_y : LIST_Y;
    const int lx0 = 2;
    const int lx1 = lx0 + pane_left_w(p) - 4;
    const int lw = pane_left_w(p);
    const int R = 3, w1 = 1;
    const int sh = (p->right_gap > 0) ? p->right_gap : 3;
    const int sx = lx1 + sh, rx = lx1 + sh + 1;

    int total = pane_total(p);
    int max_vis = (pane_bottom(p) - list_y) / ROW_H;
    if (max_vis < 1) max_vis = 1;
    if (p->sel_folder < p->folder_scroll) p->folder_scroll = p->sel_folder;
    if (p->sel_folder >= p->folder_scroll + max_vis) p->folder_scroll = p->sel_folder - max_vis + 1;
    int sel_top = -1, sel_bot = -1;
    int y = list_y;
    for (int i = 0; i < max_vis && (p->folder_scroll + i) < total; i++) {
        int idx = p->folder_scroll + i;
        char short_name[64];
        pane_left_name(p, idx, short_name, sizeof(short_name));
        int tw = text_width(short_name);
        int tx = lx0 + (lw - tw) / 2;
        if (tx < lx0 + 2) tx = lx0 + 2;
        draw_text(lcd, tx, y + 4, short_name, false);   /* 24px 字在 32px 行垂直居中 */
        if (idx == p->sel_folder) { sel_top = y; sel_bot = y + ROW_H - 1; }   /* 当前分类始终显示选中框 (与焦点无关) */
        y += ROW_H;
    }

    /* ==== 一根特色水管分割线 (同软件管家): 右缘贯穿竖线, 选中行令让位改走包围框 ====
     * 选中段分割线上/下各让 R, 上/下段仍贯穿; 选中行由「左竖+上横+下横+四角 R3 外凸弯头」
     * 围成框; 上端顶状态栏下方, 下端直线插到底部. */
    int sy0 = list_y - GAP; if (sy0 < 0) sy0 = 0;
    int sy1 = ui_screen_h() - 4;
    if (sel_top < 0) {
        draw_vline(lcd, sx, sy0, sy1, ST7305_COLOR_BLACK);
        draw_vline(lcd, rx, sy0, sy1, ST7305_COLOR_BLACK);
    } else {
        const int ty = sel_top, by = sel_bot;
        if (ty - R > sy0) {
            draw_vline(lcd, sx, sy0, ty - R - 1, ST7305_COLOR_BLACK);
            draw_vline(lcd, rx, sy0, ty - R - 1, ST7305_COLOR_BLACK);
        }
        if (by + w1 + R < sy1) {
            draw_vline(lcd, sx, by + w1 + R + 1, sy1, ST7305_COLOR_BLACK);
            draw_vline(lcd, rx, by + w1 + R + 1, sy1, ST7305_COLOR_BLACK);
        }
        /* 选中包围框: 左竖(上下让R给弯头) + 上横 + 下横 (2px) */
        draw_vline(lcd, lx0,     ty + R, by - R, ST7305_COLOR_BLACK);
        draw_vline(lcd, lx0 + 1, ty + R, by - R, ST7305_COLOR_BLACK);
        draw_hline(lcd, lx0 + w1 + R, rx - R, ty,     ST7305_COLOR_BLACK);
        draw_hline(lcd, lx0 + w1 + R, rx - R, ty + 1, ST7305_COLOR_BLACK);
        draw_hline(lcd, lx0 + w1 + R, rx - R, by,     ST7305_COLOR_BLACK);
        draw_hline(lcd, lx0 + w1 + R, rx - R, by - 1, ST7305_COLOR_BLACK);
        pane_elbow(lcd, lx0, ty, R, 1, ST7305_COLOR_BLACK);
        pane_elbow(lcd, rx,  ty, R, 4, ST7305_COLOR_BLACK);
        pane_elbow(lcd, lx0, by, R, 2, ST7305_COLOR_BLACK);
        pane_elbow(lcd, rx,  by, R, 3, ST7305_COLOR_BLACK);
    }
}

void os_pane_render(ui_ctx_t *ctx, os_pane_t *p) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    const int list_y = p->list_y > 0 ? p->list_y : LIST_Y;
    const int sh = (p->right_gap > 0) ? p->right_gap : 3;
    int lx1 = 2 + pane_left_w(p) - 4;
    int rx0 = lx1 + sh + GAP + 4;   /* 右栏起点 */

    /* 左栏 (文字 + 水管分割线 + 选中框) 复用同一套模板: 阅读/游戏/密码机左栏样式一致 */
    os_pane_draw_left(lcd, p);

    /* 右栏: 设置模式走调用方自定义渲染 (内联设置项, 含平滑跟手滚动) */
    if (p->settings_mode) {
        int view_h2 = pane_bottom(p) - list_y;
        int off2 = p->item_off;
        if (off2 < 0) off2 = 0;
        int top2 = off2 / ROW_H;
        int mod2 = off2 % ROW_H;
        if (p->settings_render)
            p->settings_render(ctx, p, rx0, RIGHT_EDGE,
                               list_y, pane_bottom(p), top2, mod2);
        return;
    }

    /* 右栏: 游戏/内容, 选中整行反色. 平滑跟手滚动由 item_off(像素) 驱动 */
    int view_h = pane_bottom(p) - list_y;
    int max_vis = view_h / ROW_H;
    if (max_vis < 1) max_vis = 1;
    int off = p->item_off;
    if (off < 0) off = 0;
    int top_row = off / ROW_H;
    int off_mod = off % ROW_H;
    int n_vis = (p->item_count - top_row) > (max_vis + 1) ? (max_vis + 1) : (p->item_count - top_row);
    for (int i = 0; i < n_vis; i++) {
        int idx = top_row + i;
        bool sel = (p->focus == 1 && idx == p->sel_item);
        int py = list_y + i * ROW_H - off_mod;   /* 平滑纵坐标, 跟随手指 */
        if (sel && py + ROW_H - 2 >= list_y && py < pane_bottom(p)) {
            int fy = py < list_y ? list_y : py;
            int ty = py + ROW_H - 2; if (ty > pane_bottom(p) - 1) ty = pane_bottom(p) - 1;
            fill_rect(lcd, rx0, fy, RIGHT_EDGE, ty, ST7305_COLOR_BLACK);
        }
        if (py + ROW_H < list_y || py >= pane_bottom(p)) continue;
        int tw = text_width(p->items[idx]);
        int tx = rx0 + ((RIGHT_EDGE - rx0) - tw) / 2;   /* 右栏水平居中 */
        if (tx < rx0) tx = rx0;
        draw_text(lcd, tx, py + 4, p->items[idx], sel);   /* 24px 字在 32px 行垂直居中 */
    }
    /* 收藏栏为空提示: 右栏内居中 (不越过左/右分割线), 文案=长按确认游戏加入收藏 */
    if (p->fav_mode && p->item_count == 0) {
        const char *tip = p->fav_empty_tip ? p->fav_empty_tip :
            "\xe9\x95\xbf\xe6\x8c\x89\xe7\xa1\xae\xe8\xae\xa4\xe6\xb8\xb8\xe6\x88\x8f\xe5\x8a\xa0\xe5\x85\xa5\xe6\x94\xb6\xe8\x97\x8f"; /* 长按确认游戏加入收藏 */
        int tyy = list_y + view_h / 2 - 12;
        int tw = text_width(tip);
        int tx = rx0 + ((RIGHT_EDGE - rx0) - tw) / 2;
        if (tx < rx0) tx = rx0;
        draw_text(lcd, tx, tyy, tip, false);
    }
    /* 最近阅读为空提示: 右栏内居中 */
    if (p->recent_mode && p->item_count == 0) {
        const char *tip = "\xe6\x9c\x80\xe8\xbf\x91\xe6\x97\xa0\xe9\x98\x85\xe8\xaf\xbb\xe8\xae\xb0\xe5\xbd\x95"; /* 最近无阅读记录 */
        int tyy = list_y + view_h / 2 - 12;
        int tw = text_width(tip);
        int tx = rx0 + ((RIGHT_EDGE - rx0) - tw) / 2;
        if (tx < rx0) tx = rx0;
        draw_text(lcd, tx, tyy, tip, false);
    }
}

/* 右栏可视高度 (设置/内容共用) */
static int pane_view_h(const os_pane_t *p) {
    int list_y = p->list_y > 0 ? p->list_y : LIST_Y;
    return pane_bottom(p) - list_y;
}
/* 右栏最大平滑滚动偏移 (设置区与内容区共用, 拖动/按键统一夹紧) */
static int pane_max_off(const os_pane_t *p) {
    int total = p->settings_mode ? p->settings_count : p->item_count;
    int mo = total * ROW_H - pane_view_h(p);
    return mo < 0 ? 0 : mo;
}

/* 按键导航的"光标跟随": 让选中黑框在可视窗内跟随方向键移动(框动、列表不动),
 * 仅当选中项越出可视窗边界时才滚动列表一位.
 * 可视窗内部滚动保持行对齐(item_off 始终是行的整数倍), 末行仍夹紧到底. */
static void pane_follow_selection(os_pane_t *p) {
    int first = p->item_off / ROW_H;
    int max_vis = pane_view_h(p) / ROW_H;
    if (max_vis < 1) max_vis = 1;
    if (p->sel_item < first) first = p->sel_item;
    else if (p->sel_item >= first + max_vis) first = p->sel_item - max_vis + 1;
    if (first < 0) first = 0;
    p->item_off = first * ROW_H;
    int mo = pane_max_off(p);
    if (p->item_off > mo) p->item_off = mo;
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
        if (p->focus == 0) {
            if (p->sel_folder > 0) {
                p->sel_folder--;
                os_pane_build_right(p);   /* 切左栏分类 → 立即刷新右栏, 不必按确定(与触摸一致) */
            }
        } else               { if (p->sel_item > 0) p->sel_item--; }
        /* 光标跟随: 框在可视窗内移动(按键/触屏都顺手), 越界才滚动 */
        if (p->focus == 1) pane_follow_selection(p);
        ctx->needs_redraw = true;
        return true;
    case OS_ACTION_DOWN:
        if (p->focus == 0) {
            if (p->sel_folder < total - 1) {
                p->sel_folder++;
                os_pane_build_right(p);   /* 切左栏分类 → 立即刷新右栏, 不必按确定(与触摸一致) */
            }
        } else {
            int mx = p->settings_mode ? (p->settings_count - 1) : (p->item_count - 1);
            if (p->sel_item < mx) p->sel_item++;
        }
        /* 光标跟随: 框在可视窗内移动, 越界才滚动 */
        if (p->focus == 1) pane_follow_selection(p);
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
                    /* 已在收藏中→视为成功(显示已收藏, 不再误报"收藏失败"):
                     * 常见于在"全部/分类"视图对已收藏的游戏再次长按. */
                    fav_engine_t eng = favorites_engine_for_path(path);
                    if (favorites_contains(eng, path)) {
                        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe6\x94\xb6\xe8\x97\x8f");   /* 已收藏 */
                    } else {
                        bool added = favorites_add(favorites_engine_for_path(path), path);
                        os_dialog_toast(ctx, added ? "\xe5\xb7\xb2\xe6\x94\xb6\xe8\x97\x8f"   /* 已收藏 */
                                                    : "\xe6\x94\xb6\xe8\x97\x8f\xe5\xa4\xb1\xe8\xb4\xa5"); /* 收藏失败 */
                    }
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
    /* V1.5.x: 滚动保护 — 本手势滚动过 (浏览列表), 松手被误判为点击的 tap 直接吞掉,
     * 不打开游戏/不触发操作. 标志由 pane_poll 置位/新手势清零, 此处消费一次. */
    if (s_scroll_suppress_tap) { s_scroll_suppress_tap = false; return true; }
    const int list_y = p->list_y > 0 ? p->list_y : LIST_Y;
    if (y < list_y || y >= pane_bottom(p)) return false;
    /* 左栏: 切目录/设置 */
    if (x < pane_left_w(p)) {
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
    /* 右栏: 设置项或内容项 (均按平滑滚动偏移映射点击项) */
    if (p->settings_mode) {
        int idx = (p->item_off + (y - list_y)) / ROW_H;
        if (idx >= p->settings_count) return false;
        p->sel_item = idx;
        p->focus = 1;
        if (ctx) ctx->needs_redraw = true;
        if (p->on_settings_select) p->on_settings_select(ctx, p, idx);
        return true;
    }
    int idx = (p->item_off + (y - list_y)) / ROW_H;   /* 按平滑滚动偏移映射点击项 */
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
 * 平滑跟手: 记录按下时的起始 y 与 item_off, 手指每移动 1px 内容跟随 1px.
 * 输入层已区分: 大位移拖动手势不投递 tap, 不会误触发选中/启动. */
void os_pane_poll(ui_ctx_t *ctx, os_pane_t *p) {
    int tx, ty;
    static int s_last_y = -1;    /* 当前按下时的起始 y */
    static int s_start_off = 0;  /* 起始 item_off */
    static bool s_started_right = false;
    const int list_y = p->list_y > 0 ? p->list_y : LIST_Y;
    bool down = input_get_touch_pos(&tx, &ty);
    if (down) {
        /* 页面级触摸映射 (书架竖屏/180°): 拖动坐标同样换算到 UI 逻辑坐标系 */
        if (ctx && ctx->map_touch) ctx->map_touch(&tx, &ty);
        /* 按住右栏内容: 立即把手指下方条目设为选中并聚焦(黑底反色), 
         * 这样"按住即高亮", 长按收藏也定位到它 (无需先点选/无需松手). */
        if (!p->settings_mode && tx >= pane_left_w(p) && ty >= list_y && ty < pane_bottom(p)) {
            int hidx = (p->item_off + (ty - list_y)) / ROW_H;
            if (hidx >= 0 && hidx < p->item_count)
                if (p->sel_item != hidx || p->focus != 1) {
                    p->sel_item = hidx;
                    p->focus = 1;
                    if (ctx) ctx->needs_redraw = true;
                }
        }
        if (s_last_y < 0) {
            /* 记录起始: 按下点在右栏(设置区或内容区)则启用跟手滚动, 否则本次不滚 */
            bool in_right = (tx >= pane_left_w(p) && ty >= list_y && ty < pane_bottom(p));
            s_started_right = in_right;
            s_last_y = ty;
            s_start_off = p->item_off;
            s_scroll_suppress_tap = false;   /* 新手势起始: 作废旧滚动标志 (防误吞下次点击) */
            return;
        }
        if (!s_started_right) return;   /* 起点在左栏 → 不跟手滚动 */
        int max_off = pane_max_off(p);
        if (max_off < 0) max_off = 0;
        int new_off = s_start_off - (ty - s_last_y);   /* 1:1 跟手 */
        if (new_off < 0) new_off = 0;
        if (new_off > max_off) new_off = max_off;
        if (new_off != p->item_off) {
            p->item_off = new_off;
            p->item_scroll = (max_off > 0) ? (new_off / ROW_H) : 0;
            s_scroll_suppress_tap = true;   /* 本手势滚动过 → 松手 tap 无效 (防误开游戏) */
            if (ctx) ctx->needs_redraw = true;
        }
    } else {
        s_last_y = -1;
        s_start_off = 0;
        s_started_right = false;
    }
}
