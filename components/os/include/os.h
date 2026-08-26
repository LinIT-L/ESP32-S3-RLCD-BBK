/**
 * os.h — 标准化模块化 OS 内核契约.
 *
 * 设计目标:
 *  1. 无共享大结构: 每个"页面/应用"模块自持私有状态, 留在各自 .c（static/私有 struct）。
 *  2. 极薄公共上下文 ui_ctx_t: 只给内核与模块传"世界句柄"。
 *  3. 全局服务单例: 状态栏/音量/电量/蓝牙 等不占页面栈, 可任意叠加渲染。
 *  4. 纯引擎/纯组件不动: 只做接口对接（见各 *_module.c 适配层）。
 *
 * 用法:
 *   os_init(&ctx, &lcd);
 *   os_register(&some_module);        // 编译期静态注册
 *   os_register_service(&statusbar);  // 注册全局服务
 *   os_push(PAGE_MAIN);               // 进入桌面
 *   // 主循环:
 *   os_handle_action(&ctx, action);
 *   if (os_handle_touch(&ctx, x, y)) ...
 *   os_render(&ctx);
 */
#ifndef OS_H
#define OS_H

#include "st7305.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==== 动作码（语义与旧 menu_action_t 一致, 避免改 main.c/input.c/各引擎交互） ==== */
typedef enum {
    OS_ACTION_NONE = 0,
    OS_ACTION_UP,
    OS_ACTION_DOWN,
    OS_ACTION_LEFT,
    OS_ACTION_RIGHT,
    OS_ACTION_CONFIRM,
    OS_ACTION_BACK,
    OS_ACTION_HOME,        /* 退出到桌面 (手柄映射第 7 键) */
    OS_ACTION_LONG_LEFT,   /* LEFT 长按 500ms */
    OS_ACTION_LONG_PRESS,  /* 触摸长按 */
    OS_ACTION_KEY_FAV,     /* 物理 KEY 长按 = 收藏 */
    OS_ACTION_POWER_LOCK,  /* 软关机键点按 = 锁屏休眠 */
    OS_ACTION_POWER_HINT,  /* 软关机键 0.5s = 提示 */
    OS_ACTION_POWER_RELEASE, /* 软关机键 0.5s 后松手 */
} os_action_t;

/* ==== 模块/页面 id（迁移后的页面清单, 与旧 menu_page_t 语义一一对应） ==== */
typedef enum {
    OS_PAGE_MAIN = 0,          /* 主菜单桌面 */
    OS_PAGE_SELECT_GAME,       /* 选择文曲星游戏 */
    OS_PAGE_SETTINGS,          /* 设置容器 */
    OS_PAGE_GAMEPAD,           /* 手柄配置 */
    OS_PAGE_SETTINGS_DISPLAY,  /* 显示设置 */
    OS_PAGE_SETTINGS_TIME,     /* 时间设置 */
    OS_PAGE_SETTINGS_BT,       /* 蓝牙设备 */
    OS_PAGE_SETTINGS_SD,       /* TF卡管理 */
    OS_PAGE_SETTINGS_INFO,     /* 系统信息 */
    OS_PAGE_BTINFO,            /* 蓝牙设备信息 */
    OS_PAGE_KEY_CONFIG,        /* 按键设置 */
    OS_PAGE_VOLUME,            /* 音量设置 */
    OS_PAGE_RETURN_GAME,       /* 返回游戏 */
    OS_PAGE_FILE_BROWSER,      /* 文件浏览器 */
    OS_PAGE_MP3_PLAYER,        /* MP3 播放器 */
    OS_PAGE_GAME_SETTINGS,     /* 游戏设置 */
    OS_PAGE_GB_GAME,           /* GB 游戏 */
    OS_PAGE_BOOK,              /* 电子书 */
    OS_PAGE_WALLPAPER,         /* 壁纸设置 */
    OS_PAGE_POMODORO,          /* 番茄钟 */
    OS_PAGE_USB_HID,           /* USB HID 键鼠 */
    OS_PAGE_APP_MANAGER,       /* 应用管理 */
    OS_PAGE_ICON_EDITOR,       /* 图标编辑器 */
    OS_PAGE_DIAGNOSIS,         /* 故障诊断 */
    OS_PAGE_WQX,               /* wqx 文曲星 */
    OS_PAGE_VPET,              /* 暴龙机 */
    OS_PAGE_MESHTASIC,         /* Meshtasic 网络面板 */
    OS_PAGE_WHITEBOARD,        /* 白板 */
    OS_PAGE_TERMINAL,          /* 终端 */
    OS_PAGE_WIFI_KB,           /* WiFi 虚拟键盘 (终端内独立模块) */
    OS_PAGE_DIALOG,            /* 通用弹窗 (modal 覆盖页, 由 os_dialog 驱动) */
    OS_PAGE_COUNT
} os_page_t;

