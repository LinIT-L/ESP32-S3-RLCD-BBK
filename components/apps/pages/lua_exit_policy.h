/* lua_exit_policy.h — Lua 应用退出收尾判据 (纯逻辑, 零依赖, 宿主机可测)
 *
 * 背景 (2026-11-23 linit 报障): 退出「远程控制测试应用」后连着出现几个空白页,
 * 每按一次返回才退一层。根因是返回栈里堆了多层 Lua 页 (远程控制台每次
 * `app <脚本>` 都 os_push 一层), 而退出时只弹写死的 3 层。
 *
 * 这个头只回答一个问题: **这一步还要不要继续弹?**
 * 单独成文件是为了让主机测试能直接编译生产判据 (tests/app_core),
 * 而不是在测试里重写一份"看起来一样"的逻辑。
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 退出 Lua 应用时, 是否再弹一层返回栈。
 *   cur_page    : 当前页 id
 *   lua_page    : Lua 页 id (OS_PAGE_LUA 的值; 由调用方传入, 本文件不依赖 os.h)
 *   stack_depth : 返回栈深度 (>0 才有可弹的父页)
 *   done        : 本次退出已弹层数
 *   max_pop     : 上限 (防异常状态空转)
 * 返回 true = 继续弹; false = 收工。 */
bool lua_exit_pop_continue(int cur_page, int lua_page, int stack_depth,
                           int done, int max_pop);

#ifdef __cplusplus
}
#endif
