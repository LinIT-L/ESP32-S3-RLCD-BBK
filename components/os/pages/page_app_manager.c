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
#include "engine_manager.h"   /* 返回应用管家统一卸载引擎 (应用管家=主菜单级边界) */
#include "input.h"
#include "vibrator.h"         /* V1.5.x: 点击应用震动 (ctx_block 屏蔽 input 层, 打开/选中时补) */
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
#define AM_CATBAR_W    75     /* 左侧分类栏宽度 (V1.5.x: 4px左缘+4px边距+64图标+2px空隙+1px分割线) */
#define AM_CAT_Y0      24     /* 分类栏起始 y = 状态栏边界 */
#define AM_CAT_X0      4      /* V1.5.x: 左缘留白, 让左角外凸圆角有显示空间 (需 ≥ 选中框 R) */
#define AM_ICON_SIZE   64     /* 网格图标尺寸 */
#define AM_ICON_GAP    4      /* V1.5.x: 图标周围/间隔 4px 空间 */
#define AM_ICON_X      (AM_CAT_X0 + AM_ICON_GAP + AM_ICON_SIZE / 2)  /* 图标中心 x = 40 (左缘 4px 留白) */
#define AM_SEL_GAP     2      /* V1.5.x: 选中指示线距图标 2px */
#define AM_SEL_W       2      /* V1.5.x: 选中指示线宽 2px */
#define AM_SEL_R       3      /* V1.5.x: 选中指示圆角半径 R3 */
#define AM_GRID_COLS   4      /* 网格列数 */
#define AM_GRID_ROWS   3      /* 网格可见行数 */
#define AM_SUB_H       26     /* 顶部子分类条高 */
#define AM_VIEW_ICON   24     /* 视图切换小图标 24×24 */

/* 左侧 4 分类 (引擎/游戏/应用/运维) */
#define AM_CAT_ENGINE  0   /* 引擎 */
#define AM_CAT_GAME    1   /* 游戏 */
#define AM_CAT_APP     2   /* 应用 */
#define AM_CAT_STORE   3   /* 运维 (原商店, 已改为已安装运维应用) */
#define AM_CAT_COUNT   4

/* 商店/应用 子分类 (横排条) */
#define AM_SUB_ALL     0
#define AM_SUB_GAME    1   /* 游戏 */
#define AM_SUB_PROG    2   /* 软件 (原 程序) */
#define AM_SUB_SYS     3   /* 系统 (新增) */
#define AM_SUB_OPS     4   /* 运维 */
#define AM_SUB_STUDY   5   /* 学习 */
#define AM_SUB_COUNT   6
static const char *const s_sub_names[AM_SUB_COUNT] = {
    "\xe5\x85\xa8\xe9\x83\xa8",  /* 全部 */
    "\xe6\xb8\xb8\xe6\x88\x8f",  /* 游戏 */
    "\xe8\xbd\xaf\xe4\xbb\xb6",  /* 软件 */
    "\xe7\xb3\xbb\xe7\xbb\x9f",  /* 系统 */
    "\xe8\xbf\x90\xe7\xbb\xb4",  /* 运维 */
    "\xe5\xad\xa6\xe4\xb9\xa0",  /* 学习 */
};

/* ==== 页面状态 ==== */
static int s_cat = AM_CAT_ENGINE;     /* 左栏分类 */
static int s_sub = AM_SUB_ALL;        /* 顶部子分类 */
static bool s_view_grid = true;       /* true=网格  false=单条列表 */
static int  s_focus = 0;              /* 右栏选中索引 */
static int  s_scroll = 0;             /* 上卷行数 */
static int  s_focus_bar = 0;          /* 方向键聚焦区: 0=内容网格, 1=顶部子分类横排, 2=左侧分类竖排 */
static int s_list_n = 0;             /* 当前显示条目数 */
static const os_app_t *s_list[64];    /* 已安装条目 (应用分类, 预留扩容) */

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

/* 长按 (首次按下即算, 无需放手指): 按住图标 3s 弹"添加/删除桌面快捷方式"确认 */
#define AM_LP_MS       3000
#define AM_LP_DRAG_MAX 12      /* 长按期间位移超过此值则取消长按意图 */
static int      s_lp_idx = -1;        /* 长按目标索引 (-1=无) */
static uint32_t s_lp_t0  = 0;         /* 按下时刻 */
static bool     s_lp_fired = false;   /* 已触发, 防重复 */
static bool     s_lp_suppress = false;/* 抑制长按松手后的那次 tap */

/* 视图切换 (网格/列表): 长按硬件确认键 3s 无提示切换.
 * 确认键长按 2s 时 input 层先投递 OS_ACTION_BT_SEARCH (os_core 已为应用管家拦截),
 * 该动作到达本页作为"已按住 2s"的起点, 再按住满 1s (累计 3s) 即切换. */
static bool     s_vt_armed = false;   /* 已收到 BT_SEARCH 起点, 正在等待满 3s */
static uint32_t s_vt_t0   = 0;        /* BT_SEARCH 起点时刻 (ms) */
static bool     s_vt_fired = false;   /* 已切换, 防 hold 期间重复 */

