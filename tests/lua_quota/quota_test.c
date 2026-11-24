/* quota_test.c — 内存配额账本 + 时限预算的主机测试
 *
 * 编译的是生产源码 lua_quota.c（固件里 page_lua.c 的分配器调的就是它）。
 * 目的不是"跑通"，而是证明**不会出现计费漂移**：
 *   - 扩容/缩容/释放三种情形扣减都对；
 *   - 异常入参(osize > used)不会让 size_t 下溢把配额架空；
 *   - 反复申请释放 10 万次后计数精确归零（这是长期运行后才暴露的那类 bug）。
 */
#include "lua_quota.h"

#include <stdio.h>
#include <stdint.h>

static int g_pass, g_fail;
#define CHECK(cond, fmt, ...) do {                                    \
    if (cond) { g_pass++; printf("  PASS  " fmt "\n", ##__VA_ARGS__); } \
    else      { g_fail++; printf("  FAIL  " fmt "\n", ##__VA_ARGS__); } \
} while (0)

#define KB (1024u)

int main(void)
{
    const char *fake = (const char *)0x1000;   /* 只当"非空指针"用, 不解引用 */

    printf("=== 内存配额账本（生产源码 lua_quota.c）===\n\n");

    printf("T1 新分配 / 扩容 / 缩容 / 释放\n");
    size_t u = 0;
    u = lua_quota_after(u, NULL, 0, 100);          CHECK(u == 100, "新分配 100 → %zu", u);
    u = lua_quota_after(u, fake, 100, 400);        CHECK(u == 400, "扩容 100→400 (不叠加) → %zu", u);
    u = lua_quota_after(u, fake, 400, 50);         CHECK(u == 50,  "缩容 400→50 (要扣回去) → %zu", u);
    u = lua_quota_after(u, NULL, 0, 200);          CHECK(u == 250, "再新分配 200 → %zu", u);
    u = lua_quota_after(u, fake, 50, 0);           CHECK(u == 200, "释放 50 → %zu", u);
    u = lua_quota_after(u, fake, 200, 0);          CHECK(u == 0,   "全部释放 → %zu", u);
    printf("\n");

    printf("T2 异常入参不能让计数下溢（下溢=配额永远通过）\n");
    u = lua_quota_after(10, fake, 9999, 0);        CHECK(u == 0,  "osize>used 释放 → 夹到 0 (实际 %zu)", u);
    u = lua_quota_after(10, fake, 9999, 64);       CHECK(u == 64, "osize>used 扩容 → 按 64 计 (实际 %zu)", u);
    u = lua_quota_after(0, NULL, 0, 0);            CHECK(u == 0,  "全零 → 0");
    printf("\n");

    printf("T3 配额判定边界\n");
    CHECK(lua_quota_allows(4096 * KB, 4096 * KB) == true,  "正好等于上限 → 允许");
    CHECK(lua_quota_allows(4096 * KB + 1, 4096 * KB) == false, "超 1 字节 → 拒绝");
    CHECK(lua_quota_allows(99999999, 0) == true, "limit=0 表示不限");
    CHECK(lua_quota_allows(0, 1) == true, "空载永远允许");
    printf("\n");

    printf("T4 反复申请/释放 10 万次后必须精确归零（长期运行才暴露的漂移）\n");
    u = 0;
    for (int i = 0; i < 100000; i++) {
        size_t a = (size_t)(i % 300 + 1);
        u = lua_quota_after(u, NULL, 0, a);
        u = lua_quota_after(u, fake, a, a * 2);     /* 中途扩容一次 */
        u = lua_quota_after(u, fake, a * 2, a / 2);  /* 中途缩容一次 */
        u = lua_quota_after(u, fake, a / 2, 0);      /* 释放 */
    }
    CHECK(u == 0, "10 万轮后 used=%zu (必须 0)", u);
    printf("\n");

    printf("T5 连续 realloc 增长到超限 → 被拒, 释放后能回到 0\n");
    {
        const size_t lim = 64 * KB;
        u = 0;
        int rejected = 0, allocs = 0;
        for (int i = 0; i < 400; i++) {
            const size_t nsize = (size_t)(i + 1) * 256;      /* 每步 +256B */
            const size_t next = lua_quota_after(u, i ? fake : NULL, (size_t)i * 256, nsize);
            if (!lua_quota_allows(next, lim)) { rejected++; break; }
            u = next; allocs++;
        }
        CHECK(rejected == 1, "在 %d 次分配后被拒(上限 64KB)", allocs);
        CHECK(u <= lim, "被拒时 used=%zu 未越界", u);
        u = lua_quota_after(u, fake, u, 0);
        CHECK(u == 0, "释放后归零 → %zu", u);
    }
    printf("\n");

    printf("T6 时限预算\n");
    CHECK(lua_quota_remain_ms(0, 1000) == -1, "deadline=0 → 不限(-1)");
    CHECK(lua_quota_remain_ms(10000000, 9000000) == 1000, "剩 1s");
    CHECK(lua_quota_remain_ms(10000000, 10000000) == 0, "正好到期 → 0");
    CHECK(lua_quota_remain_ms(10000000, 11000000) == 0, "已过期 → 0(不返回负数)");
    CHECK(lua_quota_remain_ms(20000000, 10000000) == 10000, "剩 10s");
    CHECK(lua_quota_remain_ms(INT64_MAX / 2, 0) == 2147483647, "超大余量夹到 INT_MAX");
    CHECK(lua_quota_remain_ms(1500, 1000) == 0, "剩 500us → 0ms(不四舍五入成 1)");
    printf("\n");

    printf("================ %d PASS / %d FAIL ================\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
