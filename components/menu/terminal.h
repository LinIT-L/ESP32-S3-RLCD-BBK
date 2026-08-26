#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdbool.h>
#include "menu_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 终端 (命令行 + 52 键虚拟键盘). 运行时才分配内存, 退出释放. */
void term_reset(menu_state_t *st);           /* 进入: 分配内存 + 欢迎信息 */
void term_render(menu_state_t *st);          /* 整页渲染 (输出区 + 命令栏 + 52键) */
bool term_touch(menu_state_t *st, int x, int y); /* 离散点击统一吞掉 */
void term_action(menu_state_t *st, menu_action_t a); /* 方向键/确认 */
void term_poll(menu_state_t *st);            /* 每帧: 键盘触摸输入 */
void term_autosave(void);                    /* 退出: 恢复上滑手势 + 清空缓冲, 释放后台占用 */
bool term_modal_active(void);                /* 是否处于模态 (与 exitask 协调, 暂为 false) */
void term_modal_action(menu_state_t *st, menu_action_t a); /* 模态按键 (暂空) */

#ifdef __cplusplus
}
#endif

#endif /* TERMINAL_H */