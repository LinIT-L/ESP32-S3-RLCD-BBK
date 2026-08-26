#ifndef APP_CLEANUP_H
#define APP_CLEANUP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 统一清理钩子: 所有应用/模块"返回桌面(或来源页)"之前, 由菜单系统统一调用
 * app_cleanup_run(state) 释放后台在途资源.
 *
 * 设计目标:
 *  - 解耦: 各模块只负责实现自己的清理回调, 在 menu_init 里统一登记(幂等, 可重复调).
 *  - 只释放"不该留在后台"的在途资源(扫描/连接中/番茄钟计时/终端缓冲),
 *    保留必须的后台服务(Wi-Fi/蓝牙已建立连接、触摸)。
 *
 * 约定:
 *  - app_cleanup_register 在模块初始化时调用, 重复注册同一回调只记账一次.
 *  - app_cleanup_run 幂等, 可安全地在多个退出出口(返回键/HOME/游戏退出)重复调用.
 */
struct menu_state_s;
typedef void (*app_cleanup_fn)(struct menu_state_s *state);

/* 登记清理回调 (幂等). */
void app_cleanup_register(app_cleanup_fn fn);

/* 执行所有已登记清理回调 (幂等). 应在离开模块到桌面/来源页前调用. */
void app_cleanup_run(struct menu_state_s *state);

#ifdef __cplusplus
}
#endif

#endif /* APP_CLEANUP_H */