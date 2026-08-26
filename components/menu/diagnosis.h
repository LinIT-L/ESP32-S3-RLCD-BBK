/**
 * @file diagnosis.h
 * @brief 故障诊断应用: 按故障现象分层排查 + 设备概率排序.
 *
 * 特性:
 *  - 故障大类(6, 3x2) → 逐级细化 → 咨询设备类型/配置 → 出结果.
 *  - 顶部 32px 步进条 (替代状态栏): 动态增长, 均分铺满, 拥挤只显关键节点,
 *    左已走/中当前/右未到, 可点返回上一步.
 *  - 右侧设备概率排序栏: 空间自适应显示几个, 设备名+右对齐百分比+进度条.
 *
 * 集成点 (menu_system.c / main.c):
 *  - current_page == MENU_PAGE_DIAGNOSIS 时, render 走 diag_render,
 *    action 走 diag_action, 触摸 tap 走 diag_touch, 每帧轮询走 diag_poll.
 */
#ifndef DIAGNOSIS_H
#define DIAGNOSIS_H

#include "menu_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 进入页面时重置诊断流程到起始. */
void diag_reset(menu_state_t *state);

/* 每帧轮询 (闪烁/动画). 返回 true 表示需重绘. */
bool diag_poll(menu_state_t *state);

/* 渲染当前诊断界面. */
void diag_render(menu_state_t *state);

/* 按键/方向/确认/返回分发. */
void diag_action(menu_state_t *state, menu_action_t action);

/* 触摸点击命中. 返回 true 表示已消费. */
bool diag_touch(menu_state_t *state, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* DIAGNOSIS_H */