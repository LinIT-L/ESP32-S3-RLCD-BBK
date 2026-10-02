#ifndef ENGINE_API_H
#define ENGINE_API_H

/* ============================================================================
 * engine_api — 引擎层唯一 API 面 (L3 引擎层门面 / facade)
 * ----------------------------------------------------------------------------
 * 目的: 把"引擎能用什么"收敛成一个入口, 让引擎与硬件/系统解耦.
 *
 * 铁律 (由 tools/check_deps.py 校验):
 *   · 引擎 (gam4980 / gb_emu / gbc_emu / nes_emu / md_emu / sms_emu /
 *           lavax / vpet / arduboy_avr / book_reader)
 *     **只允许**  #include "engine_api.h"
 *   · 不得再直接 include st7305.h / input.h / bt_manager.h / cheater.h /
 *     board_shim.h / virtual_keys.h / web_gamepad.h / audio_player.h / font_zh.h
 *   · 引擎需要的"上层动作" (退出确认弹窗 / 游戏内状态栏 / 电源事件) 一律用
 *     **回调注入** (见各引擎的 set_*_cb), 绝不反向 include os.h
 *
 * 现有引擎尚未全量迁移 (按 guard 报告逐个体检改); 新代码必须直接用本 facade.
 * ==========================================================================*/

/* ---- 显示: 1bpp 帧缓冲直写 (st7305) ---- */
#include "st7305.h"
/* ---- 绘制原语 / 图标 / 中英文字体 (ui_common + fonts) ---- */
#include "ui_common.h"
/* ---- 输入: 原始按键/触摸/手柄事件 (input) ---- */
#include "input.h"
/* ---- 屏幕虚拟按键覆盖层 ---- */
#include "virtual_keys.h"
/* ---- 引擎共用硬件适配层 (帧缓冲双缓冲/缩放/时序) ---- */
#include "board_shim.h"
/* ---- 金手指 / 内存搜索修改 (cheater) ---- */
#include "cheater.h"
/* ---- 蓝牙手柄状态 (连接/按键) ---- */
#include "bt_manager.h"
/* ---- 网页手柄 (手机当手柄, HTTP 服务) ---- */
#include "web_gamepad.h"
/* ---- 音频输出 (PCM → I2S) ---- */
#include "audio_player.h"

#endif /* ENGINE_API_H */
