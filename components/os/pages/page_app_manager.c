/**
 * page_app_manager.c — 应用管家 页面模块 (应用管理 + 应用商店 合并, 3.3 网格风格).
 *
 * 左侧分类栏: 引擎 / 游戏 / 应用 / 商店 (4 类, 仅图标).
 * 右侧内容:
 *   - 引擎/游戏/应用: 已安装应用网格/列表; 顶部子分类条(游戏/程序/网络/运维/手册/学习)筛选;
 *     选中应用 → 操作弹窗: 打开 / 添加到主菜单 / 移出主菜单 / 卸载
 *   - 商店: 顶部子分类条 + 可下载应用 (内置清单 + bbk.linit.cn 拉取), 下载/安装/卸载
 * 右上角 24×24 小图标: 切换 网格视图 / 单条列表视图 (商店"横排"同列表).
 * 主菜单显隐由 os_app 注册表 + NVS 持久化 (os_app_set_mainmenu / os_app_set_hidden).
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "os_theme.h"
#include "ui_common.h"
#include "input.h"
#include "touch_panel.h"
#include "wifi_manager.h"
#include "font_zh.h"
#include "font_zh16.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "APPM"

/* ==== 布局 (复刻 3.3 应用管理页) ==== */
#define AM_CATBAR_W    68     /* 左侧分类栏宽度 */
#define AM_CAT_Y0      24     /* 分类栏起始 y = 状态栏边界 */
#define AM_ICON_SIZE   64     /* 网格图标尺寸 */
#define AM_GRID_COLS   4      /* 网格列数 */
#define AM_GRID_ROWS   3      /* 网格可见行数 */
#define AM_SUB_H       26     /* 顶部子分类条高 */
#define AM_VIEW_ICON   24     /* 视图切换小图标 24×24 */

/* 左侧 4 分类 (引擎/游戏/应用/商店) */
#define AM_CAT_ENGINE  0   /* 引擎 */
#define AM_CAT_GAME    1   /* 游戏 */
#define AM_CAT_APP     2   /* 应用 */
#define AM_CAT_STORE   3   /* 商店 */
#define AM_CAT_COUNT   4

/* 商店/应用 子分类 (横排条) */
#define AM_SUB_ALL     0
#define AM_SUB_GAME    1   /* 游戏 */
#define AM_SUB_PROG    2   /* 程序 */
#define AM_SUB_NET     3   /* 网络 */
#define AM_SUB_OPS     4   /* 运维 */
#define AM_SUB_MANUAL  5   /* 手册 */
#define AM_SUB_STUDY   6   /* 学习 */
#define AM_SUB_COUNT   7
static const char *const s_sub_names[AM_SUB_COUNT] = {
    "\xe5\x85\xa8\xe9\x83\xa8",  /* 全部 */
    "\xe6\xb8\xb8\xe6\x88\x8f",  /* 游戏 */
    "\xe7\xa8\x8b\xe5\xba\x8f",  /* 程序 */
    "\xe7\xbd\x91\xe7\xbb\x9c",  /* 网络 */
    "\xe8\xbf\x90\xe7\xbb\xb4",  /* 运维 */
    "\xe6\x89\x8b\xe5\x86\x8c",  /* 手册 */
    "\xe5\xad\xa6\xe4\xb9\xa0",  /* 学习 */
};

/* ==== 商店可下载应用 (内置清单 + 服务器) ==== */
#define STORE_MAX 24
typedef struct {
    char id[24];
    char name[24];
    char brief[48];
    int  icon_idx;
    int  cat;            /* 子分类索引 (AM_SUB_*) */
} store_app_t;
static store_app_t s_catalog[STORE_MAX];
static int  s_catalog_n = 0;
static bool s_store_installed[STORE_MAX];
#define STORE_NS "os_store"
#define STORE_MANIFEST "https://bbk.linit.cn/manifest.json"

/* 内置示例商店清单 */
static const store_app_t s_builtin_catalog[] = {
    { "snake",  "\xe8\xb4\xaa\xe5\x90\x83\xe8\x9b\x87",      "\xe7\xbb\x8f\xe5\x85\xb8\xe8\xb4\xaa\xe5\x90\x83\xe8\x9b\x87", -1, AM_SUB_GAME },
    { "tetris", "\xe4\xbf\x84\xe7\xbd\x97\xe6\x96\xaf\xe6\x96\xb9\xe5\x9d\x97", "\xe7\xbb\x8f\xe5\x85\xb8\xe4\xbf\x84\xe7\xbd\x97\xe6\x96\xaf\xe6\x96\xb9\xe5\x9d\x97", -1, AM_SUB_GAME },
    { "calc",   "\xe8\xae\xa1\xe7\xae\x97\xe5\x99\xa8",      "\xe7\xae\x80\xe6\x98\x93\xe5\x9b\x9b\xe5\x88\x99\xe8\xbf\x90\xe7\xae\x97", -1, AM_SUB_PROG },
    { "notepad","\xe8\xae\xb0\xe4\xba\x8b\xe6\x9c\xac",      "\xe7\xae\x80\xe6\x98\x93\xe8\xae\xb0\xe4\xba\x8b/\xe5\xa4\x87\xe5\xbf\x98", -1, AM_SUB_PROG },
    { "clock",  "\xe5\x8d\x93\xe9\x9d\xa2\xe6\x97\xb6\xe9\x92\x9f", "\xe5\xa4\xa7\xe5\xad\x97\xe6\x97\xb6\xe9\x92\x9f/\xe7\xa7\x92\xe8\xa1\xa8", -1, AM_SUB_PROG },
};
#define BUILTIN_COUNT (int)(sizeof(s_builtin_catalog)/sizeof(s_builtin_catalog[0]))

