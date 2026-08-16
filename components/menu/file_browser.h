#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include "menu_system.h"
#include "sd_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化: 重置浏览器到 /sdcard 根目录 */
void fb_init(void);

/* 当前目录的路径 (只读, 用于显示在标题) */
const char *fb_get_path(void);

/* 菜单子页回调: build_items / on_confirm (跟 select_game_on_confirm 同套框架) */
int  fb_build(menu_state_t *state, char buf[][64], int max);
bool fb_on_confirm(menu_state_t *state, int idx);

/* 在 FILE_BROWSER 页按 BACK 时调用:
 * - 非根目录: 返回上一级
 * - 根目录: 返回主菜单 (返回 true 让 menu_handle_action 跳到 MAIN)
 * 返回 true 表示已经处理 (不进入 menu_handle_action 默认 BACK 逻辑) */
bool fb_on_back(menu_state_t *state);

/* === 弹窗化文件浏览器 (用户需求: 用 list_dialog 弹窗样式) ===
 * 从 SD 管理 list_dialog 中"浏览文件"项打开:
 *   - 弹窗显示: 目录/文件 + "上一级"(非根) + "返回"
 *   - 选中目录: 进入 (重新填充弹窗项)
 *   - 选中文件: 启动游戏
 *   - 选中"上一级": 返回上级目录 (非根时显示)
 *   - 选中"返回" 或 按 BACK 键: 关闭弹窗, 回到 SD 管理弹窗 (via on_close) */
void fb_open_dialog(menu_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* FILE_BROWSER_H */