/* 前置声明（与旧 menu_state_s 回调指针同一形态约束） */
typedef struct ui_ctx ui_ctx_t;

/* ==== 标准模块契约 — 任何"页面/应用"都实现这组 ops ==== */
typedef struct os_module os_module_t;
struct os_module {
    const char *name;                 /* 模块名（调试用） */
    os_page_t  page_id;               /* 页面 id, 唯一 */

    void (*on_enter)(ui_ctx_t *ctx);  /* 进入: 分配资源/加载. 可能为 NULL */
    void (*on_exit)(ui_ctx_t *ctx);   /* 退出: 释放内存 + 存档. 可能为 NULL */
    void (*render)(ui_ctx_t *ctx);    /* 整页渲染 (无系统状态栏时负责全屏) */
    void (*action)(ui_ctx_t *ctx, os_action_t a);   /* 按键: 模块内部处理 */
    bool (*touch)(ui_ctx_t *ctx, int x, int y);     /* 触摸 hit-test: true=命中消费 */
    void (*poll)(ui_ctx_t *ctx);      /* 每帧后台轮询(可选). 可能为 NULL */

    bool modal;                       /* 是否模态覆盖页 (是则不重绘底层) */
    bool fullscreen;                  /* 是否全屏(隐藏状态栏); 游戏/应用页=true */
};

/* ==== 全局服务契约 — 状态栏/音量/电量 等, 不占页面栈, 可在任意页面上方叠加 ==== */
typedef struct os_service os_service_t;
struct os_service {
    const char *name;
    /* 每帧开头调用, 用于驱动内部状态(电量采样/蓝牙状态等) */
    void (*tick)(ui_ctx_t *ctx);
    /* 帧末叠加渲染(在模块 render 之后、flush 之前). 可能为 NULL */
    void (*render)(ui_ctx_t *ctx);
};

/* ==== 内核公共状态（薄） ==== */
struct ui_ctx {
    st7305_handle_t *lcd;      /* 屏句柄 */
    int  current_page;         /* 当前模块 id (OS_PAGE_*) */
    bool needs_redraw;         /* 脏标记 */
    bool modal_active;         /* 当前是否为模态覆盖页 */
    bool fullscreen;           /* 运行时全屏覆盖 (模块 render 内设置, 帧末复位;
                                * 阅读器/游戏等需隐藏状态栏的应用页用) */
    /* 内部(不应被模块直接触碰) */
    const os_module_t *cur;    /* 当前激活模块 */
    /* 模块私有状态一律放各自 .c, 不放这里 */
};

/* ==== 内核 API ==== */
void os_init(ui_ctx_t *ctx, st7305_handle_t *lcd);

/* 注册模块(编译期静态/运行期均可). 页 id 重复时后者覆盖并告警. */
void os_register(const os_module_t *mod);

/* 注册全局服务(可在任意页面叠加渲染). */
void os_register_service(const os_service_t *svc);

/* 进入页: 压栈 + 调用 on_enter. 返回 true=成功. */
bool os_push(ui_ctx_t *ctx, os_page_t page);

/* 退出当前页: 调用 on_exit + 弹栈回到记录页. */
void os_pop(ui_ctx_t *ctx);

/* 回主菜单: 弹栈到桌面(遇到 fullscreen 应用页会先走其 on_exit). */
void os_pop_to_main(ui_ctx_t *ctx);

/* 事件路由: 按当前模块 action(); 若模块不消费则处理全局 BACK/退栈. */
void os_handle_action(ui_ctx_t *ctx, os_action_t a);

/* 输入分发: 触摸 hit-test 到当前模块; 未命中返回 false (调用方可回退确认). */
bool os_handle_touch(ui_ctx_t *ctx, int x, int y);

/* 每帧: 驱动所有 service tick(). */
void os_tick(ui_ctx_t *ctx);

/* 渲染: 非模态模块 render (全屏页隐藏状态栏), 再叠加所有 service render, flush. */
void os_render(ui_ctx_t *ctx);

/* 查询当前是否停在桌面 (供蓝牙自动重连/屏保等后台逻辑判断). */
bool os_is_main(const ui_ctx_t *ctx);