/* ==== 页面状态 ==== */
static int s_cat = AM_CAT_ENGINE;     /* 左栏分类 */
static int s_sub = AM_SUB_ALL;        /* 顶部子分类 */
static bool s_view_grid = true;       /* true=网格  false=单条列表 */
static int  s_focus = 0;              /* 右栏选中索引 */
static int  s_scroll = 0;             /* 上卷行数 */
static int s_list_n = 0;             /* 当前显示条目数 */
static const os_app_t *s_list[32];    /* 已安装条目 (应用分类) */
static bool s_list_is_store = false;  /* 当前是否为商店条目 */

/* 触摸拖动滚动状态 (列表整行滚动) */
#define AM_DRAG_THRESH 12   /* 超过此位移判定为拖动 (不触发点击) */

/* ==== 网格相机 (仿主菜单: 跟手 + 平滑吸附) ====
 * s_gcam 以"行"为单位, 可小数; 拖动时跟手, 松手后平滑吸附到最近焦点行. */
static float s_gcam = 0.0f;            /* 当前相机 (行, 可小数) */
static float s_gcam_base = 0.0f;       /* 拖动起点相机 */
static float s_gcam_anim_from = 0.0f;
static float s_gcam_anim_to   = 0.0f;
static uint32_t s_gcam_anim_start = 0;
static uint32_t s_gcam_anim_dur   = 0;
static int   s_gcam_anim_sel = -1;     /* 动画锁定选中行 */
static int   s_gdrag_anchor_y = 0;     /* 拖动起始触摸 y */
static int   s_gdrag_last_y = -1;
static float s_gdrag_base = 0.0f;      /* 拖动起始相机 */
static bool  s_gdrag_active = false;

/* 前向声明 (定义在下方, 交叉引用) */
static void am_clamp_scroll(void);
static int  am_catalog_index(int i);
static void store_nvs_save(void);
static void am_rebuild(void);

/* 子分类是否匹配应用 */
static bool app_match_sub(const os_app_t *a) {
    if (!a || !a->cat) return (s_sub == AM_SUB_ALL);
    if (s_sub == AM_SUB_ALL) return true;
    const char *c = a->cat;
    if (s_sub == AM_SUB_GAME   && strcmp(c, "\xe6\xb8\xb8\xe6\x88\x8f") == 0) return true;
    if (s_sub == AM_SUB_PROG   && strcmp(c, "\xe7\xa8\x8b\xe5\xba\x8f") == 0) return true;
    if (s_sub == AM_SUB_NET    && strcmp(c, "\xe7\xbd\x91\xe7\xbb\x9c") == 0) return true;
    if (s_sub == AM_SUB_OPS    && strcmp(c, "\xe8\xbf\x90\xe7\xbb\xb4") == 0) return true;
    if (s_sub == AM_SUB_MANUAL && strcmp(c, "\xe6\x89\x8b\xe5\x86\x8c") == 0) return true;
    if (s_sub == AM_SUB_STUDY  && strcmp(c, "\xe5\xad\xa6\xe4\xb9\xa0") == 0) return true;
    return false;
}
/* 左侧分类是否匹配应用 (引擎/游戏/应用) */
/* 引擎分类固定顺序 (用户指定): 步步高/文曲星/暴龙机/ArduBoy/GB/GBC/NES */
static const char *const s_engine_order[] = {
    "bbk", "lavax", "vpet", "arduboy", "gb", "gbc", "nes"
};
#define ENGINE_ORDER_COUNT (int)(sizeof(s_engine_order)/sizeof(s_engine_order[0]))

static bool app_match_cat(const os_app_t *a) {
    if (!a) return false;
    const char *id = a->id;
    switch (s_cat) {
    case AM_CAT_ENGINE: {
        /* 引擎: 只认固定顺序表里的模拟器 */
        for (int i = 0; i < ENGINE_ORDER_COUNT; i++)
            if (strcmp(id, s_engine_order[i]) == 0) return true;
        return false;
    }
    case AM_CAT_GAME:
        /* 游戏分类: 暂时空白 */
        return false;
    case AM_CAT_APP: {
        /* 应用: 排除引擎, 按子分类筛选 */
        for (int i = 0; i < ENGINE_ORDER_COUNT; i++)
            if (strcmp(id, s_engine_order[i]) == 0) return false;
        return app_match_sub(a);
    }
    }
    return false;
}

/* 重建右侧列表 */
static void am_rebuild(void) {
    s_list_is_store = (s_cat == AM_CAT_STORE);
    s_list_n = 0;
    if (s_cat == AM_CAT_STORE) {
        /* 商店: 按子分类筛选 s_catalog */
        for (int i = 0; i < s_catalog_n && s_list_n < 32; i++) {
            if (s_sub != AM_SUB_ALL && s_catalog[i].cat != s_sub) continue;
            s_list[s_list_n++] = NULL;   /* 商店条目用 NULL 占位, 具体数据在渲染/操作时按索引取 catalog */
        }
    } else if (s_cat == AM_CAT_ENGINE) {
        /* 引擎: 按用户指定顺序插入 (从注册表取对应应用) */
        for (int o = 0; o < ENGINE_ORDER_COUNT && s_list_n < 32; o++) {
            int total = os_app_count();
            for (int i = 0; i < total; i++) {
                const os_app_t *a = os_app_get(i);
                if (a && strcmp(a->id, s_engine_order[o]) == 0) {
                    s_list[s_list_n++] = a;
                    break;
                }
            }
        }
    } else {
        int total = os_app_count();
        for (int i = 0; i < total && s_list_n < 32; i++) {
            const os_app_t *a = os_app_get(i);
            if (app_match_cat(a)) s_list[s_list_n++] = a;
        }
    }
    if (s_focus >= s_list_n) s_focus = 0;
    if (s_focus < 0) s_focus = 0;
    /* 网格: 相机吸附到焦点行 */
    s_gcam = (float)(s_focus / AM_GRID_COLS);
    s_gcam_anim_start = 0;
    s_scroll = s_focus / AM_GRID_COLS;
    am_clamp_scroll();
}
static int am_max_scroll(void) {
    if (!s_view_grid) {
        int content_top = (s_cat == AM_CAT_APP || s_cat == AM_CAT_STORE) ? (AM_CAT_Y0 + AM_SUB_H) : AM_CAT_Y0;
        int mv = (UI_SCREEN_H - content_top) / 46;
        int m = s_list_n - mv;
        return m < 0 ? 0 : m;
    }
    int rows = (s_list_n + AM_GRID_COLS - 1) / AM_GRID_COLS;
    int vis  = AM_GRID_ROWS;
    int m = rows - vis;
    return m < 0 ? 0 : m;
}
static void am_clamp_scroll(void) {
    if (s_list_n <= 0) { s_scroll = 0; return; }
    int row = s_focus / AM_GRID_COLS;
    if (row < s_scroll) s_scroll = row;
    else if (row >= s_scroll + AM_GRID_ROWS)
        s_scroll = row - (AM_GRID_ROWS - 1);
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > am_max_scroll()) s_scroll = am_max_scroll();
}

