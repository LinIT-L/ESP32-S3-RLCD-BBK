/**
 * os_internal.h — OS 内部注册辅助（内核向页面装配层暴露的接口）.
 * 仅库内部使用, 不对外公开.
 */
#ifndef OS_INTERNAL_H
#define OS_INTERNAL_H

#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 由 os_pages.c 实现: 编译期注册所有内建模块(页面)到 os_register()。
 * os_init() 末尾调用, 让内核在启动时即可解析各页 id。 */
/* V1.2: os 内核侧服务装配 (状态栏/修改机图标). 应用页与应用表的注册已迁到
 * L4 apps 组件 (components/apps, apps_register_all), 由 main 在 os_init() 后调用. */
void os_register_services(void);

/* ==== 页面模块注册函数 (components/os/pages 目录各文件实现) ====
 * 每页一个 .c, 私有 static state 留在各自文件; os_pages.c 统一装配. */
void os_page_engine_register_all(void);    /* GB/GBC/NES/文曲星/ArduBoy 游戏选择菜单页 */
void os_page_dialog_register(void);        /* 通用弹窗 (modal 覆盖页) */
        /* 收藏夹 (账号/密码 一键输入) */
/* 音频自检已删除 */

#ifdef __cplusplus
}
#endif

#endif /* OS_INTERNAL_H */
