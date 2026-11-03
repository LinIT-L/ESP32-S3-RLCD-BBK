#ifndef APPS_H
#define APPS_H

#include <stdbool.h>
#include "os.h"     /* os_app_t（内置应用表只读枚举用） */

/* L4 应用层对外接口 (唯一入口, 由 main 调用) */
void apps_register_all(void);   /* 注册全部内置应用页 + 内置应用表 */
/* 只读: 该 id 是否为固件内置应用 (供商店扫描去重) */
bool apps_builtin_has_id(const char *id);

/* ★ 2026-11-03: 内置应用表**只读枚举** —— 统一核心 am_app_core 要按表生成
 *   AM_KIND_BUILTIN 记录。只读，不改注册表，不影响启动。 */
int             apps_builtin_count(void);
const os_app_t *apps_builtin_at(int i);   /* 越界返回 NULL */

#endif /* APPS_H */
