#ifndef INPUT_H
#define INPUT_H

/* 输入动作码 (从旧 menu_system.h 迁出, 归位到 input 子系统: 由本模块产生).
 * 语义与 os.h 的 os_action_t 完全一致, os 层直接强转复用. */
typedef enum {
    MENU_ACTION_NONE = 0,
    MENU_ACTION_UP,
    MENU_ACTION_DOWN,
    MENU_ACTION_LEFT,
    MENU_ACTION_RIGHT,
    MENU_ACTION_CONFIRM,
    MENU_ACTION_BACK,
    MENU_ACTION_HOME,    /* 退出到桌面 (手柄映射第 7 键) */
    MENU_ACTION_LONG_LEFT,  /* LEFT 长按 500ms (手柄配置快捷键) */
    MENU_ACTION_LONG_PRESS, /* 触摸长按 (游戏列表收藏等) */
    MENU_ACTION_KEY_FAV,    /* 物理 KEY 长按 = 收藏 */
    MENU_ACTION_POWER_LOCK,    /* 软关机键点按 = 锁屏休眠 */
    MENU_ACTION_POWER_HINT,    /* 软关机键 0.5s = 提示 */
    MENU_ACTION_POWER_RELEASE, /* 软关机键 0.5s 后松手 = 返回 */
    MENU_ACTION_BT_SEARCH,     /* 硬件 KEY(确认) 长按 2s = 进入蓝牙搜索 */
} menu_action_t;

#include "touch_panel.h"   /* V1.0.xx: tp_point_t (多点触控) */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void input_init(void);
menu_action_t input_get_action(void);

/* V1.0.69: 本机是否带触摸屏 (两套硬件运行时区分).
 * 供 menu_system 判断物理 KEY 长按=收藏 (有触摸) 还是保持边沿收藏 (无触摸). */
bool input_has_touch(void);

/* V1.0.41: 按键映射期间禁用手柄导航键, 防止映射"返回"键时产生 MENU_ACTION_BACK
 * 终止映射流程. 默认 true, 映射启动时设 false, 结束时设 true. */
void input_set_gamepad_nav_enabled(bool enabled);

/* V1.2.x: 游戏模式标志. 只有真正进入游戏(引擎运行循环)时才置 true.
 * 用于隔离"导航关闭时仍常开手柄返回/退出键"的检测: nav 关闭既有"游戏中"(本标志
 * 生效, 允许返回键触发退出确认) 也有"按键映射中"(本标志为 false, 返回键照常被
 * 当作被映射的键捕获, 绝不误触发退出). 默认 false, 进游戏置 true, 退出置 false. */
void input_set_gamepad_gamemode(bool enabled);

/* V1.2.x: 查询当前是否处于游戏模式 (引擎运行循环已置 gamemode).
 * 供震动子系统区分"游戏内按键"(走 joypad 边沿游戏震动) 与"UI 导航"(走 UI 震动). */
bool input_is_game_mode(void);

/* V1.3.x: 游戏模式变化回调. 引擎进出游戏 (input_set_gamepad_gamemode 变化) 时立即通知,
 * 供"暗黑UI模式"(除游戏外反色) 等按游戏态切换的机制使用 — 游戏运行是阻塞主循环的,
 * 普通每帧 service tick 在游戏内不会执行, 必须靠此回调即时同步. */
typedef void (*input_gamemode_cb_t)(bool enabled);
void input_set_gamemode_cb(input_gamemode_cb_t cb);

/* 全局活动标记: 游戏 joypad 有任意键按下/按住时由 input 层置位, 供 main 主循环
 * 刷新休眠计时 (游戏输入不经 os 物理键/触摸, 否则会被误判空闲提前休眠).
 * input_mark_activity() 由输入层调用; input_consume_activity() 由 main 每帧消费. */
void input_mark_activity(void);
bool input_consume_activity(void);
/* V1.5.x: 统一"最近输入"毫秒时间戳 — 由任意输入(手柄/物理键/触摸/joypad)刷新.
 * 桌面主循环与阻塞游戏循环共同据此判定休眠, 口径一致不漂移. 0 表示从未有输入. */
uint32_t input_last_any_ms(void);

/* V1.5.x: 休眠(屏保)禁触屏开关 — 屏保激活时置 true, 触摸被完全屏蔽:
 * 不刷新统一输入时钟, 不产生 tap/touch_pos/touch_action. 这样休眠时碰触屏
 * (触摸芯片仍工作) 不会刷新 s_last_any_ms 触发误唤醒. 由 wallpapers 屏保
 * 进入(enable) / 退出(disable)时调用. 物理键/手柄不受影响, 仍可唤醒. */
void input_set_touch_blocked(bool block);
bool input_touch_blocked(void);

/* 统一"选择式 UI"输入方案: true=触屏优先(默认不高亮/点哪算哪),
 * false=手柄(默认高亮一项+方向键). 所有列表/弹窗/键盘模板都应查询它. */
bool input_scheme_touch(void);

/* 当前是否按住指定按键 (持续电平检测, 不消费任何状态机事件)
 * idx: 0 = KEY (GPIO18), 1 = BOOT (GPIO0). 返回 true 表示正在按住. */
bool input_is_held(int idx);

/* V1.0.65: 消费最近一次触摸"点击"的屏幕坐标 (400x300).
 * 返回 true 表示有未消费的点击 (坐标写入 x 和 y, 可为 NULL), 取走后清零. */
bool input_consume_tap(int *x, int *y);

/* V1.0.66: 返回当前触摸的实时屏幕坐标 (400x300), 供主菜单跟手拖动.
 * 返回 false 表示当前无手指按下. */