/* ==== 网格相机 (仿主菜单) ==== */
static uint32_t am_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 当前相机行的边界: 目标行取与 from 最近的同余 (0..max_row) */
static float am_gcam_clamp(float c) {
    int max_row = am_max_scroll();
    if (c < 0) c = 0;
    if (c > max_row) c = (float)max_row;
    return c;
}
/* 相机目标行 → 焦点 (网格: 保持列; 列表: 直接=行) */
static void am_gcam_set_focus_row(int row) {
    if (s_view_grid)
        s_focus = row * AM_GRID_COLS + (s_focus % AM_GRID_COLS);
    else
        s_focus = row;
}
/* 网格吸附动画: 从当前相机平滑缓动到目标行 (无弹簧过冲), 同步 s_focus */
static void am_gcam_animate(int to_row, uint32_t dur_ms) {
    int total_rows = am_max_scroll();
    if (to_row > total_rows) to_row = total_rows;
    if (to_row < 0) to_row = 0;
    float from_c = am_gcam_clamp(s_gcam);
    float to_c = (float)to_row;
    if (from_c == to_c) {
        s_gcam = to_c;
        am_gcam_set_focus_row(to_row);
        s_gcam_anim_start = 0;
        return;
    }
    s_gcam_anim_from   = from_c;
    s_gcam_anim_to     = to_c;
    s_gcam_anim_start  = am_now_ms();
    s_gcam_anim_dur    = dur_ms ? dur_ms : 1;
    s_gcam_anim_sel    = to_row;
    am_gcam_set_focus_row(to_row);
}
/* 吸附完成: 把相机锁定到最近焦点行 (动画结束后调用) */
static void am_gcam_settle(void) {
    s_gcam = am_gcam_clamp(s_gcam);
    int row = (int)(s_gcam + 0.5f);
    am_gcam_set_focus_row(row);
    s_gcam = (float)row;
    s_gcam_anim_start = 0;
}
/* 每帧更新动画 (poll 调用), 返回是否还在动画中 */
static bool am_gcam_tick(void) {
    if (s_gcam_anim_start == 0) return false;
    uint32_t d = am_now_ms() - s_gcam_anim_start;
    float tt = d < s_gcam_anim_dur ? (float)d / (float)s_gcam_anim_dur : 1.0f;
    /* 平滑缓动 (抛物线减速), 无弹簧过冲 */
    float ease = 1.0f - (float)pow(1.0f - tt, 2.0f);
    s_gcam = s_gcam_anim_from + (s_gcam_anim_to - s_gcam_anim_from) * ease;
    if (tt >= 1.0f) {
        am_gcam_settle();
    }
    return s_gcam_anim_start != 0;
}

/* ==== 16px 文本 (名称/标签) ==== */
static int am_label_width(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) { w += ZH16_FONT_W; p += 3; }
        else { w += 8; p++; }
    }
    return w;
}
static void am_draw_label(st7305_handle_t *lcd, int cx, int y, const char *s, bool inverted) {
    st7305_color_t fg = inverted ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    int aofs = (ZH16_FONT_H - 12) / 2;
    int x = cx - am_label_width(s) / 2;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) {
            int idx = font_zh16_find_utf8((const char *)p);
            const uint8_t *b = (idx >= 0) ? font_zh16_get_bitmap_by_index(idx) : NULL;
            if (b)
                for (int r = 0; r < ZH16_FONT_H; r++)
                    for (int c = 0; c < ZH16_FONT_W; c++)
                        if (b[r * 2 + (c >> 3)] & (0x80 >> (c & 7)))
                            st7305_draw_pixel(lcd, x + c, y + r, fg);
            x += ZH16_FONT_W; p += 3;
        } else {
            if (*p >= 0x20 && *p <= 0x7e) {
                const uint8_t *b = FONT8X12[*p - 0x20];
                for (int cy = 0; cy < 12; cy++)
                    for (int cc = 0; cc < 8; cc++)
                        if (b[cy] & (0x80u >> cc)) st7305_draw_pixel(lcd, x + cc, y + aofs + cy, fg);
            }
            x += 8; p++;
        }
    }
}

/* 16px 文本左对齐绘制 (不居中, x 为起点; 用于列表视图说明等) */
static void am_draw_text_l(st7305_handle_t *lcd, int x, int y, const char *s, bool inv) {
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    int aofs = (ZH16_FONT_H - 12) / 2;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if ((*p & 0xe0) == 0xe0) {
            int idx = font_zh16_find_utf8((const char *)p);
            const uint8_t *b = (idx >= 0) ? font_zh16_get_bitmap_by_index(idx) : NULL;
            if (b)
                for (int r = 0; r < ZH16_FONT_H; r++)
                    for (int c = 0; c < ZH16_FONT_W; c++)
                        if (b[r * 2 + (c >> 3)] & (0x80 >> (c & 7)))
                            st7305_draw_pixel(lcd, x + c, y + r, fg);
            x += ZH16_FONT_W; p += 3;
        } else {
            if (*p >= 0x20 && *p <= 0x7e) {
                const uint8_t *b = FONT8X12[*p - 0x20];
                for (int cy = 0; cy < 12; cy++)
                    for (int cc = 0; cc < 8; cc++)
                        if (b[cy] & (0x80u >> cc)) st7305_draw_pixel(lcd, x + cc, y + aofs + cy, fg);
            }
            x += 8; p++;
        }
    }
}

