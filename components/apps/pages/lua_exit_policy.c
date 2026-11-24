/* lua_exit_policy.c — 见 lua_exit_policy.h
 *
 * 三条停止条件, 缺一条都会出问题:
 *   ① 当前页已经不是 Lua 页 → 退干净了, 继续弹会误伤上层的正常页面;
 *   ② 返回栈已空          → os_pop 在栈空时原地早退 (current_page 不变),
 *                            循环会空转, 必须靠这条停;
 *   ③ 达到上限            → 异常状态 (比如页 id 被写成 LUA) 兜底, 绝不无限弹。
 */
#include "lua_exit_policy.h"

bool lua_exit_pop_continue(int cur_page, int lua_page, int stack_depth,
                           int done, int max_pop)
{
    if (done >= max_pop)     return false;   /* ③ 上限 */
    if (stack_depth <= 0)    return false;   /* ② 栈空 */
    return cur_page == lua_page;             /* ① 还在 Lua 页 */
}