/* 前向声明 (定义在下方, 交叉引用) */
static void am_clamp_scroll(void);
static void am_rebuild(void);          /* 常规重建: 相机对齐到焦点行 */
static void am_rebuild_keep(void);     /* 返回重建: 保留原滚动位置 */

/* 子分类是否匹配应用 */
static bool app_match_sub(const os_app_t *a) {
    if (!a || !a->cat) return (s_sub == AM_SUB_ALL);
    if (s_sub == AM_SUB_ALL) return true;
    const char *c = a->cat;
    if (s_sub == AM_SUB_GAME && strcmp(c, "\xe6\xb8\xb8\xe6\x88\x8f") == 0) return true;  /* 游戏 */
    if (s_sub == AM_SUB_PROG && (strcmp(c, "\xe7\xa8\x8b\xe5\xba\x8f") == 0 || strcmp(c, "\xe8\xbd\xaf\xe4\xbb\xb6") == 0)) return true; /* 程序/软件 */
    if (s_sub == AM_SUB_SYS  && (strcmp(c, "\xe7\xb3\xbb\xe7\xbb\x9f") == 0 || strcmp(c, "\xe5\xb7\xa5\xe5\x85\xb7") == 0)) return true;  /* 系统/工具 */
    if (s_sub == AM_SUB_OPS  && strcmp(c, "\xe8\xbf\x90\xe7\xbb\xb4") == 0) return true;  /* 运维 */
    if (s_sub == AM_SUB_STUDY&& strcmp(c, "\xe5\xad\xa6\xe4\xb9\xa0") == 0) return true;  /* 学习 */
    return false;
}
/* 左侧分类是否匹配应用 (引擎/游戏/应用) */
/* 引擎分类固定顺序 (用户指定): 步步高/文曲星/暴龙机/ArduBoy/GB/GBC/NES */
static const char *const s_engine_order[] = {
    "bbk", "lavax", "vpet", "arduboy", "gb", "gbc", "nes", "md", "sms"
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
        /* 游戏分类(独立): 显示 cat=游戏 的非引擎应用 (如手柄); 引擎(GB/NES等)归引擎分类, 不在此重复 */
        if (!a->cat || strcmp(a->cat, "\xe6\xb8\xb8\xe6\x88\x8f") != 0) return false;
        for (int i = 0; i < ENGINE_ORDER_COUNT; i++)
            if (strcmp(id, s_engine_order[i]) == 0) return false;
        return true;
    case AM_CAT_APP: {
        /* 应用: 排除引擎 + 排除运维工具 + 排除游戏, 避免在多分类重复显示 */
        if (strcmp(id, "app_mgr") == 0) return false;
        if (a->cat && (strcmp(a->cat, "\xe8\xbf\x90\xe7\xbb\xb4") == 0 ||
                       strcmp(a->cat, "\xe6\xb8\xb8\xe6\x88\x8f") == 0)) return false;
        for (int i = 0; i < ENGINE_ORDER_COUNT; i++)
            if (strcmp(id, s_engine_order[i]) == 0) return false;
        return app_match_sub(a);
    }
    }
    return false;
}

static float am_gcam_clamp(float c);   /* 前向声明 (定义在下方) */

/* 当前视图可见行数 (网格=AM_GRID_ROWS 行; 列表=整屏可容纳的行数) */
static int am_vis_rows(void) {
    return s_view_grid ? AM_GRID_ROWS : ((UI_SCREEN_H - AM_CAT_Y0) / 46);
}
/* 焦点所在行 (网格=每行列数归组; 列表=一项一行) */
static int am_focus_row(void) {
    return s_view_grid ? (s_focus / AM_GRID_COLS) : s_focus;
}

/* 重建右侧列表.
 * keep_cam=true  (从应用返回): 保留当前滚动位置, 仅当焦点已滚出视野才把相机拉回焦点行.
 *   修复"触摸点开第二排的应用, 退出后第一排消失、第二排顶到第一排" —— 触摸换焦点时
 *   只改 s_focus 不改相机 (p_am_touch), 若这里无条件把相机对齐焦点行, 画面就会凭空跳一行.
 * keep_cam=false (切分类/子分类/视图): 相机对齐焦点行 (常规重置). */