/* 是否有模态弹窗/覆盖 (弹窗期间禁止背景拖动等). */
bool os_modal_active(const ui_ctx_t *ctx);

/* 当前壁纸程序 id (page_wallpaper 保存; 桌面/屏保渲染用; 0=星空默认). */
int os_wallpaper_get_program(void);

/* ==== 通用弹窗 (modal 覆盖页) ====
 * 供各页面复用统一弹窗: 列表选择 / 确认框 / 提示 toast.
 * 打开后压入 OS_PAGE_DIALOG (modal), 不退出底层页面; 关闭后自动恢复底层.
 * 回调 result: -1=取消/BACK, >=0 选中索引 (确认框: 0=确定 1=取消). */
typedef void (*os_dialog_cb_t)(ui_ctx_t *ctx, int result, void *ud);

void os_dialog_list(ui_ctx_t *ctx, const char *title,
                    const char *const *items, int count, int sel,
                    os_dialog_cb_t cb, void *ud);
void os_dialog_confirm(ui_ctx_t *ctx, const char *msg, os_dialog_cb_t cb, void *ud);
void os_dialog_toast(ui_ctx_t *ctx, const char *msg);   /* 1s 自动关闭 */

/* ==== 弹窗栈 (settings/list_dialog 嵌套弹窗支持) ====
 * os_dialog 内部维护一层弹窗栈: 开子弹窗压栈, 返回弹栈恢复父弹窗原选中位.
 * 自定义渲染/按键 (时间编辑等复杂弹窗) 通过 on_render/on_key 挂接. */
typedef struct os_dlg_stack os_dlg_stack_t;
typedef bool (*os_dlg_render_t)(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud);
typedef bool (*os_dlg_key_t)(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud);
struct os_dlg_stack {
    char  title[24];
    char  items[8][30];
    int   count;
    int   sel;
    os_dialog_cb_t cb;
    void *ud;
    os_dlg_render_t on_render;   /* 可选: 自定义整层渲染, true=已消费 */
    os_dlg_key_t    on_key;      /* 可选: 自定义按键, true=已消费 */
};
void os_dialog_push(ui_ctx_t *ctx, const os_dlg_stack_t *dlg);  /* 压入弹窗栈 (栈空自动开弹窗页) */
void os_dialog_pop(ui_ctx_t *ctx);                              /* 弹栈; 栈空则关闭弹窗页 */
int  os_dialog_depth(void);
const os_dlg_stack_t *os_dialog_top(void);

/* ==== 应用注册表 (主菜单/应用管理共用; 为应用商店动态安装预留) ==== */
typedef struct os_app os_app_t;
struct os_app {
    const char *id;        /* 唯一 id, 如 "settings" / "wasm:snake" */
    const char *label;     /* 显示名 (UTF-8) */
    int         icon_idx;  /* 主菜单 SF 图标索引 */
    os_page_t   page;      /* 启动进入的页面 */
    bool        hidden;    /* true=不在主菜单/应用管理显示 (内部页) */
};

/* 编译期/运行期注册应用 (商店安装后运行时注册, 主菜单自动出现). */
void os_app_register(const os_app_t *app);
int  os_app_count(void);
const os_app_t *os_app_get(int idx);

/* ==== 平台状态挂钩 (由外壳 main.c 注入, 解耦 os 与具体硬件组件) ====
 * 状态栏/服务不直接引用 bt_manager/wifi_manager/board_battery 等实现,
 * 而是通过这组 getter 读取系统状态, 避免 os 依赖旧 menu 巨石组件. */
typedef struct os_platform_ops {
    uint8_t (*get_battery)(void);   /* 0..100, 255=未检测到电池 */
    int     (*get_volume)(void);    /* 0..100 */
    bool    (*is_muted)(void);
    bool    (*bt_enabled)(void);
    bool    (*bt_connected)(void);
    bool    (*wifi_enabled)(void);
    bool    (*wifi_connected)(void);
    bool    (*usb_data_active)(void);
} os_platform_ops_t;

/* 注入平台状态挂钩 (可多次调用, 覆盖生效). */
void os_platform_set(const os_platform_ops_t *ops);

/* ==== 内建全局服务 (components/os/services 目录) ==== */

/* 状态栏: 由 os_register_all_internal() 自动注册; 此声明仅供需要手动注册/调试时使用. */
void os_svc_statusbar_register(void);

#ifdef __cplusplus
}
#endif

#endif /* OS_H */