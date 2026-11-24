/* lua_quota.h — 第三方 Lua 应用的资源账本纯逻辑（无 ESP-IDF 依赖）
 *
 * 为什么单独抽出来: 内存计费是最容易"看起来对、实际算错"的一段算术。
 *   放在 page_lua.c 里就没法在主机上跑测试, 只能靠真机日志猜。
 *   抽成纯函数后 **固件和主机测试编译同一份源码**, 断言的是生产逻辑本身。
 *
 * 只有三件事: ①realloc 后的已用量 ②是否超额 ③剩余预算毫秒数。
 */
#ifndef LUA_QUOTA_H
#define LUA_QUOTA_H

#include <stdbool.h>
#include <stddef.h>

/* 一次分配/重分配之后的"已用量"。
 *   ptr == NULL           → 新分配, 不减旧块
 *   nsize == 0            → 释放, 返回 used - osize
 * 用有符号中间量做减法: 如果直接写 `used - osize`, 一旦 osize 因为任何原因
 * 大于 used(size_t 下溢)就会变成天文数字, 配额判断从此永远通过 ——
 * 那正是"计费不一致"最隐蔽的形态。这里显式夹到 0。 */
size_t lua_quota_after(size_t used, const void *ptr, size_t osize, size_t nsize);

/* 是否仍在配额内。limit == 0 表示不限。 */
bool lua_quota_allows(size_t used, size_t limit);

/* 剩余预算(ms): unlimited 返回 -1; 已过期返回 0。
 * 给阻塞式 C API(app.util.sleep / app.net.http_get)夹紧用 ——
 * lua_sethook 只能在 Lua 字节码边界打断, 拦不住 C 里的一次 vTaskDelay。 */
int lua_quota_remain_ms(long long deadline_us, long long now_us);

#endif /* LUA_QUOTA_H */
