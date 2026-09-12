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
    OS_ACTION_BT_SEARCH,     /* 硬件确认键长按 2s = 蓝牙搜索 */
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
    OS_PAGE_MP3_PLAYER,        /* MP3 播放器 */
    OS_PAGE_GAME_SETTINGS,     /* 游戏设置 */
    OS_PAGE_GB_GAME,           /* GB 游戏 */
    OS_PAGE_BOOK,              /* 电子书 */
    OS_PAGE_WALLPAPER,         /* 壁纸设置 */
    OS_PAGE_POMODORO,          /* 番茄钟 */
    OS_PAGE_USB_HID,           /* USB HID 键鼠 */
    OS_PAGE_APP_MANAGER,       /* 应用管家 (应用管理+商店合并) */
    OS_PAGE_STORAGE,           /* 存储管理 (3.3: 浏览/挂载/格式化/信息) */
    OS_PAGE_GBC_GAME,          /* GBC 游戏 */
    OS_PAGE_NES_GAME,          /* NES 游戏 */
    OS_PAGE_MD_GAME,           /* MD/Genesis 游戏 (Gwenesis) */
    OS_PAGE_SMS_GAME,          /* SMS/GG 游戏 (SMSPlus) */
    OS_PAGE_ARDUBOY_GAME,      /* ArduBoy 游戏 */
    OS_PAGE_DIAGNOSIS,         /* 故障诊断 */
    OS_PAGE_WQX,               /* wqx 文曲星 */
    OS_PAGE_VPET,              /* 暴龙机 */
    OS_PAGE_MESHTASIC,         /* Meshtasic 网络面板 */
    OS_PAGE_WHITEBOARD,        /* 白板 */
    OS_PAGE_TERMINAL,          /* 终端 */
    OS_PAGE_SPONSOR,           /* 全屏弹窗 (第5类模板: 全屏显示, 如收款二维码) */
    OS_PAGE_DIALOG,            /* 通用弹窗 (modal 覆盖页, 由 os_dialog 驱动) */
    OS_PAGE_USB_BT,            /* USB 蓝牙适配器 (USB-BT) */
    OS_PAGE_USB_NET,           /* USB 网卡共享 (有线/USB-RNDIS) */
    OS_PAGE_WINASSIST,         /* 运维助手 (USB 键鼠+串口+U盘复合) */
    OS_PAGE_PLACEHOLDER_SECURE,/* 占位应用: 安全密码机 (开发中) */
    OS_PAGE_PLACEHOLDER_ALARM, /* 占位应用: 闹钟 (开发中) */
    OS_PAGE_MODTOOL,           /* 修改机: 游戏内存搜索修改工具 */
    OS_PAGE_LOGDIAO,           /* 日志诊断: Ghost-Audit 集成 (目标电脑日志收集/回传) */
    OS_PAGE_CALC,              /* 计算器: 科学计算器 */
    OS_PAGE_WIFIPROBE,         /* Wi-Fi 万用表: 扫描/热力图/探针嗅探/隐藏SSID */
    OS_PAGE_MORSE,             /* 摩斯密码: 52键键盘+发声/震动+播放全部 */
    OS_PAGE_NETDECT,           /* 网络分析: 主机发现/端口服务/弱口令/报告 */
    OS_PAGE_FAV,               /* 收藏夹: 账号/密码 一键模拟键盘输入 */
    OS_PAGE_COUNT
} os_page_t;

/* 前置声明（与旧 menu_state_s 回调指针同一形态约束） */
typedef struct ui_ctx ui_ctx_t;

/* 页面模块契约 — 任何"页面/应用"都实现这组 ops ==== */
typedef void (*os_hk_fn)(void);   /* 资源管家: 瞬态资源清理回调 */
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
    /* V1.5.x: 可选"常驻热区触摸". 当前模块无 touch 且非全屏/非弹窗时兜底分发,
     * 供状态栏图标等常驻控件响应点击 (命中=返回 true). 旧服务不设则为 NULL. */
    bool (*touch)(ui_ctx_t *ctx, int x, int y);
};