static void am_rebuild_ex(bool keep_cam) {
    s_list_n = 0;
    if (s_cat == AM_CAT_STORE) {
        /* 运维: 显示 cat=运维 的已安装应用 */
        int total = os_app_count();
        for (int i = 0; i < total && s_list_n < 64; i++) {
            const os_app_t *a = os_app_get(i);
            if (a && a->cat && strcmp(a->cat, "\xe8\xbf\x90\xe7\xbb\xb4") == 0 &&
                (s_sub == AM_SUB_ALL || app_match_sub(a)))
                s_list[s_list_n++] = a;
        }
    } else if (s_cat == AM_CAT_ENGINE) {
        /* 引擎: 按用户指定顺序插入 (从注册表取对应应用) */
        for (int o = 0; o < ENGINE_ORDER_COUNT && s_list_n < 64; o++) {
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
        for (int i = 0; i < total && s_list_n < 64; i++) {
            const os_app_t *a = os_app_get(i);
            if (app_match_cat(a)) s_list[s_list_n++] = a;
        }
    }
    if (s_focus >= s_list_n) s_focus = 0;
    if (s_focus < 0) s_focus = 0;
    if (keep_cam) {
        /* 保留原画面: 焦点在视野内就不动相机, 只有焦点被滚出视野才拉回去 */
        int frow  = am_focus_row();
        int fvis  = (int)(s_gcam + 0.5f);
        int vlast = fvis + am_vis_rows() - 1;
        if (frow < fvis || frow > vlast) s_gcam = (float)frow;
        s_gcam   = am_gcam_clamp(s_gcam);
        s_scroll = (int)s_gcam;
    } else {
        /* 网格: 相机吸附到焦点行 */
        s_gcam   = (float)(s_focus / AM_GRID_COLS);
        s_scroll = s_focus / AM_GRID_COLS;
    }
    s_gcam_anim_start = 0;
    am_clamp_scroll();
}
static void am_rebuild(void)      { am_rebuild_ex(false); }
static void am_rebuild_keep(void) { am_rebuild_ex(true);  }
static int am_max_scroll(void) {
    if (!s_view_grid) {
        int content_top = AM_CAT_Y0;
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
/* 网格吸附动画: 只移动相机 (不回写焦点, 焦点由方向键直接推进).
 * 关键修复: 原来是动画把焦点钳到 am_max_scroll() 行, 导致方向键到不了最后一排
 * (最多只剩 (max_scroll+1)*列 个); 现在相机最多滚到视图容纳处, 焦点可自由推到任意项. */
static void am_gcam_animate(int to_row, uint32_t dur_ms) {
    int total_rows = am_max_scroll();
    if (to_row > total_rows) to_row = total_rows;
    if (to_row < 0) to_row = 0;
    float from_c = am_gcam_clamp(s_gcam);
    float to_c = (float)to_row;
    if (from_c == to_c) {
        s_gcam = to_c;
        s_gcam_anim_start = 0;
        return;
    }
    s_gcam_anim_from   = from_c;
    s_gcam_anim_to     = to_c;
    s_gcam_anim_start  = am_now_ms();
    s_gcam_anim_dur    = dur_ms ? dur_ms : 1;
    s_gcam_anim_sel    = to_row;
}
/* 吸附完成: 相机锁定到最近焦点行 (只锁相机, 不动 s_focus) */
static void am_gcam_settle(void) {
    s_gcam = am_gcam_clamp(s_gcam);
    int row = (int)(s_gcam + 0.5f);
    s_gcam = (float)row;
    s_gcam_anim_start = 0;
    (void)row;
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
    int aofs = (ZH16_FONT_H - 16) / 2;
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
                for (int cy = 0; cy < 16; cy++)
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
    int aofs = (ZH16_FONT_H - 16) / 2;
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
                for (int cy = 0; cy < 16; cy++)
                    for (int cc = 0; cc < 8; cc++)
                        if (b[cy] & (0x80u >> cc)) st7305_draw_pixel(lcd, x + cc, y + aofs + cy, fg);
            }
            x += 8; p++;
        }
    }
}

/* 实心圆 (用于"已添加到主菜单"角标, 参考 3.3 app_fill_circle) */
static void am_fill_circle(st7305_handle_t *lcd, int cx, int cy, int r, st7305_color_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                st7305_draw_pixel(lcd, cx + dx, cy + dy, c);
}
/* 已添加到桌面(主菜单)的应用: 图标右上角圆形标识.
 * 三层同心圆 (白黑白) 保证黑色背景/白色背景都能看清. */
static void am_draw_mainmenu_dot(st7305_handle_t *lcd, int cx, int cy, int size) {
    int ccx = cx + size / 2 - 4;      /* 右上角, 往里收几像素 */
    int ccy = cy - size / 2 + 4;
    am_fill_circle(lcd, ccx, ccy, 5, ST7305_COLOR_WHITE);  /* 外圈白 (黑色背景可见) */
    am_fill_circle(lcd, ccx, ccy, 3, ST7305_COLOR_BLACK);  /* 中圈黑 (白色背景可见) */
    am_fill_circle(lcd, ccx, ccy, 1, ST7305_COLOR_WHITE);  /* 内圈白 */
}

/* 应用图标 (网格视图) */
static void am_draw_app_icon(ui_ctx_t *ctx, st7305_handle_t *lcd, const os_app_t *a, int cx, int cy, int size, bool sel) {
    const os_theme_t *th = os_theme_get();
    if (a && th->draw_icon) th->draw_icon(ctx, lcd, a->icon_idx, cx, cy, size, sel);
    else draw_main_icon_stretched(lcd, cx, cy, size, size, a->icon_idx);
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
        /* 顶部子分类聚焦中: 给当前子分类画 U 形括号 (顶+两侧, 底部开口并向下延伸),
         * 表示"正在浏览该分类, 下面内容归它管" — 区别于普通黑块, 便于分辨在哪一层 */
        if (s_focus_bar == 1 && i == s_sub) {
            int bx0 = sx + 1, bx1 = sx + slot - 1, by = y0 - 3, bbot = y0 + AM_SUB_H + 16;
            draw_hline(lcd, bx0, bx1, by, ST7305_COLOR_BLACK);
            draw_vline(lcd, bx0, by, bbot, ST7305_COLOR_BLACK);
            draw_vline(lcd, bx1, by, bbot, ST7305_COLOR_BLACK);
        }
    }
    fill_rect(lcd, x0, y0 + AM_SUB_H - 1, x1, y0 + AM_SUB_H - 1, ST7305_COLOR_BLACK);
}

