/* lua_path_guard.h — 第三方 Lua 应用的文件路径守卫（纯 C, 无 ESP 依赖, 可主机测试）
 *
 * 为什么单独一个文件: 路径规范化是最容易写错、也最该被测试覆盖的一段。
 * 抽成无依赖的纯函数后, tests/lua_guard 可以在主机上跑上百个用例,
 * 不用烧板子也知道 "../" 到底有没有被挡住。
 */
#ifndef LUA_PATH_GUARD_H
#define LUA_PATH_GUARD_H

#include <stdbool.h>
#include <stddef.h>

/* 判断 in 是否允许访问, 允许时把**规范化后的绝对路径**写进 out。
 *
 *   data_dir : 该应用独占的可读写目录 (如 /sdcard/apps/.data/myapp)
 *   app_dir  : 该应用自身所在目录, 只读 (如 /sdcard/apps/app)
 *   want_write: true = 打开方式含写/追加/创建
 *
 * 规则:
 *   - 相对路径一律相对 data_dir 解析;
 *   - 绝对路径必须落在 data_dir 或 app_dir 之下(含相等), 否则拒绝;
 *   - 任何 ".." 越过根 → 拒绝; "." 与空段被规整掉;
 *   - 写操作只允许在 data_dir 之下(app_dir 只读);
 *   - 结果超过 out 容量 → 拒绝(不截断, 截断会产生另一个路径 = 漏洞)。
 * 返回 true = 允许, out 内是可直接交给 fopen 的路径。
 */
bool lua_path_guard(const char *data_dir, const char *app_dir,
                    const char *in, bool want_write,
                    char *out, size_t cap);

/* 从脚本路径推 appid: 取 basename, 去掉 .lua, 只保留 [A-Za-z0-9_-]。
 * 结果为空(或超长)返回 false —— 调用方应当拒绝启动该脚本。 */
bool lua_path_appid(const char *script_path, char *out, size_t cap);

#endif /* LUA_PATH_GUARD_H */