/* 商店默认图标 (店铺图形) */
static void am_draw_store_icon(st7305_handle_t *lcd, int cx, int cy, int size) {
    int h2 = size / 2;
    fill_rect(lcd, cx - h2 + 4,  cy - h2,  cx + h2 - 4,  cy - 2, ST7305_COLOR_BLACK);
    fill_rect(lcd, cx - h2 + 12, cy + 2,   cx + h2 - 12, cy + h2 - 2, ST7305_COLOR_BLACK);
    fill_rect(lcd, cx - 6,       cy + 4,   cx + 6,       cy + h2 - 2, ST7305_COLOR_WHITE);
}

/* 实心圆 (用于"已添加到主菜单"角标, 参考 3.3 app_fill_circle) */
static void am_fill_circle(st7305_handle_t *lcd, int cx, int cy, int r, st7305_color_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                st7305_draw_pixel(lcd, cx + dx, cy + dy, c);
}
/* 已添加到主菜单的应用: 图标右上角实心黑圆点 (半径3, 往右上各偏移4px) */
static void am_draw_mainmenu_dot(st7305_handle_t *lcd, int cx, int cy, int size) {
    int dr = 3;
    am_fill_circle(lcd, cx + size / 2 - dr + 4, cy - size / 2 + dr - 4, dr, ST7305_COLOR_BLACK);
}

/* 应用图标 (网格视图) */
static void am_draw_app_icon(ui_ctx_t *ctx, st7305_handle_t *lcd, const os_app_t *a, int cx, int cy, int size, bool sel) {
    if (a) {
        const os_theme_t *th = os_theme_get();
        if (th->draw_icon) th->draw_icon(ctx, lcd, a->icon_idx, cx, cy, size, sel);
        else draw_main_icon_stretched(lcd, cx, cy, size, size, a->icon_idx);
    } else {
        am_draw_store_icon(lcd, cx, cy, size);
    }
}

/* 子分类条绘制 (y 顶部, 先填白底盖住下方内容残留, 恒固定显示) */
static void am_draw_subbar(ui_ctx_t *ctx, st7305_handle_t *lcd, int x0, int y0, int x1) {
    (void)ctx;
    /* 先整条白底, 覆盖拖动/滚动时滑到顶部的图标残留, 保证子分类条固定不被遮挡 */
    fill_rect(lcd, x0, y0, x1, y0 + AM_SUB_H - 1, ST7305_COLOR_WHITE);
    int w = x1 - x0;
    int slot = w / AM_SUB_COUNT;
    for (int i = 0; i < AM_SUB_COUNT; i++) {
        int sx = x0 + i * slot;
        int cx = sx + slot / 2;
        bool sel = (i == s_sub);
        if (sel) fill_rect(lcd, sx + 2, y0, sx + slot - 2, y0 + AM_SUB_H - 1, ST7305_COLOR_BLACK);
        am_draw_label(lcd, cx, y0 + 5, s_sub_names[i], sel);
    }
    fill_rect(lcd, x0, y0 + AM_SUB_H - 1, x1, y0 + AM_SUB_H - 1, ST7305_COLOR_BLACK);
}

/* 视图切换小图标 24×24 (最右): 网格=4宫格, 列表=3横条 */
static void am_draw_view_icon(st7305_handle_t *lcd, int x, int y) {
    if (s_view_grid) {
        /* 网格图标: 田字格 */
        fill_rect(lcd, x + 3, y + 3, x + 9, y + 9, ST7305_COLOR_BLACK);
        fill_rect(lcd, x + 14, y + 3, x + 20, y + 9, ST7305_COLOR_BLACK);
        fill_rect(lcd, x + 3, y + 14, x + 9, y + 20, ST7305_COLOR_BLACK);
        fill_rect(lcd, x + 14, y + 14, x + 20, y + 20, ST7305_COLOR_BLACK);
    } else {
        /* 列表图标: 3 横条 */
        fill_rect(lcd, x + 3, y + 4, x + 20, y + 7, ST7305_COLOR_BLACK);
        fill_rect(lcd, x + 3, y + 10, x + 20, y + 13, ST7305_COLOR_BLACK);
        fill_rect(lcd, x + 3, y + 16, x + 20, y + 19, ST7305_COLOR_BLACK);
    }
}

/* ==== 左侧分类栏布局 (前3个平均分布, 第4个商店贴底) ==== */
static int am_cat_cy(int r) {
    if (r >= AM_CAT_COUNT - 1)
        return UI_SCREEN_H - 1 - AM_ICON_SIZE / 2;      /* 最后一个贴底 */
    int top = AM_CAT_Y0;
    int bottom = UI_SCREEN_H - 1 - AM_ICON_SIZE;        /* 最后一个图标顶 */
    int slot = (bottom - top) / (AM_CAT_COUNT - 1);     /* 前3个均分 */
    return top + r * slot + AM_ICON_SIZE / 2;
}

