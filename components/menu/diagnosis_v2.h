/**
 * @file diagnosis_v2.h
 * @brief 故障诊断应用 V2: 按故障现象分层排查 + 设备概率排序.
 *
 * V1.1.0 重设计 (与原 diagnosis 并存, 编译开关切换):
 *  - 硬件概率栏移到【左侧】并变窄(120px), 图元改上下结构(图标/名称/概率/进度条).
 *  - 故障/策略选项中区移到右侧, 一次性加载, 上下滑动选择.
 *  - 左栏支持主菜单式整体拖动滚动查看全部候选.
 *  - 进入时先展示「获取信息」采集策略按钮组.
 *
 * 集成点 (menu_system.c):
 *  - current_page == MENU_PAGE_DIAGNOSIS 时, render/action/touch/poll 走 diag_*.
 */
#ifndef DIAGNOSIS_V2_H
#define DIAGNOSIS_V2_H

#include "menu_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 进入页面时重置诊断流程到起始. */
void diag_reset(menu_state_t *state);

/* 每帧轮询 (闪烁/动画/拖动). 返回 true 表示需重绘. */
bool diag_poll(menu_state_t *state);

/* 渲染当前诊断界面. */
void diag_render(menu_state_t *state);

/* 按键/方向/确认/返回分发. */
void diag_action(menu_state_t *state, menu_action_t action);

/* 触摸点击命中. 返回 true 表示已消费. */
bool diag_touch(menu_state_t *state, int x, int y);

/* 当前是否处于诊断最顶层 (选故障大类 D_ST_CAT).
 * 供菜单层判断: 顶层 BACK 触发统一「退出程序?」; 深层 BACK 仍逐级回退. */
bool diag_at_top(void);

#ifdef __cplusplus
}
#endif

#endif /* DIAGNOSIS_V2_H */