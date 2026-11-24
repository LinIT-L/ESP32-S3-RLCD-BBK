/* 宿主机测试桩：只声明 os_app.c 用到的内核契约（与 components/system/os/include/os.h 对齐）。
 * ⚠ 被测的是**生产源码** components/system/os/kernel/os_app.c，
 *   本文件只提供它需要的最小接口，不含任何实现逻辑。 */
#ifndef TEST_STUB_OS_H
#define TEST_STUB_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OS_PAGE_MAIN = 0,
    OS_PAGE_SETTINGS,
    OS_PAGE_TERMINAL,
    OS_PAGE_LUA,
    OS_PAGE_COUNT
} os_page_t;

typedef struct {
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
void os_app_unregister(const char *id);
void os_app_sync(const os_app_t *const *views, int n);
int  os_app_prune(bool (*keep)(const char *id, void *ud), void *ud);
uint32_t os_app_sync_epoch(void);
int  os_app_count(void);
const os_app_t *os_app_get(int idx);
uint32_t os_app_vis_gen(void);

/* os_app_visible_with() 在定义之前就用到这两个（真实 os.h 里也是先声明后使用） */
bool os_app_in_mainmenu(const char *id);
bool os_app_in_hidden(const char *id);

#endif