/* ==== 选中框样式 (V1.6.x) ====
 * - 选中态 = 一根 2px 水管线 (直线 + 90°外凸圆弧弯头, 像水管拐弯), 不使用曲线弧/圆端.
 * - 顶/中/底: 左竖边 + 相应横边, 弯头处直边让位 R.
 * - 常驻分割线在选中行让位, 其余分类右侧保留竖条分隔. */

/* 90°外凸圆弧弯头 (像水管拐弯): 拐点 (cx,cy), 半径 r, 带宽 2px, 圆弧向外凸出.
 * kind=1: 外凸向左上 (左上弯: 竖线在左 + 上横线在上), 圆心 (cx+r, cy+r).
 * kind=2: 外凸向左下 (左下弯: 竖线在左 + 下横线在下), 圆心 (cx+r, cy-r).
 * kind=3: 外凸向右上 (右上弯: 竖线在右 + 上横线在上), 圆心 (cx-r, cy+r).
 * kind=4: 外凸向右下 (右下弯: 竖线在右 + 下横线在下), 圆心 (cx-r, cy-r).
 * 保留到圆心距离 d∈[r-2,r] 的环带像素 (各角朝外凸出的 90° 弧). */
static void am_elbow(st7305_handle_t *lcd, int cx, int cy, int r, int kind, st7305_color_t color) {
    int ox, oy, x0, x1, y0, y1, sx, sy;
    if (kind == 1)      { ox = cx + r; oy = cy + r; x0 = cx;    x1 = cx + r; y0 = cy;    y1 = cy + r; sx = -1; sy = -1; }
    else if (kind == 2) { ox = cx + r; oy = cy - r; x0 = cx;    x1 = cx + r; y0 = cy - r; y1 = cy;    sx = -1; sy =  1; }
    else if (kind == 3) { ox = cx - r; oy = cy + r; x0 = cx - r; x1 = cx;    y0 = cy;    y1 = cy + r; sx =  1; sy = -1; }
    else                { ox = cx - r; oy = cy - r; x0 = cx - r; x1 = cx;    y0 = cy - r; y1 = cy;    sx =  1; sy =  1; }
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= UI_SCREEN_H) continue;
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x >= UI_SCREEN_W) continue;
            float dx = (float)x - ox;
            float dy = (float)y - oy;
            if (!((sx * dx >= 0) && (sy * dy >= 0))) continue;
            float d = sqrtf(dx * dx + dy * dy);
            if (d >= (r - 2) && d <= r) st7305_draw_pixel(lcd, x, y, color);
        }
    }
}

/* ==== 左侧分类栏布局 (V1.5.x: 4 图标等距排布, 顶部距状态栏 4px,
 * 底部留 6px 供选中框下缘+外凸圆角完整显示) ==== */
static int am_cat_cy(int r) {
    int top = AM_CAT_Y0 + AM_ICON_GAP;               /* 图标0顶: 状态栏 + 4px */
    int slot = AM_ICON_SIZE + AM_ICON_GAP - 1;       /* 67: 图标间隔 3px (底部圆角让位 1px) */
    return top + r * slot + AM_ICON_SIZE / 2;
}

