/*
 * lavax_emu.h — wqx 文曲星 (LavaX 虚拟机 lavaxvm) 宿主 API.
 * 供 menu_system 等上层调用.
 */
#pragma once

#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 后台初始化 VM (分配 PSRAM 内存). 幂等: 已在运行则直接返回.
 * 由 engine_manager 在进入文曲星二级菜单时调用. */
esp_err_t lavax_emu_background_init(st7305_handle_t *lcd);

/* 释放全部 VM 内存 (返回主菜单/卸载时调用) */
void lavax_emu_unload(void);

/* 加载并阻塞运行一个 .lav 程序, 直至用户退出 (内部 main_loop).
 * 返回后 VM 仍可复用, 再次调用 lavax_emu_load_and_run 载入下一局. */
esp_err_t lavax_emu_load_and_run(const char *lav_path);

/* 是否正在运行游戏 */
bool lavax_emu_is_running(void);
/* V1.0.9x: 宿主注入"退出确认浮层"绘制回调, 由平台层每帧送屏后叠加调用 */
void lavax_set_exit_confirm_ui(void (*draw)(st7305_handle_t *lcd));
void lavax_draw_exit_confirm(st7305_handle_t *lcd);

/* 注册退出检测回调 (由 menu 层注入): 每帧调用, 返回 true 则请求退出当前游戏.
 * 回调逻辑由 menu 决定 (检测物理返回键/手柄 BACK/F_EXIT 等), 使 lavax 组件
 * 不依赖 menu 头, 避免组件依赖环. */
typedef bool (*lavax_exit_check_fn)(void);
void lavax_set_exit_check(lavax_exit_check_fn fn);

#ifdef __cplusplus
}
#endif