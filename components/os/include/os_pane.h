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

struct os_pane {
    /* ==== 调用方提供 (使用前设置) ==== */
    const char         *root;        /* 扫描根目录, 如 "/sdcard/gam" */
    bool        (*is_item)(const char *name);   /* 文件过滤 (如 .gam/.txt) */
    os_pane_select_cb   on_select;   /* 右栏确认回调 */
    const char         *all_label;   /* 左栏第一项, NULL="全部" */

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
    int   focus;                 /* 0=左栏, 1=右栏 */
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
/* 当前右栏选中项完整路径 (未选中返回 NULL) */
const char *os_pane_selected_path(os_pane_t *p);

#ifdef __cplusplus
}
#endif

#endif /* OS_PANE_H */
