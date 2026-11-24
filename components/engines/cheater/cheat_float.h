#ifndef CHEAT_FLOAT_H
#define CHEAT_FLOAT_H

#include "cheater.h"
#include "st7305.h"

/* 独立"修改机浮标 + 修改机菜单任务" (引擎无关).
 *
 * 架构: 游戏与修改机彻底分开为两个任务.
 *   - 引擎仅在运行开始/结束时登记自己的游戏内存:
 *         cheat_float_attach(&region, bits);   // 开始
 *         cheat_float_detach();                // 结束
 *   - 显示合成层(与 virtual_keys_draw 同一点)每帧调用一次:
 *         cheat_float_overlay_tick(lcd);
 *     overlay_tick 仅绘制浮动图标、检测热键/点按; 一旦触发就唤醒独立修改机任务并
 *     **立即返回**(不再阻塞当前任务). 返回 true 表示本帧已触发接管, 调用方应跳过
 *     本帧的 LCD flush, 把屏幕交给修改机任务.
 *   - 独立修改机任务(prio7)被唤醒后置 active/frozen, 独占 LCD+输入, 运行
 *     cheat_ui_open(菜单/搜索)直至退出, 再交还屏幕.
 *   - 冻结: cheat_float_is_frozen() 在菜单打开期间恒 true. 各引擎模拟步进循环顶
 *     检查后停走(不推进模拟), 保证搜索读到的游戏内存稳定. 各显示路径在 active 期间
 *     停止画帧/flush, 避免覆盖修改机菜单.
 */

/* 登记当前运行游戏的内存 region(引擎自有 RAM). reg=NULL 等价 detach. */
void cheat_float_attach(const cheater_region_t *reg, cheater_bits_t bits);
/* 注销当前运行游戏(退出时调用). */
void cheat_float_detach(void);
/* 当前是否已登记一个可修改的目标. */
bool cheat_float_is_attached(void);
/* 修改机菜单打开期间返回 true. 引擎模拟任务应在循环顶检查: 冻结时停走.
 * 显示路径也据此停止画帧/flush, 让修改机任务独占屏幕. */
bool cheat_float_is_frozen(void);
/* 修改机任务独占屏幕/输入期间返回 true (同 is_frozen, 语义强调"显示让渡"). */
bool cheat_float_is_active(void);

/* 每显示合成帧调用(在 virtual_keys_draw 之后、flush 之前): 画浮标 + 检测呼出.
 * 触发接管时返回 true, 调用方应跳过本帧 flush 并将屏幕让给修改机任务. */
bool cheat_float_overlay_tick(st7305_handle_t *lcd);

#endif /* CHEAT_FLOAT_H */