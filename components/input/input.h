#ifndef INPUT_H
#define INPUT_H

#include "menu_system.h"
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

/* 当前是否按住指定按键 (持续电平检测, 不消费任何状态机事件)
 * idx: 0 = KEY (GPIO18), 1 = BOOT (GPIO0). 返回 true 表示正在按住. */
bool input_is_held(int idx);

/* V1.0.65: 消费最近一次触摸"点击"的屏幕坐标 (400x300).
 * 返回 true 表示有未消费的点击 (坐标写入 x 和 y, 可为 NULL), 取走后清零. */
bool input_consume_tap(int *x, int *y);

/* V1.0.66: 返回当前触摸的实时屏幕坐标 (400x300), 供主菜单跟手拖动.
 * 返回 false 表示当前无手指按下. */
bool input_get_touch_pos(int *x, int *y);

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