bool input_get_touch_pos(int *x, int *y);

/* V1.7.x: 无副作用"当前按住坐标"查询 (只读, 不消费状态机).
 * 供"按住反黑"等按压反馈使用 — main.c 每帧已消费过一次 input_get_touch_pos,
 * 弹窗/页面 render 再调会破坏其 s_o_armed 锁存, 导致按压反馈失效. */
bool input_touch_now(int *x, int *y);

/* V1.1.x: 多点触控 — 返回当前触摸点 (最多 TP_MAX_POINTS=5 点, 已映射屏幕坐标).
 * pts[0..n-1] 各点 (未按下的置 pressed=false), count 返回实际点数 (0..TP_MAX_POINTS).
 * 调用方需在 input_get_action/input_get_touch_pos 同帧读取 (依赖同一触摸缓存). */
void input_get_touch_multi(tp_point_t pts[TP_MAX_POINTS], int *count);

/* V1.0.9x: 返回本次手势按下的起点屏幕坐标 (供主菜单拖动排序判定"是否已移动").
 * 未按住或无手势时返回 false. 必须与 input_get_touch_pos 同帧/之后调用. */
bool input_touch_start_pos(int *x, int *y);

/* V1.0.99: 只读查询"当前手指按下 且 本次手势起点落在底部屏蔽带" (不消费任何状态).
 * 仅供 main.c 的 touch_shield_blocks_drag 使用: 它复用 input_get_touch_pos 已维护的
 * 锁存结果, 必须在同帧 input_get_touch_pos 调用之后使用, 避免对 input_get_touch_pos
 * 二次调用导致锁存状态被两次消费而残留 (拖动全面失效的竞态). */
bool input_touch_in_bottom_zone(void);

/* V1.0.68: 只轮询一次触摸芯片并刷新缓存坐标 (不跑手势/动作状态机).
 * 供游戏内虚拟按键等每帧需要触摸坐标、但又不希望产生菜单动作的场景
 * (如 BBK 模拟器游戏循环本身不调用 input_get_action). 同一 tick 内重复
 * 调用会被去重, 不会与 input_get_action 抢读 GT911 状态寄存器. */
void input_poll_touch(void);

/* V1.1.0: 开关底部上滑=返回手势 (白板等全屏绘图页屏蔽, 避免误退) */
void input_set_swipe_back(bool enable);

/* 阅读器横滑翻页开关: 仅阅读器打开时启用, 横向滑动 -> LEFT/RIGHT 翻页 */
void input_set_swipe_turn(bool enable);

/* V1.4.x: 上下文屏蔽按键模拟震动 (按下即震 / 按住>0.5s松手补震).
 * 主菜单/软件管家页在 os_tick 每帧置 true (拖动图标/管理列表不咔哒);
 * 弹窗/二级页面/软件程序置 false (保持触控按键手感). 由 os_core 驱动. */
void input_set_key_sim_ctx_block(bool block);

/* 引擎优化开关 (page_engine 调用): 控制引擎游戏模式下按键点击音 / 游戏操作震动.
 * 仅影响游戏模式(joypad 上升沿), 不影响 UI 导航/按键模拟震动. 默认开. */
void input_set_engine_sound(bool on);
void input_set_engine_vib(bool on);

/* V1.0.95: 返回当前触摸按住时长(ms); 未按住返回 0.
 * 供壁纸屏保"触摸需长按 1 秒才退出"判定. */
uint32_t input_touch_hold_ms(void);

/* V1.0.68: 只轮询触摸手势并返回 action (不处理物理键/手柄导航).
 * 供游戏内循环每帧检测"状态栏长按 3s → HOME", 不干扰游戏自身的手柄/物理键输入.
 * 内部会刷新触摸缓存坐标 (等价 input_poll_touch + 手势状态机). */
menu_action_t input_get_touch_action(void);
/* V1.0.68: 最近一次 input_get_action 返回的动作是否来自触摸 (底部上滑等).
 * 用于确认框区分"触摸上滑(BACK)"与"物理 BACK 键". */
bool input_touch_last_action(void);

/* V1.0.68: 软关机键 (GPIO1) 长按 2 秒软关机轮询. 每帧调用; 返回 true 表示
 * 已检测到"长按 2 秒并松手", 调用方应立即进入 deep sleep (软关机).
 * 短按=确认, 0.5s=返回主菜单 由 input_get_action 统一投递; 关机后按下 GPIO1 唤醒. */
bool input_power_should_sleep(void);

/* V1.0.68: 设置屏幕旋转方向 (电子书竖屏时触摸跟随旋转).
 * rot: 0=横屏 1=180° 2=左90°竖屏 3=右90°竖屏. */
void input_set_screen_rotation(int rot);

/* V1.0.90: 判断逻辑坐标是否落在"物理屏幕底部中间"区域 (跟随当前旋转方向).
 * 用于: 底部上滑=返回 / 拖动屏蔽带. thick 为厚度(逻辑像素). */
bool input_in_bottom_zone(int sx, int sy, int thick);

/* GB/GBC joypad 掩码 (低电平有效), 供 GB/GBC 模拟器每帧查询按键状态.
 * bit0=A bit1=B bit2=Select bit3=Start bit4=右 bit5=左 bit6=上 bit7=下
 * 来源: 手柄逻辑键 (经按键映射) + 设备物理键 (KEY=A, BOOT=B). */
uint8_t input_get_held_gb_joypad(void);

#ifdef __cplusplus
}
#endif

#endif