/* ==== 渲染 ==== */
static void p_am_render(ui_ctx_t *ctx) {
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    const os_theme_t *th = os_theme_get();
    st7305_clear(lcd, th->bg_color);

    int grid_x0 = AM_CATBAR_W;
    /* 常驻分割线已删除: 分类栏与内容区之间不再画贯穿竖线, 选中态由一根 2px 水管线承担 */

    /* 内容起点 (状态栏下方, 不再有顶部子分类条) */
    int content_top = AM_CAT_Y0;

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
                bool sel = !input_scheme_touch() && (i == s_focus);   /* 触屏: 不显示选中框 */
                const os_app_t *a = s_list[i];
                am_draw_app_icon(ctx, lcd, a, cx, y_center, AM_ICON_SIZE, sel);
                /* 已显示在主菜单的应用: 图标右上角黑色圆点 (3.3 样式).
                 * 用 os_app_visible(与主菜单遍历一致), 而非 os_app_in_mainmenu
                 * (后者只记录用户显式添加; 默认显示的应用不在该列表, 会漏标). */
                if (a && os_app_visible(a))
                    am_draw_mainmenu_dot(lcd, cx, y_center, AM_ICON_SIZE);
                const char *nm = a ? a->label : "?";
                int lw = am_label_width(nm);
                int ly = y_center + AM_ICON_SIZE / 2 + 2;
                if (sel) fill_rect(lcd, cx - lw / 2, ly, cx + lw / 2 - 1, ly + ZH16_FONT_H - 1, ST7305_COLOR_BLACK);
                am_draw_label(lcd, cx, ly, nm, sel);
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
            bool sel = !input_scheme_touch() && (i == s_focus);   /* 触屏: 不显示选中框 */
            if (sel) fill_rect(lcd, grid_x0 + 2, y, UI_SCREEN_W - 2, y + row_h - 2, ST7305_COLOR_BLACK);
            else     fill_rect(lcd, grid_x0 + 2, y, UI_SCREEN_W - 2, y + row_h - 2, ST7305_COLOR_WHITE);
            if (i > first) fill_rect(lcd, grid_x0 + 2, y, UI_SCREEN_W - 2, y, ST7305_COLOR_BLACK);
            const os_app_t *a = s_list[i];
            const char *nm = a ? a->label : "?";
            const char *id = a ? a->id : "";
            am_draw_text_l(lcd, grid_x0 + 12, y + 6, nm, sel);
            am_draw_text_l(lcd, grid_x0 + 12, y + 27, id, sel);
            bool mm = a && os_app_visible(a);
            const char *st = mm ? "\xe5\x9c\xa8\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95" : "\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95";
            am_draw_text_l(lcd, UI_SCREEN_W - 58, y + 14, st, sel);
        }
    }

    /* 视图切换图标已移除: 改为 长按硬件确认键 3s 切换 (见 p_am_poll/OS_ACTION_BT_SEARCH). */

    if (s_list_n == 0) {
        draw_text(lcd, grid_x0 + 40, content_top + 60,
                  "\xe8\xaf\xa5\xe5\x88\x86\xe7\xb1\xbb\xe6\x9a\x82\xe6\x97\xa0\xe5\xba\x94\xe7\x94\xa8", false);
        /* 该分类暂无应用 */
    }

    /* 左侧分类栏 (引擎/游戏/应用/运维: 4px 等距排布, 仅图标).
     * V1.6.x 选中态 = 从头到尾仅一根 2px 水管线:
     *   右侧一条竖线从上贯穿到底 (形成"完善分割"), 选中行这一段让位改走包围框.
     *   选中行: 右侧进来 → 往左(顶部) → 往下(左边) → 往右(底部) → 回到右侧 → 继续直下到底.
     *   其余未选中行: 右侧就是直的 2px 竖条.
     *   路径依选中行位置而变 (顶部/中间/底部), 拐角先按直线直角处理. */
    int sx  = AM_CATBAR_W;                        /* 右侧分割线列 (2px: sx, sx+1 = 右缘, 右移 2px) */
    int rx  = AM_CATBAR_W + 1;                   /* 右侧分割线右列 (右移 2px) */
    int sy0 = AM_CAT_Y0 + AM_ICON_GAP;                /* 最上图标顶 (状态栏下方) */
    int sy1 = am_cat_cy(AM_CAT_COUNT - 1) + AM_ICON_SIZE / 2; /* 最下图标底 */

    /* 选中行包围框几何 (上/左/下 三边, 开口朝右, 让位 2px 空隙) */
    int cy  = am_cat_cy(s_cat);
    int it  = cy - AM_ICON_SIZE / 2;                  /* 选中图标顶 */
    int ib  = cy + AM_ICON_SIZE / 2;                  /* 选中图标底 */
    int lx  = AM_ICON_X - AM_ICON_SIZE / 2 - AM_SEL_GAP - AM_SEL_W; /* 左竖 x0 */
    int ty  = it - AM_SEL_GAP - AM_SEL_W;             /* 上横 y0 */
    int by  = ib + AM_SEL_GAP;                        /* 下横 y0 */
    int w1  = AM_SEL_W - 1;

    /* 分类图标先铺底 (四类均用独立 cat 图标) */
    for (int r = 0; r < AM_CAT_COUNT; r++)
        draw_cat_icon_stretched(lcd, AM_ICON_X, am_cat_cy(r), AM_ICON_SIZE, AM_ICON_SIZE, r);

    /* ==== 一根 2px 水管线 ==== */
    int R = AM_SEL_R;                          /* 拐角外凸圆弧 R3 */
    bool is_top = (s_cat == 0);
    bool is_bot = (s_cat == AM_CAT_COUNT - 1);

    /* 右缘分隔线: 选中行让位 (砍掉 [ty, by+w1] 这段), 上方段 + 下方段贯穿.
     * 上下两段在拐点 (rx,ty)/(rx,by+w1) 处让位 R, 由右上/右下外凸弧连接.
     * 顶部项上方无 (直接对接状态栏), 底部项下方无 (直接到底). */
    if (ty - R > sy0) { draw_vline(lcd, sx,  sy0,   ty - R - 1, ST7305_COLOR_BLACK);
                        draw_vline(lcd, sx+1, sy0,  ty - R - 1, ST7305_COLOR_BLACK); }
    if (by + w1 + R < sy1) { draw_vline(lcd, sx,   by + w1 + R + 1, sy1, ST7305_COLOR_BLACK);
                             draw_vline(lcd, sx+1, by + w1 + R + 1, sy1, ST7305_COLOR_BLACK); }

    /* 选中行包围框 (左竖 + 上横 + 下横, 均 2px), 四角让位 R, 由外凸弧连接.
     * 顶部项不画上横+上两角 (右缘直碰状态栏); 底部项不画下横+下两角 (右缘直到底). */
    int lty0 = is_top ? ty        : ty + R;     /* 左竖上端: 顶部项平头, 否则让 R 给左上角 */
    int lby1 = is_bot ? sy1       : by + w1 - R; /* 左竖下端: 底部项延伸到屏幕底部, 否则让 R 给左下角 */
    draw_vline(lcd, lx, lty0, lby1,     ST7305_COLOR_BLACK);   /* 左竖 */
    draw_vline(lcd, lx + 1, lty0, lby1, ST7305_COLOR_BLACK);
    if (!is_top) { /* 上横 (顶部项无) */
        draw_hline(lcd, lx + w1 + R, rx - R, ty,    ST7305_COLOR_BLACK);
        draw_hline(lcd, lx + w1 + R, rx - R, ty + 1, ST7305_COLOR_BLACK);
    }
    if (!is_bot) { /* 下横 (底部项无) */
        draw_hline(lcd, lx + w1 + R, rx - R, by,    ST7305_COLOR_BLACK);
        draw_hline(lcd, lx + w1 + R, rx - R, by + 1, ST7305_COLOR_BLACK);
    }

    /* 四角 R3 外凸圆弧弯头 (接触分割线的是右上/右下, 用 kind3/kind4) */
    if (!is_top) { /* 上两角: 仅非顶部项 */
        am_elbow(lcd, lx, ty, R, 1, ST7305_COLOR_BLACK);   /* 左上: 外凸向左上 */
        am_elbow(lcd, rx, ty, R, 4, ST7305_COLOR_BLACK);   /* 右上: 外凸向右下 */
    }
    if (!is_bot) { /* 下两角: 仅非底部项 */
        am_elbow(lcd, lx, by + w1, R, 2, ST7305_COLOR_BLACK);  /* 左下: 外凸向左下 */
        am_elbow(lcd, rx, by + w1, R, 3, ST7305_COLOR_BLACK);  /* 右下: 外凸向右上 */
    }
}

