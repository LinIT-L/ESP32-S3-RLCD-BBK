#ifndef APPS_H
#define APPS_H

/* L4 应用层对外接口 (唯一入口, 由 main 调用) */
void apps_register_all(void);   /* 注册全部内置应用页 + 内置应用表 */

#endif /* APPS_H */
