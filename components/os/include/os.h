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

#ifdef __cplusplus
}
#endif

#endif /* OS_H */