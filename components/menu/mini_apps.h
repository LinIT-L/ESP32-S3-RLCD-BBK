/**
 * @file mini_apps.h
 * @brief 一批面向 ESP32-S3 400x300 1-bit LCD 的独立小应用 (小工具 + 小游戏).
 *
 * 设计目标:
 *  - 完全自包含: 只用 st7305 驱动 + 内置 ASCII 字体 + 全局菜单状态, 不依赖
 *    运行期资源 (SD/字体文件/网络), 保证任何情况下都能稳定渲染.
 *  - 与 menu_system 解耦: 页面切换通过 MENU_PAGE_MINIAPPS 进入, 由 mini_apps
 *    内部管理"启动器 + 各应用"的渲染/按键/触摸/轮询.
 *  - 无动态分配: 应用内全部用静态缓冲区, 避免内存碎片.
 *
 * 集成点 (menu_system.c / main.c):
 *  - current_page == MENU_PAGE_MINIAPPS 时, render 走 mini_apps_render,
 *    action 走 mini_apps_action, 触摸 tap 走 mini_apps_touch, 每帧轮询走
 *    mini_apps_poll.
 */
#ifndef MINI_APPS_H
#define MINI_APPS_H

#include "menu_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 每帧轮询 (动画/计时/连续触摸). 返回 true 表示此帧需要重绘. */
bool mini_apps_poll(menu_state_t *state);

/* 渲染当前界面 (启动器列表 或 已打开的应用). */
void mini_apps_render(menu_state_t *state);

/* 按键/方向键/确认/返回 分发到当前界面与应用. */
void mini_apps_action(menu_state_t *state, menu_action_t action);

/* 触摸点击命中 (含坐标). 返回 true 表示已消费, 不向下传递. */
bool mini_apps_touch(menu_state_t *state, int x, int y);

/* 进入页面时调用, 复位启动器为首页. */
void mini_apps_reset(menu_state_t *state);

/* ===== 应用管理集成 =====
 * 迷你应用并入应用管理右网格 (工具=小工具, 游戏=小游戏). 应用管理据此:
 *  - 每分类"迷你应用数量 / 第 k 个应用的全局索引"生成图标项;
 *  - 按图标启动 (mini_apps_launch) 后, 页面分发自动转交给 mini_apps_*.
 * giant"gi" 为 s_apps[] 全局索引 (与 mini_icons[] 顺序一致). */
int  mini_apps_count(void);                 /* 迷你应用总数 */
const char *mini_apps_title(int gi);        /* 应用英文标题 (ASCII, 用于图标下方) */
int  mini_apps_cat_count(bool game);        /* game=true→游戏类数量, false→工具类数量 */
int  mini_apps_cat_gi(bool game, int k);    /* 该分类第 k 个应用的全局索引 (越界返回 -1) */
void mini_apps_launch(int gi);              /* 启动某迷你应用 */
bool mini_apps_active(void);                /* 当前是否已在某个迷你应用内 */
void mini_apps_close(void);                 /* 关闭迷你应用 (回到装界面) */

/* ===== 网络工具独立主菜单页 (NETTOOL) =====
 * 作为主菜单独立模块进入, 与迷你应用启动器解耦. 内部若经 nt_launch 打开了
 * 嵌套的迷你应用 (如局域网扫描), 会自动转交 mini_apps_* 渲染/交互. */
void nettool_reset(menu_state_t *state);
void nettool_render(menu_state_t *state);
void nettool_action(menu_state_t *state, menu_action_t action);
bool nettool_poll(menu_state_t *state);
bool nettool_touch(menu_state_t *state, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* MINI_APPS_H */