/* ==== 交互 ==== */
/* 是否有顶部子分类横排 (仅在 程序/商店 分类) */
static bool am_has_sub(void) { return (s_cat == AM_CAT_APP) || (s_cat == AM_CAT_STORE); }

static void p_am_action(ui_ctx_t *ctx, os_action_t a) {
    /* 左侧分类栏聚焦 (UP/DOWN 切分类; RIGHT/确认 选定回内容; LEFT/BACK 回内容) */
    if (s_focus_bar == 2) {
        switch (a) {
        case OS_ACTION_UP:   s_cat = (s_cat + AM_CAT_COUNT - 1) % AM_CAT_COUNT; am_rebuild(); break;
        case OS_ACTION_DOWN: s_cat = (s_cat + 1) % AM_CAT_COUNT; am_rebuild(); break;
        case OS_ACTION_RIGHT:
        case OS_ACTION_CONFIRM:
        case OS_ACTION_BACK:
        case OS_ACTION_LEFT:
        case OS_ACTION_HOME:
            s_focus_bar = 0; break;
        default: break;
        }
        ctx->needs_redraw = true;
        return;
    }
    /* 顶部子分类横排聚焦 (LEFT/RIGHT 切子分类; DOWN/确认 选定回内容; UP 不继续向上=停在此层) */
    if (s_focus_bar == 1) {
        switch (a) {
        case OS_ACTION_LEFT:  s_sub = (s_sub + AM_SUB_COUNT - 1) % AM_SUB_COUNT; am_rebuild(); break;
        case OS_ACTION_RIGHT: s_sub = (s_sub + 1) % AM_SUB_COUNT; am_rebuild(); break;
        case OS_ACTION_DOWN:
        case OS_ACTION_CONFIRM:
            s_focus_bar = 0; break;                 /* 选定子分类 → 回内容 */
        case OS_ACTION_UP:
            break;                                  /* 子分类已到顶: 不再继续向上 */
        case OS_ACTION_BACK:
            s_focus_bar = 0; break;
        default: break;
        }
        ctx->needs_redraw = true;
        return;
    }
    switch (a) {
    case OS_ACTION_UP:
        if (s_view_grid) {
            if (s_focus >= AM_GRID_COLS) {
                s_focus -= AM_GRID_COLS;
                am_gcam_animate(s_focus / AM_GRID_COLS, 300);
            } else {
                s_focus_bar = 0;   /* 到顶 → 停在内容 (已无顶部子分类条) */
            }
        } else {
            if (s_focus > 0) { s_focus--; am_gcam_animate(s_focus, 260); }
            else s_focus_bar = 0;   /* 到顶 → 停在内容 (已无顶部子分类条) */
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
        /* 内容已到最左列 → 跳到左侧分类栏 */
        if (s_view_grid ? (s_focus % AM_GRID_COLS == 0) : (s_focus == 0)) {
            s_focus_bar = 2;
            ctx->needs_redraw = true;
            break;
        }
        s_focus--;
        if (s_view_grid) am_gcam_animate(s_focus / AM_GRID_COLS, 260);
        else am_gcam_animate(s_focus, 260);
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
        {
            const os_app_t *a = s_list[s_focus];
            if (a) os_push(ctx, a->page);   /* 直接打开应用, 不再弹二级操作菜单 */
        }
        break;
    case OS_ACTION_BT_SEARCH:
        /* 确认键已长按 2s (os_core 已为应用管家拦截蓝牙搜索):
         * 以此刻为起点, 再按住满 1s (累计 3s) 切换 网格/列表 视图, 无提示. */
        s_vt_armed = true;
        s_vt_t0    = esp_timer_get_time() / 1000;
        s_vt_fired = false;
        ctx->needs_redraw = true;
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
    /* 长按已触发: 吞掉松手后的那次 tap (避免重复弹操作窗) */
    if (s_lp_suppress) { s_lp_suppress = false; return true; }
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
            s_focus_bar = 0;   /* 触摸点分类后回到内容聚焦 */
            am_rebuild();
            ctx->needs_redraw = true;
        }
        return true;
    }
    /* 顶部状态栏区域: 视图切换图标已移除 (改长按确认键3s), 顶部触摸不再切换 */
    if (y >= AM_CAT_Y0 && y < AM_CAT_Y0 + AM_SUB_H && (s_cat == AM_CAT_APP || s_cat == AM_CAT_STORE)) {
        return true;
    }
    /* 右栏内容 (y < content_top: 顶部状态栏/子分类条区域 → 不触发下方图标) */
    int content_top = AM_CAT_Y0;
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
            vibrator_click();
            const os_app_t *a = s_list[idx]; if (a) os_push(ctx, a->page);  /* 触屏: 点一次直接打开 */
            ctx->needs_redraw = true;
            return true;
        }
    } else {
        int row_h = 46;
        int idx = (int)floorf(s_gcam) + (y - content_top) / row_h;
        if (idx >= 0 && idx < s_list_n) {
            vibrator_click();
            const os_app_t *a = s_list[idx]; if (a) os_push(ctx, a->page);  /* 触屏: 点一次直接打开 */
            ctx->needs_redraw = true;
            return true;
        }
    }
    return false;
}

