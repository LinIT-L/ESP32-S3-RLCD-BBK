/**
 * time_logic_test.c — 时间可信度判定主机测试（不需要板子）
 *
 *   cc -O2 -I components/services/svc_time/include \
 *      tests/time/time_logic_test.c components/services/svc_time/time_logic.c -o /tmp/time_test
 *   /tmp/time_test
 *
 * 覆盖的是**这次最怕出错的三件事**:
 *   ① 刷机/断电后把 1970 当成"有效时间"，于是拒绝正确网络时间；
 *   ② 正常离线几天被误判成"时间被重置"；
 *   ③ 每次联网都全量重校，系统时间反复跳。
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "time_logic.h"

static int g_pass, g_fail;
#define CHECK(cond, fmt, ...) do {                                   \
    if (cond) { g_pass++; }                                          \
    else { g_fail++; printf("  ✗ " fmt "\n", ##__VA_ARGS__); }        \
} while (0)
#define CHECK_EQ(got, want, what) CHECK((got) == (want), \
    "%s: got=%d want=%d (line %d)", what, (int)(got), (int)(want), __LINE__)

/* 2026-11-01 10:00:00 UTC 附近，方便手算 */
#define T_2026  ((int64_t)1793527200)
#define T_1970  ((int64_t)5)

/* ---------------- 启动判定 ---------------- */
static void test_boot(void)
{
    puts("① 启动时本地时间判定");

    /* 刷机/断电：RTC 归零 */
    CHECK_EQ(tl_eval_boot(T_1970, 0), TL_BOOT_INVALID, "1970 + 无记录 → INVALID");
    CHECK_EQ(tl_eval_boot(T_1970, T_2026), TL_BOOT_INVALID, "1970 + 有记录 → INVALID（刷机）");

    /* 首次开机（擦过 flash），但时间看着像样 */
    CHECK_EQ(tl_eval_boot(T_2026, 0), TL_BOOT_PROVISIONAL, "像样 + 无记录 → PROVISIONAL");

    /* 正常：时间在记录之后 */
    CHECK_EQ(tl_eval_boot(T_2026 + 3600, T_2026), TL_BOOT_RTC_VALID, "晚 1 小时 → RTC_VALID");
    CHECK_EQ(tl_eval_boot(T_2026 + 3 * 86400, T_2026), TL_BOOT_RTC_VALID, "离线 3 天 → 仍 RTC_VALID");

    /* 倒退容差：60 秒内不算倒退（同一次开机前后的抖动） */
    CHECK_EQ(tl_eval_boot(T_2026 - 59, T_2026), TL_BOOT_RTC_VALID, "倒退 59s 容差内");
    CHECK_EQ(tl_eval_boot(T_2026 - 61, T_2026), TL_BOOT_INVALID, "倒退 61s → 判重置");

    /* 放太久：晚一年以上降级 */
    CHECK_EQ(tl_eval_boot(T_2026 + 400LL * 86400, T_2026), TL_BOOT_PROVISIONAL, "晚 400 天 → PROVISIONAL");

    /* 记录本身是坏数据（0 / 太小）时不能参与判定 */
    CHECK_EQ(tl_eval_boot(T_2026, 0), TL_BOOT_PROVISIONAL, "记录为 0 视为无记录");
}

/* ---------------- 网络时间判定 ---------------- */
static void test_net(void)
{
    puts("② 网络时间采纳/拒绝");

    /* 服务器给了 1970 → 坏数据，谁都不能采纳 */
    CHECK_EQ(tl_judge_net(100, T_2026, false), TL_NET_REJECT_LOW, "服务器时间过低 → 拒绝");
    CHECK_EQ(tl_judge_net(100, 100, true),     TL_NET_REJECT_LOW, "本地也无效 + 服务器过低 → 仍拒绝");

    /* 本地无效：差 56 年也要采纳（这是恢复，不是偏差） */
    CHECK_EQ(tl_judge_net(T_2026, 5, true), TL_NET_ACCEPT_IMMEDIATE, "本地 1970 → 立即采纳");

    /* 本地可信：小偏差采纳 / 大偏差拒绝 */
    CHECK_EQ(tl_judge_net(T_2026 + 3,  T_2026, false), TL_NET_ACCEPT, "差 3 秒 → 采纳");
    CHECK_EQ(tl_judge_net(T_2026 + 120, T_2026, false), TL_NET_ACCEPT, "差 2 分钟 → 采纳");
    CHECK_EQ(tl_judge_net(T_2026 + 1799, T_2026, false), TL_NET_ACCEPT, "差 29:59 → 采纳");
    CHECK_EQ(tl_judge_net(T_2026 + 1801, T_2026, false), TL_NET_REJECT_FAR, "差 30:01 → 拒绝");
    CHECK_EQ(tl_judge_net(T_2026 - 3600, T_2026, false), TL_NET_REJECT_FAR, "差 -1 小时 → 拒绝");
    CHECK_EQ(tl_judge_net(T_2026 + 5LL * 365 * 86400, T_2026, false), TL_NET_REJECT_FAR,
             "本地可信但差 5 年 → 拒绝（不覆盖）");
}

/* ---------------- 校准节流 ---------------- */
static void test_schedule(void)
{
    puts("③ 什么时候才做完整校准");

    CHECK_EQ(tl_need_full_sync(true, false, -1, -1), true,  "本地无效 → 必校");
    CHECK_EQ(tl_need_full_sync(false, true, 60, 60), true,  "上次失败 → 允许重试");
    CHECK_EQ(tl_need_full_sync(false, false, 5 * 3600, 5 * 3600), false, "刚校 5 小时 → 跳过");
    CHECK_EQ(tl_need_full_sync(false, false, 6 * 3600, 6 * 3600), true,  "满 6 小时 → 校准");
    CHECK_EQ(tl_need_full_sync(false, false, 120, 120), false, "刚校 2 分钟 → 跳过");
    /* 本次开机还没校过：按持久化记录判断 */
    CHECK_EQ(tl_need_full_sync(false, false, -1, 7 * 3600), true,  "本次未校 + 记录 7 小时 → 校准");
    CHECK_EQ(tl_need_full_sync(false, false, -1, 3600), false, "本次未校 + 记录 1 小时 → 跳过");
    /* 首次开机（无记录） */
    CHECK_EQ(tl_need_full_sync(false, false, -1, -1), true, "无任何记录 → 校一次");
}

int main(void)
{
    puts("=== time_logic 主机测试 ===");
    test_boot();
    test_net();
    test_schedule();
    printf("\n通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    puts(g_fail == 0 ? "全部通过 ✅" : "有失败项 ❌");
    return g_fail == 0 ? 0 : 1;
}
