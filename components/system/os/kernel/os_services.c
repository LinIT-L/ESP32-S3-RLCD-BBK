/**
 * os_services.c — OS 全局服务装配层 (V1.2 从 os_pages.c 收窄而来).
 *
 * 只负责注册 os 内核侧全局服务 (状态栏 / 修改机常驻图标).
 * 应用页与应用表的注册已迁到 apps 组件 (apps_register_all), 由 main 在 os_init() 后调用,
 * 从而彻底断开 os -> apps 的反向依赖.
 */
#include "os.h"
#include "os_internal.h"

void os_register_services(void)
{
    os_svc_statusbar_register();   /* 状态栏: 日期/时间/电池/蓝牙/音量/NET 常驻图标 */
    os_svc_modtool_register();     /* 修改机常驻图标 (热区) */
    os_svc_quickbar_register();    /* 顶部下拉快捷栏: 音量/蓝牙/Wi-Fi (全屏页也可用) */
}