/* ==== 渲染 ==== */
static void p_am_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    const os_theme_t *th = os_theme_get();
    st7305_clear(lcd, th->bg_color);

    int grid_x0 = AM_CATBAR_W;
    fill_rect(lcd, AM_CATBAR_W - 1, AM_CAT_Y0, AM_CATBAR_W, UI_SCREEN_H - 1, ST7305_COLOR_BLACK);

    /* 内容起点 (子分类条下方或直接状态栏下方) */
    int content_top = (s_cat == AM_CAT_APP || s_cat == AM_CAT_STORE) ? (AM_CAT_Y0 + AM_SUB_H) : AM_CAT_Y0;

    if (s_view_grid) {
        /* === 网格视图 (4×3, 相机 s_gcam 行单位, 跟手+吸附) === */
        int cell_w = (UI_SCREEN_W - grid_x0 - 8) / AM_GRID_COLS;
        int step_y = (UI_SCREEN_H - content_top - 8) / AM_GRID_ROWS;
        if (step_y < AM_ICON_SIZE + 18) step_y = AM_ICON_SIZE + 18;
        float cam = s_gcam;
        int grid_start_x = grid_x0 + 4;
        int first = (int)floorf(cam);
        int last = first + AM_GRID_ROWS;   /* 多画一行做进出过渡 */
        for (int row = first; row <= last; row++) {
            int y_center = content_top + 2 + (int)(((float)row - cam) * step_y) + AM_ICON_SIZE / 2;
            if (y_center - AM_ICON_SIZE / 2 > UI_SCREEN_H) break;
            /* 硬裁剪: 图标顶部不得进入子分类条/状态栏区域 (杜绝穿透) */
            if (y_center - AM_ICON_SIZE / 2 < content_top) continue;
            for (int col = 0; col < AM_GRID_COLS; col++) {
                int i = row * AM_GRID_COLS + col;
                if (i < 0 || i >= s_list_n) continue;
                int cx = grid_start_x + col * cell_w + cell_w / 2;
                bool sel = (i == s_focus);
                if (s_list_is_store) {
                    int ci = am_catalog_index(i);
                    const store_app_t *sa = (ci >= 0) ? &s_catalog[ci] : NULL;
                    am_draw_store_icon(lcd, cx, y_center, AM_ICON_SIZE);
                    const char *nm = sa ? sa->name : "\xe6\x9c\xaa\xe7\x9f\xa5";
                    int lw = am_label_width(nm);
                    int ly = y_center + AM_ICON_SIZE / 2 + 2;
                    if (sel) fill_rect(lcd, cx - lw / 2, ly, cx + lw / 2 - 1, ly + ZH16_FONT_H - 1, ST7305_COLOR_BLACK);
                    am_draw_label(lcd, cx, ly, nm, sel);
                } else {
                    const os_app_t *a = s_list[i];
                    am_draw_app_icon(ctx, lcd, a, cx, y_center, AM_ICON_SIZE, sel);
                    /* 已添加到主菜单: 图标右上角黑色圆点 (3.3 样式) */
                    if (a && os_app_in_mainmenu(a->id))
                        am_draw_mainmenu_dot(lcd, cx, y_center, AM_ICON_SIZE);
                    const char *nm = a ? a->label : "?";
                    int lw = am_label_width(nm);
                    int ly = y_center + AM_ICON_SIZE / 2 + 2;
                    if (sel) fill_rect(lcd, cx - lw / 2, ly, cx + lw / 2 - 1, ly + ZH16_FONT_H - 1, ST7305_COLOR_BLACK);
                    am_draw_label(lcd, cx, ly, nm, sel);
                }
            }
        }
    } else {
        /* === 单条列表视图 (相机 s_gcam 行单位, 跟手+吸附) === */
        int row_h = 46;
        float cam = s_gcam;
        int max_vis = (UI_SCREEN_H - content_top) / row_h;
        int list_max = s_list_n - max_vis;
        if (list_max < 0) list_max = 0;
        if (cam > list_max) cam = (float)list_max;
        if (cam < 0) cam = 0;
        int first = (int)floorf(cam);
        for (int i = first; i < s_list_n; i++) {
            int y = content_top + (int)(((float)i - cam) * row_h);
            if (y + row_h > UI_SCREEN_H) break;
            /* 硬裁剪: 行顶不得进入子分类条/状态栏区域 (杜绝穿透) */
            if (y < content_top) continue;
            bool sel = (i == s_focus);
            if (sel) fill_rect(lcd, grid_x0 + 2, y, UI_SCREEN_W - 2, y + row_h - 2, ST7305_COLOR_BLACK);
            else     fill_rect(lcd, grid_x0 + 2, y, UI_SCREEN_W - 2, y + row_h - 2, ST7305_COLOR_WHITE);
            if (i > first) fill_rect(lcd, grid_x0 + 2, y, UI_SCREEN_W - 2, y, ST7305_COLOR_BLACK);
            if (s_list_is_store) {
                int ci = am_catalog_index(i);
                const store_app_t *sa = (ci >= 0) ? &s_catalog[ci] : NULL;
                const char *nm = sa ? sa->name : "?";
                const char *br = sa ? sa->brief : "";
                /* 说明左对齐 (不居中, 避免挡左侧分割栏) */
                am_draw_text_l(lcd, grid_x0 + 12, y + 5, nm, sel);
                am_draw_text_l(lcd, grid_x0 + 12, y + 25, br, sel);
                bool inst = (ci >= 0) && s_store_installed[ci];
                const char *st = inst ? "\xe5\xb7\xb2\xe5\xae\x89\xe8\xa3\x85" : "\xe5\xae\x89\xe8\xa3\x85";
                am_draw_text_l(lcd, UI_SCREEN_W - 48, y + 12, st, sel);
            } else {
                const os_app_t *a = s_list[i];
                const char *nm = a ? a->label : "?";
                const char *id = a ? a->id : "";
                am_draw_text_l(lcd, grid_x0 + 12, y + 6, nm, sel);
                am_draw_text_l(lcd, grid_x0 + 12, y + 27, id, sel);
                bool mm = a && os_app_in_mainmenu(a->id);
                const char *st = mm ? "\xe5\x9c\xa8\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95" : "\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95";
                am_draw_text_l(lcd, UI_SCREEN_W - 58, y + 14, st, sel);
            }
        }
    }

    /* 顶部子分类条 + 视图切换图标 (内容之后绘制, 恒固定在最上层不被拖动遮挡) */
    if (s_cat == AM_CAT_APP || s_cat == AM_CAT_STORE) {
        am_draw_subbar(ctx, lcd, grid_x0 + 2, AM_CAT_Y0, UI_SCREEN_W - AM_VIEW_ICON - 6);
        /* 视图切换图标 (最右) */
        int vx = UI_SCREEN_W - AM_VIEW_ICON - 2;
        int vy = AM_CAT_Y0 + 1;
        fill_rect(lcd, vx - 1, vy - 1, vx + AM_VIEW_ICON, vy + AM_VIEW_ICON, ST7305_COLOR_WHITE);
        fill_rect(lcd, vx - 1, vy - 1, vx + AM_VIEW_ICON, vy - 1, ST7305_COLOR_BLACK);
        am_draw_view_icon(lcd, vx, vy);
    }

    if (s_list_n == 0) {
        draw_text(lcd, grid_x0 + 40, content_top + 60,
                  "\xe8\xaf\xa5\xe5\x88\x86\xe7\xb1\xbb\xe6\x9a\x82\xe6\x97\xa0\xe5\xba\x94\xe7\x94\xa8", false);
        /* 该分类暂无应用 */
    }

    /* 左侧分类栏 (引擎/游戏/应用/商店: 前3均分 + 商店贴底, 仅图标, 不显示名称, 选中=图标下方 2px 横线) */
    for (int r = 0; r < AM_CAT_COUNT; r++) {
        int icon_cx = AM_CATBAR_W / 2;
        int icon_cy = am_cat_cy(r);
        draw_cat_icon_stretched(lcd, icon_cx, icon_cy, AM_ICON_SIZE, AM_ICON_SIZE, r);
        if (r == s_cat) {
            int lx0 = icon_cx - AM_ICON_SIZE / 2;
            int lx1 = icon_cx + AM_ICON_SIZE / 2;
            int ly  = icon_cy + AM_ICON_SIZE / 2 + 1;
            fill_rect(lcd, lx0, ly, lx1 - 1, ly + 1, ST7305_COLOR_BLACK);
        }
    }
}

