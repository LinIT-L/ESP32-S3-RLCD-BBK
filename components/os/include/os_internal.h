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
void os_register_all_internal(void);

#ifdef __cplusplus
}
#endif

#endif /* OS_INTERNAL_H */