#ifndef APPS_H
#define APPS_H

#include <stdbool.h>

/* L4 应用层对外接口 (唯一入口, 由 main 调用) */
void apps_register_all(void);   /* 注册全部内置应用页 + 内置应用表 */
/* 只读: 该 id 是否为固件内置应用 (供商店扫描去重) */
bool apps_builtin_has_id(const char *id);

#endif /* APPS_H */
