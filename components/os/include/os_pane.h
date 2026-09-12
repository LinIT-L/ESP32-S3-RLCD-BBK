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

/* 右栏单分类最多预渲染的游戏/内容数 (PSRAM 大, 确保"跟手拖动能选到全部") */
#define OS_PANE_ITEM_MAX 256

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
    /* 可选: 左栏分类扫描时排除的文件夹 (如文曲星 Lava 数据目录不显示为分类). 返回 true=排除. */
    bool        (*exclude_dir)(const char *name);
    os_pane_select_cb   on_select;   /* 右栏确认回调 */
    const char         *all_label;   /* 左栏第一项, NULL="全部" */
    int                 list_y;      /* 列表起点 y (0=默认48; 无状态栏的页设小值) */
    int                 list_bottom; /* 列表底缘 y (0=默认 ui_screen_h()-4; 底部需留操作行时设小值) */
    int                 left_w;      /* 左栏宽度 (0=默认 100; 需收缩时设小值, 如 66) */
    int                 right_gap;   /* 右缘水管分割线右移 px (仅本页生效, 0=默认不动, 不影响其它 os_pane 页面) */

    /* ==== 内存数据源模式 (可选): 设置后 os_pane 不自扫文件系统, 由回调填充 ====
     * 供内存数据页面 (如密钥管理器分类/条目) 复用同一套双栏渲染/交互模板.
     * mem_folders: 填充 p->folders[] (含"全部"等自定义左栏项) 与 p->folder_count,
     *              返回 folder_count. 内部应自行设 p->hide_all (内存模式通常 true,
     *              把"全部"作为 folders[0], 避免叠加 os_pane 自动"全部"). */
    int  (*mem_items)(os_pane_t *p);     /* 按当前 p->sel_folder 填充 p->items[]/s_pane索引, 返回 item_count */
    int  (*mem_folders)(os_pane_t *p);   /* 填充 p->folders[] 与 p->folder_count, 返回 folder_count */

    /* ==== 引擎优化/设置首项 (3.3 游戏页: 左栏最上方一项, 右栏内联显示设置, 不弹窗) ==== */
    const char         *settings_label;          /* 左栏首项标签 (如"引擎优化"), NULL=无 */
    int                 settings_count;          /* 右栏设置项数 */
    void (*settings_render)(ui_ctx_t *ctx, os_pane_t *p, int rx0, int rx1,
                            int list_y, int list_bottom, int top_row, int off_mod); /* 画右栏设置行 (含平滑滚动偏移) */
    os_pane_settings_cb  on_settings_select;     /* 点选/确认某设置项 (直接切换值连 API) */

    /* ==== 收藏栏 (可选, fav_enabled 时启用) ====
     * 左栏在"引擎优化"后插入"收藏栏"虚拟项; 右栏显示该引擎的收藏游戏.
     * 右栏长按(约1s): 正常列表=收藏当前项, 收藏栏=取消收藏当前项. */
    int  fav_engine;             /* 收藏引擎 id (favorites.h) */
    bool fav_enabled;            /* true=本页启用收藏栏 */
    bool fav_mode;               /* 内部: 当前右栏显示收藏栏 */
    const char *fav_empty_tip;   /* 可选: 收藏栏为空时的提示文案 (NULL=默认"长按确认游戏加入收藏") */

    /* ==== 最近阅读入口 (可选, recent_label 非 NULL 时启用) ====
     * 左栏在"阅读优化"后、"收藏栏"前插入"最近阅读"固定项.
     * 右栏显示最近打开的书 (recent_path), 选中即打开并恢复进度.
     * 用于 TF 卡损坏/换卡后的设备级兜底 (NVS 记录最近阅读). */
    const char *recent_label;    /* 左栏标签 (如"最近阅读"), NULL=无 */
    char        recent_path[160];/* 最近阅读的书完整路径 (调用方填) */
    bool        recent_mode;     /* 内部: 当前右栏显示最近阅读 */

    /* ==== 内部状态 (调用方勿直接改, 结构体静态分配) ==== */
    char  folders[24][64];
    int   folder_count;
    char  items[OS_PANE_ITEM_MAX][64];        /* 显示名 (去扩展名) */
    char  item_paths[OS_PANE_ITEM_MAX][160];  /* 完整路径 */
    int   item_count;
    int   sel_folder;            /* 0=全部, 1..=子目录 */
    int   sel_item;
    int   folder_scroll;
    int   item_scroll;
    int   item_off;              /* 右栏平滑滚动像素偏移 (跟手拖动) */
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
/* 复用左栏模板 (文字+水管分割线+选中框): 供阅读/游戏等自包含渲染页调用, 保证左栏样式与 os_pane 页一致 */
void os_pane_draw_left(st7305_handle_t *lcd, os_pane_t *p);
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
