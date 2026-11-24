/* lua_sandbox.h — Lua 应用的**资源级**运行保护 (内存配额 / 单次回调时限)
 *
 * ★ 2026-11-22 文档对齐实现 (第2回合): 本文件曾经描述"四层隔离"(摘库 / 路径守卫 /
 *   配额 / 能力白名单), 但 2026-11-02 linit 已决定**统一放开 Lua 应用权限** ——
 *   不摘库、不限路径、不 deny 任何已注册 API。为免后来者按旧描述排查(以为文件写
 *   被拦是真凶), 这里按**实际生效**的写:
 *
 *   实际保留 (是稳定性底线, 不是权限):
 *     · 内存配额    —— lua_sandbox_mem_account(): 单个应用不能吃光 PSRAM。
 *     · 回调时限    —— count hook + lua_sandbox_touch(): 防 while true 钉死主任务。
 *     · 阻塞 API 夹紧 —— lua_sandbox_clamp_ms(): 不让一次调用跨过超时预算。
 *     · appid 解析  —— 仍算 (日志/数据目录用), 但**推不出也不拒绝启动**。
 *
 *   保留但当前**不参与决策**的代码 (将来要重新隔离时改这些点即可):
 *     · lua_trust_t / lua_sandbox_trust(): 恒为 OFFICIAL (见 lua_sandbox_begin)。
 *     · lua_path_guard(): 生产路径零调用, 只有主机测试在用 (tests/lua_guard)。
 */
#ifndef LUA_SANDBOX_H
#define LUA_SANDBOX_H

#include <stdbool.h>
#include <stddef.h>
#include "lua.h"

typedef enum {
    LUA_TRUST_OFFICIAL = 0,     /* 随固件/商店正式安装 */
    LUA_TRUST_THIRD_PARTY = 1,  /* 历史预留: 当前**不再**据此收权限 */
} lua_trust_t;

/* NVS 命名空间上限是 15 字符(NVS_NS_NAME_MAX_SIZE-1)。appdb_ + 8hex = 14, 留 1 余量。 */
#define LUA_SANDBOX_NS_MAX 15

/* 每次启动脚本前调用一次(在 lua_newstate 之前):
 *   - 复位内存配额与超时基准
 *   - 解析 appid (推不出就用 "app"), 组装数据目录 /sdcard/apps/.data/<appid>/
 * 返回 false 只在**脚本路径为空**时 (调用方应拒绝启动)。
 * ⚠ 原来这里写着"推不出 appid 就拒绝启动", 实际早已改为不拒绝 —— 别按旧描述排查。 */
bool lua_sandbox_begin(const char *script_path);
/* 本次运行的脚本路径 (可能为空串)。app.pkg.self() 用它打开自身单文件包。 */
const char *lua_sandbox_script_path(void);

lua_trust_t lua_sandbox_trust(void);
const char *lua_sandbox_appid(void);
const char *lua_sandbox_data_dir(void);

/* openlibs + 桥注册之后调用: 装指令钩子(防死循环)。
 * ★ 2026-11-02: 不再按信任级摘库/限制路径/拒绝 API —— 统一放开。
 *   目前只保留内存配额(独立于本函数)与单次回调超时。 */
void lua_sandbox_apply(lua_State *L);

/* 每次进入 Lua 回调(render/action/touch)前调用: 重置单次执行的时间预算。
 * 不调用的话, 应用会在启动 10 秒后被超时钩子杀掉。 */
void lua_sandbox_touch(void);

/* 内存配额: 由 lua_psram_alloc 每次分配后调用。
 * 返回 false = 超额(调用方应让 realloc 失败, 由 Lua 抛 "not enough memory")。 */
bool lua_sandbox_mem_account(size_t used_now);

/* 当前配额(字节), 0 = 不限 */
size_t lua_sandbox_mem_limit(void);
size_t lua_sandbox_mem_peak(void);
size_t lua_sandbox_mem_used(void);

/* 运行时限检查(由 hook 调), 返回 true = 已超时 */
bool lua_sandbox_timed_out(void);

/* 本次回调是否**确实**被超时钩子打断过 (s_timeout 置位, lua_sandbox_touch() 复位)。
 * ★ 2026-11-22: 给 page_lua.c 区分"超时"与"普通脚本错误" —— 超时必须立即停应用,
 *   不能走"连续 N 次才退"的计数路径。 */
bool lua_sandbox_timeout_hit(void);

/* 剩余预算(ms): -1 = 不限。给阻塞式 C API 夹紧用。 */
int lua_sandbox_remain_ms(void);

/* 把一次阻塞等待/阻塞 I/O 的时长夹到剩余预算内 (与信任级无关, 一律生效)。
 * 没设时限 → 原样返回 want_ms; 已超时返回 1 (不返回 0, 免得"0 = 不限"的语义被误用)。 */
int lua_sandbox_clamp_ms(int want_ms);

/* NVS 命名空间: ★ 2026-11-02 起**原样返回 base** —— 不再按信任级隔离。
 *   本系统 Lua 应用都由作者自己维护, 拆分命名空间只会让老应用读不到自己以前存的数据。
 *   接口保留 (调用点不改), 将来若要重新隔离, 只改 lua_sandbox.c 这一处。 */
const char *lua_sandbox_ns(const char *base);

#endif /* LUA_SANDBOX_H */
