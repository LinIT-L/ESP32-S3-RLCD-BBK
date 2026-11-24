/* lua_quota.c — 见 lua_quota.h。纯 C, 不依赖 ESP-IDF / Lua。 */
#include "lua_quota.h"

size_t lua_quota_after(size_t used, const void *ptr, size_t osize, size_t nsize)
{
    /* 释放: 只扣旧块 */
    if (nsize == 0) {
        if (!ptr) return used;
        return (osize >= used) ? 0 : (used - osize);
    }
    /* 新块: 旧块大小由调用方保证(ptr!=NULL 时 osize 有效) */
    const size_t old = ptr ? osize : 0;
    if (old >= used) {
        /* 异常入参(old 比 used 还大): 不能减, 视为从 0 起算 —— 多算不多少算,
         * 宁可严格一点也不能让下溢把配额架空。 */
        return nsize;
    }
    return used - old + nsize;
}

bool lua_quota_allows(size_t used, size_t limit)
{
    if (limit == 0) return true;      /* 0 = 不限 */
    return used <= limit;
}

int lua_quota_remain_ms(long long deadline_us, long long now_us)
{
    if (deadline_us <= 0) return -1;             /* 没设时限 */
    if (now_us >= deadline_us) return 0;         /* 已过期 */
    const long long d = (deadline_us - now_us) / 1000;
    if (d <= 0) return 0;
    if (d > 2147483647LL) return 2147483647;     /* 夹到 int 上限, 不溢出 */
    return (int)d;
}