/* ==== 内核公共状态（薄） ==== */
struct ui_ctx {
    st7305_handle_t *lcd;      /* 屏句柄 */
    int  current_page;         /* 当前模块 id (OS_PAGE_*) */
    bool needs_redraw;         /* 脏标记 */
    bool modal_active;         /* 当前是否为模态覆盖页 */
    bool fullscreen;           /* 运行时全屏覆盖 (模块 render 内设置, 帧末复位;
                                * 阅读器/游戏等需隐藏状态栏的应用页用) */
    /* 页面级触摸坐标映射 (可选): 如书架竖屏(左/右)与 180°(下)时, 把物理横屏坐标
     * 换算成 UI 逻辑坐标. 页面在 enter/render 时设置, 内核在分发 tap 前与拖动
     * 轮询 (os_pane_poll) 中统一应用, 弹窗层与页面层共用同一份映射 — 对应 3.3
     * 书架页内手动旋转的做法. 阅读器等自管全局 input 旋转的模块不设置此钩子. */
    void (*map_touch)(int *x, int *y);
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

/* V1.5.x: 当前所在页 id (-1=未进页). 深睡待机前保存, 唤醒后恢复"原页面". */
int os_current_page(void);

/* ==== 资源管家 (瞬态资源清理) ==== */
/* 登记一个"瞬态资源"清理回调 (去重); 由内核在退回主菜单/应用管家时统一执行. */
void os_housekeeper_add(os_hk_fn fn);
/* 立即执行已登记的全部瞬态清理 (幂等, 执行后清空登记表). */
void os_housekeeper_sync(void);
/* 当前待清理的瞬态项数量 (调试). */
int  os_housekeeper_pending(void);
/* 内存审计 (V1.6.x): 输出整机内存分类明细 + 内部/PSRAM 各类大小 + 所有任务栈水位.
 * 由 remcon `audit` 与管家联调使用, 便于定位"谁占用内部 RAM". */
void os_housekeeper_mem_audit(void);

/* 事件路由: 按当前模块 action(); 若模块不消费则处理全局 BACK/退栈. */
void os_handle_action(ui_ctx_t *ctx, os_action_t a);
void os_page_gamepad_open_scan(ui_ctx_t *ctx);   /* 长按确认键2s: 任意页进入蓝牙搜索 */

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

/* ==== 屏保 (星空壁纸) ====
 * 桌面空闲超过阈值自动全屏渲染星空 (wp_program_render), 任何输入自动唤醒.
 * 由 os_mgr 空闲检测置位/复位, os_core 渲染 & 输入唤醒. */
bool os_screensaver_active(void);
void os_screensaver_reset(void);
void os_screensaver_force_enter(void);   /* 锁屏键点按: 强制进入壁纸屏保 */

/* ==== 通用弹窗 (modal 覆盖页) ====
 * 供各页面复用统一弹窗: 列表选择 / 确认框 / 提示 toast.
 * 打开后压入 OS_PAGE_DIALOG (modal), 不退出底层页面; 关闭后自动恢复底层.
 * 回调 result: -1=取消/BACK, >=0 选中索引 (确认框: 0=确定 1=取消). */
typedef void (*os_dialog_cb_t)(ui_ctx_t *ctx, int result, void *ud);

void os_dialog_list(ui_ctx_t *ctx, const char *title,
                    const char *const *items, int count, int sel,
                    os_dialog_cb_t cb, void *ud);
void os_dialog_confirm_ex(ui_ctx_t *ctx, const char *msg, uint32_t timeout_ms,
                          int timeout_result, os_dialog_cb_t cb, void *ud); /* 超时自动完成 */
/* V1.7.x: 通用档位滚轮 (上下滑动/方向键翻档, 触摸点击文字直接选中, 底部确认/取消).
 * 回调 cb(ctx, sel): sel=选中档, -1=取消. 供档位型设置 (关机时间/字体大小/音量/暗黑等) 复用. */
void os_dialog_wheel(ui_ctx_t *ctx, const char *const *items, int n, int sel,
                     os_dialog_cb_t on_done);
/* 统一退出确认 (冻结帧直绘版): 步步高/文曲星等自驱动引擎在退出点调用.
 * 外观/交互与 os_dialog_confirm_ex 完全一致: 标题"退出程序?", 左"确定"右"取消",
 * 手柄 LEFT=确定/RIGHT=取消 黑块选择, CONFIRM=当前选中项, BACK=取消(松手守卫),
 * 触摸弹窗内左=确定 右=取消. 阻塞直到用户决定. 返回 true=确认退出. */
bool os_confirm_exit_blocking(st7305_handle_t *lcd);
void os_dialog_toast(ui_ctx_t *ctx, const char *msg);   /* 1s 自动关闭 */
void os_dialog_toast_ms(ui_ctx_t *ctx, const char *msg, uint32_t ms); /* V1.5.x: 自定义时长 toast (0=默认1s) */
void os_dialog_draw_overlay(ui_ctx_t *ctx);             /* 全局顶层 toast 覆盖: os_render flush 前调用 */
/* V1.6.x: toast 查询/清除 — os_core 在按键/触摸分发最前调用, 保证任意场景提示即时关闭 */
bool os_dialog_toast_active(void);
void os_dialog_toast_clear(void);
/* V1.7.x: 全局 toast 超时清理 — os_core 每帧调用 (任意页面/弹窗上 toast 1s 自动关+重绘) */
void os_dialog_toast_tick(ui_ctx_t *ctx);
/* V1.6.x: AP 热点激活标志 — 任一模块开热点时置位, 状态栏据此显示 NET 图标 */
void os_ap_set(bool on);
bool os_ap_active(void);

/* ==== 弹窗栈 (settings/list_dialog 嵌套弹窗支持) ====
 * os_dialog 内部维护一层弹窗栈: 开子弹窗压栈, 返回弹栈恢复父弹窗原选中位.
 * 自定义渲染/按键 (时间编辑等复杂弹窗) 通过 on_render/on_key 挂接.
 * 嵌套导航: 下一步内容先 os_dialog_pop 弹掉当前层再 os_dialog_push 压入新层 (原地换内容, 不加深层级);
 * 返回键逐层回退 (上一步), 消除"每个子项单独弹窗"的观感. */
typedef struct os_dlg_stack os_dlg_stack_t;
typedef bool (*os_dlg_render_t)(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud);
typedef bool (*os_dlg_key_t)(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud);
typedef void (*os_dlg_poll_t)(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud);   /* 每帧轮询 (按键映射捕获等) */
typedef bool (*os_dlg_touch_t)(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud); /* 可选: 自定义整层触摸(点列/按钮), true=已消费 */
struct os_dlg_stack {
    char  title[32];
    char  items[60][30];   /* 列表项 (支持数值选择等长列表, 弹窗栈 4 层共 ~7.2KB) */
    int   count;
    int   sel;
    os_dialog_cb_t cb;
    void *ud;
    os_dlg_render_t on_render;   /* 可选: 自定义整层渲染, true=已消费 */
    os_dlg_key_t    on_key;      /* 可选: 自定义按键, true=已消费 */
    os_dlg_poll_t   on_poll;     /* 可选: 每帧轮询 (当前层激活时调用) */
    os_dlg_touch_t  on_touch;    /* 可选: 自定义整层触摸(点列/按钮), true=已消费 */
    bool  no_footer;             /* true=无底部"返回"行 (如确认框: 确定/取消) */
    bool  small;                 /* true=小自适应确认弹窗 (退出等, 3.3 notice 样式); false=大窗 350×250 */
    bool  auto_h;                /* true=列表自适应高度(内容少自动收窄, 多则满高滚动); false=固定 350×250 */
    bool  row_x;                 /* true=每行内容最右侧显示删除"×", 点击 x 命中该行→弹删除确认 (长按即时同义) */
    const uint8_t *icon;         /* 可选: 确认弹窗文字区下方居中的 1bpp 位图 (如挂载/传输图标); NULL=无 */
    int   icon_w, icon_h;        /* icon 宽高 (bitmap 每行 ceil(w/8) 字节, 位=1 黑像素) */
    int   fix_w;                 /* >0: 固定弹窗宽度 (两阶段同宽; 0=按内容自适应) — 供确认框→图标窗原位变形同框用 */
    uint32_t timeout_ms;         /* >0: 打开后超时自动完成 (0=不超时) */
    int      timeout_result;     /* 超时自动回调的 result */
    uint32_t open_ms;            /* 内部: push 时记录打开时刻 */
    char  footer[12];            /* 底部固定"返回"行文字 (默认"返回"; 文件浏览用"返回"/"退出"动态) */
    int   scroll_px;             /* 列表跟手拖动: 内容相对"选中窗口"的子像素偏移 ([-DLG_LINE_H, DLG_LINE_H)) */
};
void os_dialog_push(ui_ctx_t *ctx, const os_dlg_stack_t *dlg);  /* 压入弹窗栈 (栈空自动开弹窗页); cb 内调用=下一步同窗替换 */
void os_dialog_pop(ui_ctx_t *ctx);                              /* 弹栈 (回上一层); 栈空则关闭弹窗页 */
void os_dialog_clear_all(ui_ctx_t *ctx);                        /* 关闭全部弹窗 (清空栈+关弹窗页) */
int  os_dialog_depth(void);
const os_dlg_stack_t *os_dialog_top(void);
/* 原位改写当前弹窗内容 (同一框变形, 不加深/不关闭): 返回栈顶可写句柄用于改字段. */
os_dlg_stack_t *os_dialog_top_mut(ui_ctx_t *ctx);
/* 标记回调已"接管"当前弹窗 (保持同一窗不自动关闭; 配合 top_mut 原位变形用). */
void os_dialog_mark_handled(ui_ctx_t *ctx);

/* ==== 应用注册表 (主菜单/应用管理共用; 为应用商店动态安装预留) ==== */
typedef struct os_app os_app_t;
struct os_app {
    const char *id;        /* 唯一 id, 如 "settings" / "wasm:snake" */
    const char *label;     /* 显示名 (UTF-8) */
    int         icon_idx;  /* 主菜单 SF 图标索引 */
    os_page_t   page;      /* 启动进入的页面 */
    bool        hidden;    /* true=默认不在主菜单显示 (内部页/引擎等) */
    bool        touch_only;/* true=必须触摸屏才可用 (无触摸时自动隐藏) */
    const char *cat;       /* 商店子分类: "游戏"/"程序"/"网络"/"运维"/"手册"/"学习", NULL=无分类 */
    bool        reveal;    /* true=彩蛋(赞助页5连点)揭示类: 开关打开后仅显示引擎游戏+应用管家 */
};

/* 编译期/运行期注册应用 (商店安装后运行时注册, 主菜单自动出现). */
void os_app_register(const os_app_t *app);
int  os_app_count(void);
const os_app_t *os_app_get(int idx);
/* 隐藏应用开关 (赞助页 5 连点): 打开后主菜单显示隐藏的引擎/应用管理 */
bool os_app_hidden_visible(void);
void os_app_toggle_hidden_visible(void);
bool os_app_visible(const os_app_t *a);
int  os_app_visible_count(void);
const os_app_t *os_app_visible_at(int idx);

/* 主菜单自定义管理 (应用管家): 用户可把任意应用添加到主菜单 / 移出主菜单.
 * 持久化到 NVS, 覆盖应用的默认 hidden. 返回是否"显式出现在主菜单". */
bool os_app_in_mainmenu(const char *id);
void os_app_set_mainmenu(const char *id, bool show);
/* 显式隐藏/恢复 (从主菜单彻底移除, 类似卸载的入口动作). */
bool os_app_in_hidden(const char *id);
void os_app_set_hidden(const char *id, bool hide);

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

/* ==== CPU 双核静态亲和性工具 (内核 os_core, 独立于资源管家裁剪) ====
 * 与资源管理无关, 属系统范围工具, 从旧 os_mgr 迁出归位. */
int  os_core_current_core(void);                              /* 当前运行核 (0/1) */
void os_core_pin_task(void *task_handle, int core);           /* core 0/1; -1=任意核 */

/* ==== 内建全局服务 (components/os/services 目录) ==== */

/* 状态栏: 由 os_register_all_internal() 自动注册; 此声明仅供需要手动注册/调试时使用. */
void os_svc_statusbar_register(void);
/* V1.6.x: 运行时切换状态栏反白模式 (科幻暗色页调用, 仅本页有效). */
void os_statusbar_set_invert(bool v);
/* 修改机状态栏常驻服务 (mini 图标 + 二级菜单) */
void os_svc_modtool_register(void);

#ifdef __cplusplus
}
#endif

void svc_statusbar_draw_game(st7305_handle_t *lcd, uint8_t battery, bool pad_connected);
#endif /* OS_H */