/* ==== 长按桌面快捷方式确认 ==== */
static void am_lp_confirm_cb(ui_ctx_t *ctx, int result, void *ud) {
    const os_app_t *a = (const os_app_t *)ud;
    if (!a) return;
    if (result != 0) return;   /* 取消: 弹窗由框架自动关 */
    bool vis = os_app_visible(a);
    if (vis) {
        /* 移出主菜单: 显式加入隐藏列表, 覆盖 reveal/隐藏开关 的"强制显示",
         * 否则引擎等 reveal 应用删图标后仍留在主菜单/圆圈不消失 (冲突根因). */
        os_app_set_mainmenu(a->id, false);
        os_app_set_hidden(a->id, true);
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x88\xa0\xe9\x99\xa4\xe6\xa1\x8c\xe9\x9d\xa2\xe5\x9b\xbe\xe6\xa0\x87"); /* 已删除桌面图标 */
    } else {
        os_app_set_mainmenu(a->id, true);
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe6\xb7\xbb\xe5\x8a\xa0\xe6\xa1\x8c\xe9\x9d\xa2\xe5\x9b\xbe\xe6\xa0\x87"); /* 已添加桌面图标 */
    }
    /* 确定后必须立即关闭确认弹窗: os_dialog_toast 会置 s_cb_handled 阻止 dlg_finish_top
     * 自动关, 这里显式弹掉 (否则确认框一直卡着). 之后由 os_enter 重建可见列表. */
    os_dialog_pop(ctx);
    ctx->needs_redraw = true;
}
/* 按住图标 3s → 弹确认 (已添加到桌面则问删除, 否则问添加). 确认后自动关闭并弹 0.5s 提示 */
static void am_long_press(ui_ctx_t *ctx, int idx) {
    if (idx < 0 || idx >= s_list_n) return;
    if (!s_list[idx]) return;   /* 仅已安装应用图标 */
    const os_app_t *a = s_list[idx];
    bool mm = os_app_visible(a);
    const char *msg = mm ? "\xe5\x88\xa0\xe9\x99\xa4\xe6\xa1\x8c\xe9\x9d\xa2\xe5\x9b\xbe\xe6\xa0\x87?"     /* 删除桌面图标? */
                         : "\xe6\xb7\xbb\xe5\x8a\xa0\xe6\xa1\x8c\xe9\x9d\xa2\xe5\x9b\xbe\xe6\xa0\x87?";   /* 添加桌面图标? */
    os_dialog_confirm_ex(ctx, msg, 0, 0, am_lp_confirm_cb, (void *)a);
}

