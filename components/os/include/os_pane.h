/**
 * os_pane.h — 双栏选择界面公共模板 (左目录 / 右列表).
 *
 * 从 page_select_game / page_book 抽取的重复逻辑:
 *   - 左栏: 「全部」+ 子文件夹 (扫描 root, 排序)
 *   - 右栏: 当前目录下的文件列表 (按 is_item 过滤)
 *   - 交互: 左右切栏 / 上下选行 / 确认(左=切目录, 右=回调) / 触摸点选
 * 调用方只需提供 root / is_item / on_select, 渲染与按键由本组件统一处理.
 */
#ifndef OS_PANE_H
#define OS_PANE_H

#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct os_pane os_pane_t;

/* 右栏确认回调: 打开/启动选中项 (item_path 为完整路径) */
typedef void (*os_pane_select_cb)(ui_ctx_t *ctx, os_pane_t *p,
                                  const char *item_path, const char *item_name);
/* 右栏"引擎优化/设置"项选中回调: 直接切换该设置 (连 API), 不弹窗 */
typedef void (*os_pane_settings_cb)(ui_ctx_t *ctx, os_pane_t *p, int item);

struct os_pane {
    /* ==== 调用方提供 (使用前设置) ==== */
    const char         *root;        /* 扫描根目录, 如 "/sdcard/gam" */
    bool        (*is_item)(const char *name);   /* 文件过滤 (如 .gam/.txt) */
    /* 可选: 带完整路径的过滤 (如文曲星需把含 Lava 子目录内 .lav 的游戏文件夹当一项).
     * 设置了 is_item_d 时优先用它, 否则用 is_item. */
    bool        (*is_item_d)(const char *path, const char *name);
    os_pane_select_cb   on_select;   /* 右栏确认回调 */
    const char         *all_label;   /* 左栏第一项, NULL="全部" */
    int                 list_y;      /* 列表起点 y (0=默认48; 无状态栏的页设小值) */

    /* ==== 引擎优化/设置首项 (3.3 游戏页: 左栏最上方一项, 右栏内联显示设置, 不弹窗) ==== */
    const char         *settings_label;          /* 左栏首项标签 (如"引擎优化"), NULL=无 */
    int                 settings_count;          /* 右栏设置项数 */
    void (*settings_render)(ui_ctx_t *ctx, os_pane_t *p, int rx0, int rx1, int y); /* 画右栏设置行 */
    os_pane_settings_cb  on_settings_select;     /* 点选/确认某设置项 (直接切换值连 API) */

    /* ==== 收藏栏 (可选, fav_enabled 时启用) ====
     * 左栏在"引擎优化"后插入"收藏栏"虚拟项; 右栏显示该引擎的收藏游戏.
     * 右栏长按(约1s): 正常列表=收藏当前项, 收藏栏=取消收藏当前项. */
    int  fav_engine;             /* 收藏引擎 id (favorites.h) */
    bool fav_enabled;            /* true=本页启用收藏栏 */
    bool fav_mode;               /* 内部: 当前右栏显示收藏栏 */

    /* ==== 内部状态 (调用方勿直接改, 结构体静态分配) ==== */
    char  folders[24][64];
    int   folder_count;
    char  items[128][64];        /* 显示名 (去扩展名) */
    char  item_paths[128][160];  /* 完整路径 */
    int   item_count;
    int   sel_folder;            /* 0=全部, 1..=子目录 */
    int   sel_item;
    int   folder_scroll;
    int   item_scroll;
    int   built_folder;          /* 上次 build_right 的 sel_folder, 同目录不重扫 */
    int   focus;                 /* 0=左栏, 1=右栏 */
    bool  hide_all;              /* 根目录无直接文件 → 隐藏左栏"全部"项 (自动计算) */
    bool  settings_mode;         /* 右栏当前显示"引擎优化/设置"项 */
    bool  loaded;
};

/* 首次进入 / 需要重扫目录时调用 */
void os_pane_reset(os_pane_t *p);
/* 当前选中目录下重扫列表 (切目录后调用) */
void os_pane_build_right(os_pane_t *p);
/* 渲染双栏 (调用方先 st7305_clear) */
void os_pane_render(ui_ctx_t *ctx, os_pane_t *p);
/* 按键处理: 返回 true=已消费 (LEFT/RIGHT/UP/DOWN/CONFIRM); BACK 由调用方处理 */
bool os_pane_action(ui_ctx_t *ctx, os_pane_t *p, os_action_t a);
/* 触摸点选: 左栏切目录 / 右栏回调. 返回 true=命中 */
bool os_pane_touch(ui_ctx_t *ctx, os_pane_t *p, int x, int y);
/* 每帧轮询: 右栏触摸拖动滚动 (查看更多项, 3.3 行为) */
void os_pane_poll(ui_ctx_t *ctx, os_pane_t *p);
/* 当前右栏选中项完整路径 (未选中返回 NULL) */
const char *os_pane_selected_path(os_pane_t *p);

#ifdef __cplusplus
}
#endif

#endif /* OS_PANE_H */
