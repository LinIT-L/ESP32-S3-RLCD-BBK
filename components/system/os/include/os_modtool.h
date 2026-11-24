/**
 * os_modtool.h — 修改机 (游戏内存搜索) 公共接口.
 *
 * svc_modtool (状态栏常驻) 与 page_modtool (主页面) 共享运行态标志:
 *   - 页面 on_enter 置 running=true; on_exit 置 false.
 *   - 状态栏图标点击时据此决定弹"打开"还是"修改机运行中-关闭".
 *   - 从主菜单/软件管家再次点修改机图标时, page 端也据 running 弹"是否关闭".
 */
#ifndef OS_MODTOOL_H
#define OS_MODTOOL_H

#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 修改机是否已打开 (页面 on_enter/on_exit 维护) */
void os_modtool_set_running(bool on);
bool os_modtool_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* OS_MODTOOL_H */