/* 触摸点 → 当前视图条目索引 (网格/列表) */
static int am_idx_at(int tx, int ty, int content_top) {
    if (tx < AM_CATBAR_W || ty < content_top) return -1;
    if (s_view_grid) {
        int cell_w = (UI_SCREEN_W - AM_CATBAR_W - 8) / AM_GRID_COLS;
        int step_y = (UI_SCREEN_H - content_top - 8) / AM_GRID_ROWS;
        if (step_y < AM_ICON_SIZE + 18) step_y = AM_ICON_SIZE + 18;
        int col = (tx - (AM_CATBAR_W + 4)) / cell_w;
        int row_vis = (ty - (content_top + 2)) / step_y;
        if (col < 0 || col >= AM_GRID_COLS || row_vis < 0) return -1;
        int row = (int)floorf(s_gcam) + row_vis;
        int idx = row * AM_GRID_COLS + col;
        return (idx >= 0 && idx < s_list_n) ? idx : -1;
    } else {
        int idx = (int)floorf(s_gcam) + (ty - content_top) / 46;
        return (idx >= 0 && idx < s_list_n) ? idx : -1;
    }
}

/* ==== 每帧轮询: 触摸上下拖动 (相机跟手 + 惯性甩动 + 网格吸附, 仿主菜单) ==== */
static void p_am_poll(ui_ctx_t *ctx) {
    /* 视图切换: 确认键已按满 3s (BT_SEARCH 起点再加 1s) 且未松手 → 切换 网格/列表 */
    if (s_vt_armed && !s_vt_fired) {
        if (input_is_held(1)) {   /* KEY = 硬件确认键仍在按住 */
            if ((esp_timer_get_time() / 1000 - s_vt_t0) >= 1000) {
                s_vt_armed  = false;
                s_vt_fired  = true;
                s_view_grid = !s_view_grid;
                s_focus = 0; s_scroll = 0;
                am_rebuild();
                ctx->needs_redraw = true;
            }
        } else {
            s_vt_armed = false;   /* 中途松手: 取消 */
        }
    }

    /* 相机动画进行中: 每帧请求重绘 */
    if (s_gcam_anim_start > 0) {
        am_gcam_tick();
        ctx->needs_redraw = true;
    }
    if (s_list_n <= 0) return;

    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    int content_top = AM_CAT_Y0;
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
        s_lp_idx = -1;
        s_lp_fired = false;
        return;
    }

    /* 只在右栏内容区开始拖动 */
    if (tx < AM_CATBAR_W || ty < content_top || ty >= UI_SCREEN_H) {
        s_gdrag_last_y = -1;
        s_lp_idx = -1;
        return;
    }

    if (s_gdrag_last_y < 0) {
        s_gdrag_last_y  = ty;
        s_gdrag_anchor_y = ty;
        s_gdrag_base     = s_gcam;
        s_gdrag_active   = false;
        s_gcam_anim_start = 0;   /* 新拖动取消旧动画 */
        s_lp_idx   = am_idx_at(tx, ty, content_top);
        s_lp_t0    = am_now_ms();
        s_lp_fired = false;
        return;
    }
    s_gdrag_last_y = ty;

    /* 位移过大 → 判为拖动, 取消长按意图 */
    int moved = ty - s_gdrag_anchor_y;
    if (moved < 0) moved = -moved;
    if (moved >= AM_DRAG_THRESH) { s_gdrag_active = true; }
    if (moved >= AM_LP_DRAG_MAX) { s_lp_idx = -1; s_lp_fired = false; }
    else if (s_lp_idx >= 0 && !s_lp_fired && !s_gdrag_active) {
        /* 按住 3s 未放手即触发 */
        if (am_now_ms() - s_lp_t0 >= AM_LP_MS) {
            s_lp_fired = true;
            s_lp_suppress = true;
            am_long_press(ctx, s_lp_idx);
            return;
        }
    }

    /* 跟手: 相机 = 基点 - 位移/行高 */
    s_gcam = s_gdrag_base - (float)(ty - s_gdrag_anchor_y) / (float)row_h;
    /* 边界 */
    if (s_gcam < 0) s_gcam = 0;
    if (s_gcam > max_row) s_gcam = (float)max_row;
    if (s_gdrag_active) ctx->needs_redraw = true;
}

/* ==== 模块 ==== */
static void p_am_enter(ui_ctx_t *ctx) {
    /* 应用管家 = 主菜单级引擎生命周期边界: 从任意引擎/软件页 os_pop 回应用管家时,
     * 统一卸载所有已加载引擎的后台 (PSRAM/字体缓冲), 防止引擎内存泄漏到应用管家.
     * - unload_all 幂等 (各引擎 unload 均带 guard/null 保护), 即使 gb/vpet 在 select
     *   内已直接 unload 也不会双重释放; 未加载引擎时为 no-op.
     * - do_leak_check=false: 应用管家自身常驻 UI 缓冲使空闲低于开机基线, 此处比对
     *   会误报 [LEAK]; 真正的泄漏复核由主菜单返回路径 (os_mgr) 的 unload_all(true) 兜底. */
    engine_manager_unload_all(false);
    /* 不再无条件重置分类/子分类/焦点: 从应用返回时 os_pop 会再次触发 on_enter,
     * 保留 static 状态即可回到"原分类原选项" (与主菜单返回保持位置的既定行为一致).
     * 仅当状态非法时由 am_rebuild 内部兜底复位.
     * 用 am_rebuild_keep: 连滚动位置一起保留 —— 触摸点开第二排的应用再退出时,
     * 画面停在打开前那一屏 (第一排仍在), 不会把第二排顶到第一行. */
    am_rebuild_keep();
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