/* 商店条目 i → catalog 索引 (重建时按子分类过滤, 此处线性映射) */
static int am_catalog_index(int i) {
    int n = 0;
    for (int k = 0; k < s_catalog_n; k++) {
        if (s_sub != AM_SUB_ALL && s_catalog[k].cat != s_sub) continue;
        if (n == i) return k;
        n++;
    }
    return -1;
}

/* ==== 操作: 已安装应用 操作弹窗 ==== */
static void am_app_op_cb(ui_ctx_t *ctx, int result, void *ud) {
    const os_app_t *a = (const os_app_t *)ud;
    if (!a) return;
    switch (result) {
    case 0: os_push(ctx, a->page); break;                      /* 打开 */
    case 1: os_app_set_mainmenu(a->id, true);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe6\xb7\xbb\xe5\x8a\xa0\xe5\x88\xb0\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95"); /* 已添加到主菜单 */
            ctx->needs_redraw = true; break;
    case 2: os_app_set_mainmenu(a->id, false);
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe7\xa7\xbb\xe5\x87\xba\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95"); /* 已移出主菜单 */
            ctx->needs_redraw = true; break;
    case 3: os_app_set_hidden(a->id, true);                    /* 卸载=隐藏 */
            os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x8d\xb8\xe8\xbd\xbd"); /* 已卸载 */
            ctx->needs_redraw = true; break;
    default: break;   /* 返回 */
    }
}
static void am_app_op(ui_ctx_t *ctx, const os_app_t *a) {
    if (!a) return;
    static const char *items[5] = {
        "\xe6\x89\x93\xe5\xbc\x80",                    /* 打开 */
        "\xe6\xb7\xbb\xe5\x8a\xa0\xe5\x88\xb0\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95", /* 添加到主菜单 */
        "\xe7\xa7\xbb\xe5\x87\xba\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95", /* 移出主菜单 */
        "\xe5\x8d\xb8\xe8\xbd\xbd",                    /* 卸载 */
        "\xe8\xbf\x94\xe5\x9b\x9e",                    /* 返回 */
    };
    os_dialog_list(ctx, a->label, items, 5, 0, am_app_op_cb, (void *)a);
}

/* ==== 操作: 商店 安装/卸载 ==== */
static void am_store_op_cb(ui_ctx_t *ctx, int result, void *ud) {
    int ci = (int)(intptr_t)ud;
    if (result == 0) {
        if (ci >= 0 && ci < STORE_MAX) {
            bool inst = s_store_installed[ci];
            s_store_installed[ci] = !inst;
            store_nvs_save();
            os_dialog_toast(ctx, inst ? "\xe5\xb7\xb2\xe5\x8d\xb8\xe8\xbd\xbd" : "\xe5\xae\x89\xe8\xa3\x85\xe5\xae\x8c\xe6\x88\x90");
            ctx->needs_redraw = true;
        }
    }
}
static void am_store_op(ui_ctx_t *ctx, int ci) {
    if (ci < 0 || ci >= s_catalog_n) return;
    const store_app_t *sa = &s_catalog[ci];
    bool inst = s_store_installed[ci];
    char *items[2] = { "\xe5\xae\x89\xe8\xa3\x85", "\xe8\xbf\x94\xe5\x9b\x9e" }; /* 安装/返回 */
    if (inst) items[0] = "\xe5\x8d\xb8\xe8\xbd\xbd";   /* 已装 → 卸载 */
    os_dialog_list(ctx, sa->name, (const char *const *)items, 2, 0, am_store_op_cb, (void *)(intptr_t)ci);
}

