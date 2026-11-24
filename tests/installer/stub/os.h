/* 宿主机测试用的 os.h 替身 —— 只给 am_lpkg.c 用到的 os_app_t 与注册接口。
 * ⚠ 字段名必须与 components/system/os/include/os.h 的 struct os_app 一致；
 *   只用于"安装器能不能正确注册条目"这类断言，不测主菜单行为。 */
#ifndef TEST_STUB_OS_H
#define TEST_STUB_OS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { OS_PAGE_LUA = 0, OS_PAGE_MAIN = 1 } os_page_t;

typedef struct os_app {
    const char *id;
    const char *label;
    int         icon_idx;
    os_page_t   page;
    bool        hidden;
    bool        touch_only;
    const char *cat;
    bool        reveal;
    const char *requires;
} os_app_t;

void os_app_register(const os_app_t *app);
/* 卸载时清桌面图标归属（mainmenu / mmhide 两个持久化列表）—— 宿主机测试里只记录调用次数 */
void os_app_set_mainmenu(const char *id, bool show);
void os_app_set_hidden(const char *id, bool hide);

#endif
