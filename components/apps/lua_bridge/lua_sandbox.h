/* lua_sandbox.h — 第三方 Lua 应用运行时隔离（信任级 / 路径沙箱 / 能力白名单 / 内存配额 / 运行时限）
 *
 * 设计要点:
 *   1. **信任级由"脚本从哪来"决定，不由 LAPP 头决定** —— 文件头是用户可控文本,
 *      写 `--@app trust=official` 谁都会; 安装路径由安装流程决定, 应用改不了。
 *   2. 官方应用 (`/store/apps/ 下全部`) 走**完全原有行为**, 一行库都不摘 —— 这是兼容底线。
 *   3. 第三方应用摘掉危险入口 + 文件路径限制在自己的数据目录 + 能力白名单。
 *   4. 只做"能验证的最小集", 不假装是完整沙箱: 静态扫描/人工审核仍是防线之一。
 *
 * 四层(每一层单独都不够):
 *   L1 摘库      os.execute/exit/tmpname, io.popen/tmpfile, package.loadlib, loadfile/dofile
 *   L2 路径守卫  io.open / os.remove / os.rename / app.fs.* / app.img.load 全部经路径守卫
 *   L3 资源配额  内存计入 lua_psram_alloc; 时限由 count hook + 阻塞 API 夹紧
 *   L4 能力白名单 app.hid.* / app.wiki.open 等按信任级拒绝, 返回统一错误
 */
#ifndef LUA_SANDBOX_H
#define LUA_SANDBOX_H

#include <stdbool.h>
#include <stddef.h>
#include "lua.h"

typedef enum {
    LUA_TRUST_OFFICIAL = 0,     /* 随固件/商店正式安装到 /store/apps */
    LUA_TRUST_THIRD_PARTY = 1,  /* 用户侧目录, 不可信 */
} lua_trust_t;

/* NVS 命名空间上限是 15 字符(NVS_NS_NAME_MAX_SIZE-1)。appdb_ + 8hex = 14, 留 1 余量。 */
#define LUA_SANDBOX_NS_MAX 15

/* 每次启动脚本前调用一次(在 lua_newstate 之前):
 *   - 按 path 判定信任级
 *   - 解析 appid, 组装数据目录 /sdcard/apps/.data/<appid>/
 *   - 复位内存配额与超时基准
 * 返回 false = 路径非法(推不出 appid), 调用方应拒绝启动。 */
bool lua_sandbox_begin(const char *script_path);

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

/* 剩余预算(ms): -1 = 不限。给阻塞式 C API 夹紧用。 */
int lua_sandbox_remain_ms(void);

/* 把一次阻塞等待/阻塞 I/O 的时长夹到剩余预算内。
 * OFFICIAL 或没设时限 → 原样返回 want_ms。
 * 第三方 → min(want_ms, 剩余); 已超时返回 1(不返回 0, 免得"0 = 不限"的语义被误用)。 */
int lua_sandbox_clamp_ms(int want_ms);

/* 数据命名空间隔离: OFFICIAL 原样返回 base (行为不变);
 * THIRD_PARTY 返回 "appdb_<8hex>"(FNV-1a(appid) 低 32 位, 恒 14 字符, 不超 NVS 15 字节上限)。
 * 用于 app.db.* / app.nvs.* 等一切落到 NVS 的接口。 */
const char *lua_sandbox_ns(const char *base);

#endif /* LUA_SANDBOX_H */