/* ==== 交互 ==== */
static void p_am_action(ui_ctx_t *ctx, os_action_t a) {
    switch (a) {
    case OS_ACTION_UP:
        if (s_view_grid) {
            if (s_focus >= AM_GRID_COLS) {
                s_focus -= AM_GRID_COLS;
                am_gcam_animate(s_focus / AM_GRID_COLS, 300);
            }
        } else {
            if (s_focus > 0) { s_focus--; am_gcam_animate(s_focus, 260); }
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_DOWN:
        if (s_view_grid) {
            if (s_focus + AM_GRID_COLS < s_list_n) {
                s_focus += AM_GRID_COLS;
                am_gcam_animate(s_focus / AM_GRID_COLS, 300);
            }
        } else {
            if (s_focus + 1 < s_list_n) { s_focus++; am_gcam_animate(s_focus, 260); }
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_LEFT:
        if (s_focus - 1 >= 0) {
            s_focus--;
            if (s_view_grid) am_gcam_animate(s_focus / AM_GRID_COLS, 260);
            else am_gcam_animate(s_focus, 260);
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_RIGHT:
        if (s_focus + 1 < s_list_n) {
            s_focus++;
            if (s_view_grid) am_gcam_animate(s_focus / AM_GRID_COLS, 260);
            else am_gcam_animate(s_focus, 260);
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM:
        if (s_list_n <= 0) break;
        if (s_list_is_store) {
            int ci = am_catalog_index(s_focus);
            am_store_op(ctx, ci);
        } else {
            am_app_op(ctx, s_list[s_focus]);
        }
        break;
    case OS_ACTION_BACK:
    case OS_ACTION_HOME:
        os_pop(ctx);
        break;
    default:
        break;
    }
}

static bool p_am_touch(ui_ctx_t *ctx, int x, int y) {
    /* 拖动滚动中: 忽略点击 (松手后由 poll 复位 s_gdrag_active) */
    if (s_gdrag_active) return true;
    /* 左侧分类栏 (触摸按图标中心判定, 触摸区上下延伸: 取最近的分类中心) */
    if (x < AM_CATBAR_W) {
        int best = -1, best_d = 1 << 30;
        for (int r = 0; r < AM_CAT_COUNT; r++) {
            int d = y - am_cat_cy(r);
            if (d < 0) d = -d;
            if (d < best_d) { best_d = d; best = r; }
        }
        if (best >= 0 && best != s_cat) {
            s_cat = best;
            s_sub = AM_SUB_ALL;
            s_focus = 0; s_scroll = 0;
            am_rebuild();
            ctx->needs_redraw = true;
        }
        return true;
    }
    /* 顶部子分类条 + 视图切换图标 */
    if (y >= AM_CAT_Y0 && y < AM_CAT_Y0 + AM_SUB_H && (s_cat == AM_CAT_APP || s_cat == AM_CAT_STORE)) {
        /* 视图切换图标 (最右) */
        int vx = UI_SCREEN_W - AM_VIEW_ICON - 2;
        if (x >= vx - 2 && x <= vx + AM_VIEW_ICON + 2) {
            s_view_grid = !s_view_grid;
            s_focus = 0; s_scroll = 0;
            am_rebuild();
            ctx->needs_redraw = true;
            return true;
        }
        /* 子分类 */
        int slot = (UI_SCREEN_W - AM_CATBAR_W - AM_VIEW_ICON - 8) / AM_SUB_COUNT;
        int sub = (x - (AM_CATBAR_W + 2)) / slot;
        if (sub >= 0 && sub < AM_SUB_COUNT && sub != s_sub) {
            s_sub = sub;
            s_focus = 0; s_scroll = 0;
            am_rebuild();
            ctx->needs_redraw = true;
        }
        return true;
    }
    /* 右栏内容 (y < content_top: 顶部状态栏/子分类条区域 → 不触发下方图标) */
    int content_top = (s_cat == AM_CAT_APP || s_cat == AM_CAT_STORE) ? (AM_CAT_Y0 + AM_SUB_H) : AM_CAT_Y0;
    if (y < content_top) return false;
    if (s_view_grid) {
        int cell_w = (UI_SCREEN_W - AM_CATBAR_W - 8) / AM_GRID_COLS;
        int step_y = (UI_SCREEN_H - content_top - 8) / AM_GRID_ROWS;
        if (step_y < AM_ICON_SIZE + 18) step_y = AM_ICON_SIZE + 18;
        int col = (x - (AM_CATBAR_W + 4)) / cell_w;
        int row_vis = (y - (content_top + 2)) / step_y;
        if (col < 0 || col >= AM_GRID_COLS || row_vis < 0) return false;
        int row = (int)floorf(s_gcam) + row_vis;
        int idx = row * AM_GRID_COLS + col;
        if (idx >= 0 && idx < s_list_n) {
            if (idx == s_focus) {
                if (s_list_is_store) { int ci = am_catalog_index(idx); am_store_op(ctx, ci); }
                else am_app_op(ctx, s_list[idx]);
            } else s_focus = idx;
            ctx->needs_redraw = true;
            return true;
        }
    } else {
        int row_h = 46;
        int idx = (int)floorf(s_gcam) + (y - content_top) / row_h;
        if (idx >= 0 && idx < s_list_n) {
            if (idx == s_focus) {
                if (s_list_is_store) { int ci = am_catalog_index(idx); am_store_op(ctx, ci); }
                else am_app_op(ctx, s_list[idx]);
            } else s_focus = idx;
            ctx->needs_redraw = true;
            return true;
        }
    }
    return false;
}

/* ==== 每帧轮询: 触摸上下拖动 (相机跟手 + 惯性甩动 + 网格吸附, 仿主菜单) ==== */
static void p_am_poll(ui_ctx_t *ctx) {
    /* 相机动画进行中: 每帧请求重绘 */
    if (s_gcam_anim_start > 0) {
        am_gcam_tick();
        ctx->needs_redraw = true;
    }
    if (s_list_n <= 0) return;

    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    int content_top = (s_cat == AM_CAT_APP || s_cat == AM_CAT_STORE) ? (AM_CAT_Y0 + AM_SUB_H) : AM_CAT_Y0;
    int row_h = s_view_grid
        ? ((UI_SCREEN_H - content_top - 8) / AM_GRID_ROWS) : 46;
    if (s_view_grid && row_h < AM_ICON_SIZE + 18) row_h = AM_ICON_SIZE + 18;
    int max_row = s_view_grid ? am_max_scroll() : 0;
    if (!s_view_grid) {
        int mv = (UI_SCREEN_H - content_top) / 46;
        max_row = s_list_n - mv;
        if (max_row < 0) max_row = 0;
    }

    if (!down) {
        /* 松手: 平滑吸附到最近焦点行 (无惯性甩手/弹簧回弹) */
        if (s_gdrag_active) {
            s_gdrag_active = false;
            am_gcam_animate(lroundf(s_gcam), 260);
            ctx->needs_redraw = true;
        }
        s_gdrag_last_y = -1;
        return;
    }

    /* 只在右栏内容区开始拖动 */
    if (tx < AM_CATBAR_W || ty < content_top || ty >= UI_SCREEN_H) {
        s_gdrag_last_y = -1;
        return;
    }

    if (s_gdrag_last_y < 0) {
        s_gdrag_last_y  = ty;
        s_gdrag_anchor_y = ty;
        s_gdrag_base     = s_gcam;
        s_gdrag_active   = false;
        s_gcam_anim_start = 0;   /* 新拖动取消旧动画 */
        return;
    }
    s_gdrag_last_y = ty;
    /* 跟手: 相机 = 基点 - 位移/行高 */
    s_gcam = s_gdrag_base - (float)(ty - s_gdrag_anchor_y) / (float)row_h;
    /* 边界 */
    if (s_gcam < 0) s_gcam = 0;
    if (s_gcam > max_row) s_gcam = (float)max_row;
    int moved = ty - s_gdrag_anchor_y;
    if (moved < 0) moved = -moved;
    if (moved >= AM_DRAG_THRESH) {
        s_gdrag_active = true;
        ctx->needs_redraw = true;
    }
}

/* ==== 商店清单加载 ==== */
static void store_nvs_save(void) {
    nvs_handle_t h;
    if (nvs_open(STORE_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint32_t mask = 0;
        for (int i = 0; i < STORE_MAX && i < 32; i++) if (s_store_installed[i]) mask |= (1u << i);
        nvs_set_u32(h, "installed", mask);
        nvs_commit(h);
        nvs_close(h);
    }
}
static void store_catalog_init(void) {
    if (s_catalog_n > 0) return;
    int n = 0;
    memcpy(&s_catalog[n], s_builtin_catalog, sizeof(s_builtin_catalog));
    n += BUILTIN_COUNT;
    s_catalog_n = n;
    nvs_handle_t h;
    if (nvs_open(STORE_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t mask = 0;
        if (nvs_get_u32(h, "installed", &mask) == ESP_OK) {
            for (int i = 0; i < STORE_MAX && i < 32; i++) s_store_installed[i] = (mask >> i) & 1;
        }
        nvs_close(h);
    }
}
static void store_fetch(void) {
    if (!wifi_manager_is_connected()) return;
    char *body = NULL;
    esp_http_client_config_t cfg = { .url = STORE_MANIFEST, .timeout_ms = 5000, .buffer_size = 512 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        int len = esp_http_client_fetch_headers(c);
        if (len > 0 && len < 2048) {
            body = malloc((size_t)len + 1);
            if (body) {
                int r = esp_http_client_read_response(c, body, len);
                if (r > 0) body[r] = '\0';
                else { free(body); body = NULL; }
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (body) {
        char *p = body;
        while ((p = strstr(p, "\"id\":\"")) != NULL) {
            p += 6;
            char id[24] = {0}; int i = 0;
            while (*p && *p != '"' && i < 23) id[i++] = *p++;
            char *nm = strstr(p, "\"name\":\"");
            char name[24] = {0};
            if (nm) { nm += 8; int j = 0; while (*nm && *nm != '"' && j < 23) name[j++] = *nm++; }
            if (id[0] && name[0]) {
                int found = -1;
                for (int k = 0; k < s_catalog_n; k++) if (strcmp(s_catalog[k].id, id) == 0) { found = k; break; }
                if (found >= 0) strncpy(s_catalog[found].name, name, sizeof(s_catalog[found].name) - 1);
                else if (s_catalog_n < STORE_MAX) {
                    store_app_t *a = &s_catalog[s_catalog_n++];
                    snprintf(a->id, sizeof(a->id), "%s", id);
                    snprintf(a->name, sizeof(a->name), "%s", name);
                    snprintf(a->brief, sizeof(a->brief), "%s", "\xe5\xae\x98\xe6\x96\xb9\xe5\xba\x94\xe7\x94\xa8"); /* 官方应用 */
                    a->cat = AM_SUB_PROG;
                }
            }
        }
        free(body);
    }
}

/* ==== 模块 ==== */
static void p_am_enter(ui_ctx_t *ctx) {
    store_catalog_init();
    store_fetch();
    s_cat = AM_CAT_ENGINE;
    s_sub = AM_SUB_ALL;
    s_focus = 0; s_scroll = 0;
    am_rebuild();
    /* 无触摸屏: 提示触摸类应用已隐藏/禁用 (白板/触屏测试等) */
    if (!touch_panel_is_present())
        os_dialog_toast(ctx, "\xe6\x9c\xaa\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0\xe8\xa7\xa6\xe6\x91\xb8\xe5\xb1\x8f\xef\xbc\x8c\xe8\xa7\xa6\xe6\x91\xb8\xe7\xb1\xbb\xe5\xba\x94\xe7\x94\xa8\xe5\xb7\xb2\xe9\x9a\x90\xe8\x97\x8f"); /* 未检测到触摸屏，触摸类应用已隐藏 */
}

static const os_module_t s_mod_app_manager = {
    .name       = "app_butler",
    .page_id    = OS_PAGE_APP_MANAGER,
    .on_enter   = p_am_enter,
    .render     = p_am_render,
    .action     = p_am_action,
    .touch      = p_am_touch,
    .poll       = p_am_poll,
    .fullscreen = false,   /* 显示状态栏 */
};

void os_page_app_manager_register(void) { os_register(&s_mod_app_manager); }