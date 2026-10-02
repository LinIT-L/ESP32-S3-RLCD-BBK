#ifndef GESTURE_BACK_H
#define GESTURE_BACK_H
#include "input.h"          /* MENU_ACTION_* (与 os_action_t 同值, 直接复用) */
#include <stdbool.h>
#include <stdint.h>

/* 底部手势区厚度 (逻辑 px; 屏高 300 的底部 50px ≈ 17%). 与 gesture_back.c 同源,
 * input.c 兜底分支也用这个值判定"起点是否在底部带", 保证单一来源. */
#define GB_BAND_THICK 50

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * gesture_back — 系统导航手势识别器
 * ----------------------------------------------------------------------------
 * 把"系统导航"类触摸手势从 input.c 的大 switch 中抽出来集中管理, 阈值/方向判定
 * 单一来源, 便于调参与单测。当前包含两类系统导航手势:
 *
 *   (A) 底部上滑 = 返回 / 强制退出   (gesture_back_*)
 *   (B) 状态栏长按 = 回桌面 (HOME)  (gesture_home_*)
 *
 * 设计参照 (均为成熟实现, 非自创):
 *   - Palm webOS: 屏幕底部有专门"手势区", 从底部上滑 = 返回/回到上层; 起点必须落
 *     在手势区内 (本实现用 input_in_bottom_zone 按旋转统一换算)。
 *   - Android EdgeBackGestureHandler / GestureDetector.onFling: 双条件 =
 *       位移 ≥ 最小距离 且 速度 ≥ 最小速度; 也支持"慢而明确"的纯距离触发。
 *   - iOS 交互式返回: 滑动距离 ≥ ~12% 屏高 (本屏 300px → 36px) + 快速滑动速度捷径
 *       + 稳定滤波。
 *   - LVGL 手势: 方向由"首点→当前点"向量角度判定 (gesture_limit 距离 +
 *       gesture_min_velocity 速度), 比固定横向容差更鲁棒 (容许一定斜度)。
 *
 * 与旧实现相比的关键改进 (修"所有页面上滑都失效"的真因):
 *   1) 方向用"向量角度"判定 — 向上为支配轴即算上滑, 容许 ±~45° 斜度 (LVGL 风格),
 *      旧实现用硬性 adx<40 容差, 手指轻微横移即漏判成点击 (这是失效主因)。
 *   2) 距离 + 速度双阈值 (Android/iOS 风格): 慢滑够远(≥36px)即返回; 快滑
 *      (≥22px 且 ≥500px/s)即返回。
 *   3) "上滑返回"判定提前到 点击/长按 之前, 避免被"点击"分支拦走 (旧实现里
 *      <45px 的慢上滑 与 带横移的快上滑 都被"点击"分支吃掉)。
 *   4) 起点必须在底部手势区 (webOS 风格), 由 input_in_bottom_zone() 统一按旋转换算。
 *   5) 任何明确的上滑都只返回 BACK (不再有"滑到底 = 强制退出"的旧分支, 避免
 *      用户"上滑 = 返回"的直觉被 CONFIRM 强制退出打断; V1.1.1 调整)。
 *   6) 跨过第 1 档时给一次触觉反馈 (iOS/webOS 风格); 不画跟手黑线 (用户已要求删除)。
 *   7) V1.1.2: 1s 内第二次明确上滑 → CONFIRM (强制退出), 给第 2 次振动以区分。
 *      普通用户单滑=返回; 急着退出/回主菜单时连滑两次 = webOS/iOS 标准手势。
 *      两次连滑的"双击计时器"在非上滑命中处主动清零, 避免误触发。
 * ==========================================================================*/

/* —— (A) 底部上滑 = 返回 —— */
void gesture_back_enable(bool en);                       /* 开关 (替代旧 input_set_swipe_back) */
void gesture_back_on_press(int sx, int sy, uint32_t t_ms);
void gesture_back_on_move(int sx, int sy, uint32_t t_ms);
menu_action_t gesture_back_on_release(int sx, int sy, uint32_t t_ms);  /* NONE / BACK / CONFIRM */
/* 判断 (sx, sy) 是否落在 gesture_back 认定的"底部手势区"内 (rot=0 全宽底部 50px,
 * 非 default 旋转走 input_in_bottom_zone 的中心带兜底). input.c 释放分支
 * 兜底"底部带非上滑位移=点击"也用这个, 保证单一来源. */
bool gesture_back_in_bottom_zone(int sx, int sy);

/* —— (B) 状态栏长按 = 回桌面 (HOME) ——
 * 仅做"几何+时长"判定; 是否真的投递、与 game-collection 长按共享的触发守卫
 * (s_tg_long_fired) 仍由 input.c 负责 (那是跨手势的, 不属于本模块)。 */
bool gesture_home_decide(int sx0, int sy0, int cur_x, int cur_y, uint32_t dt_ms);

#ifdef __cplusplus
}
#endif
#endif
