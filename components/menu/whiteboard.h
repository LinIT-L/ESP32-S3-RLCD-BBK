#ifndef WHITEBOARD_H
#define WHITEBOARD_H

#include <stdbool.h>
#include "menu_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 白板 (画图) 独立主菜单页. 由 menu_system 主菜单入口进入, main 每帧调 wb_poll 轮询触摸绘制. */
void wb_reset(menu_state_t *st);            /* 进入页面: 初始化(首次) + 自动加载上次存档 */
void wb_render(menu_state_t *st);           /* 整页渲染 (画布 + 工具栏) */
bool wb_touch(menu_state_t *st, int x, int y);  /* 离散点击: 统一吞掉 (绘制全在 wb_poll) */
void wb_action(menu_state_t *st, menu_action_t a); /* 方向键切换工具/粗细 */
void wb_poll(menu_state_t *st);            /* 每帧轮询: 触摸绘制 + 工具栏按钮 */
void wb_autosave(void);                    /* 退出页面前落盘到 TF 卡 */
bool wb_exit_asking(void);                 /* 是否处于"确认退出"浮层 (返回键两步确认) */
void wb_exit_set(bool on);                 /* 进入/退出确认退出浮层 */
bool wb_modal_active(void);                /* 是否处于加载/删除列表模态 */
void wb_modal_action(menu_state_t *st, menu_action_t a); /* 加载列表按键处理 */

#ifdef __cplusplus
}
#endif

#endif /* WHITEBOARD_H */