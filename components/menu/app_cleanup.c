/**
 * app_cleanup.c — 统一清理钩子注册表.
 *
 * 各模块把自己的"离开时清理"回调登记进来, 菜单系统在返回桌面/来源页前统一调用
 * app_cleanup_run(state), 避免每个模块各自散落退出清理逻辑.
 *  - CPU 占用极低: 登记与执行都是 O(n), n<=16, 无堆分配.
 *  - 幂等: 重复执行清理回调不会产生副作用.
 */
#include "app_cleanup.h"

#define APP_CLEANUP_MAX 16

static app_cleanup_fn s_cleanup[APP_CLEANUP_MAX];
static int            s_n = 0;

void app_cleanup_register(app_cleanup_fn fn) {
    if (!fn) return;
    if (s_n >= APP_CLEANUP_MAX) return;
    /* 幂等: 同一回调只登记一次, 防止重复登记 */
    for (int i = 0; i < s_n; i++) {
        if (s_cleanup[i] == fn) return;
    }
    s_cleanup[s_n++] = fn;
}

void app_cleanup_run(struct menu_state_s *state) {
    for (int i = 0; i < s_n; i++) {
        if (s_cleanup[i]) s_cleanup[i](state);
    }
}