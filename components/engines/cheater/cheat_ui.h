/**
 * cheat_ui.h — 修改机共享 UI (引擎无关).
 *
 * 由 gam4980_emu.c 的本地修改机抽离而来, 供所有模拟器复用同一套 UI (参考开源
 * "内存 provider(region) + 共享 UI" 范式):
 *   - 热键呼出: 手柄 SELECT+START (bt_manager, 进程全局, 任何任务可直接调);
 *   - 浮动图标呼出: 仅触摸屏且未连手柄时显示 (可选, 各引擎自行决定是否接入);
 *   - 搜索/过滤/记录/改值: 全部经 cheater 组件, 只经 region 的 peek/poke 访问
 *     引擎自有的游戏 RAM, 绝不触碰系统/其他软件内存.
 *
 * 各引擎职责: 在自身任务里检测热键/图标 → (可选加锁) → 快照屏幕 → 调
 * cheat_ui_open(lcd, region, bits). 冻结 = 阻塞在此调用期间不让模拟前进.
 */
#ifndef CHEAT_UI_H
#define CHEAT_UI_H

#include "st7305.h"
#include "cheater.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 热键检测: SELECT+START 同时按下. 任何任务的循环里调用. */
bool cheat_ui_hotkey(void);

/* 浮动图标 (触摸呼出). 仅 touch 屏且未连手柄时显示. clear()=进出游戏时调用;
 * draw()=每渲染帧调用; tick()=输入循环里调用, 返回 true 表示被点中(呼出修改机). */
void cheat_ui_float_clear(void);
void cheat_ui_float_draw(st7305_handle_t *lcd);
bool cheat_ui_float_tick(st7305_handle_t *lcd);

/* 阻塞式修改机主菜单. 快照 lcd->fb 作为冻结背景(窗外"透出"游戏画面), 用 reg/bits
 * 绑定本引擎游戏 RAM (cheater_begin), 然后搜索/过滤/记录/改值直至用户退出.
 * 调用方需已暂停模拟(本函数阻塞期间模拟不前)。 */
void cheat_ui_open(st7305_handle_t *lcd, const cheater_region_t *reg, cheater_bits_t bits);

#ifdef __cplusplus
}
#endif

#endif /* CHEAT_UI_H */