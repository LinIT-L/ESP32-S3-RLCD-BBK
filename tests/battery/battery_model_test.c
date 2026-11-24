/**
 * battery_model_test.c — 电池算法层主机测试（不需要板子）
 *
 * 编译运行:
 *   cc -O2 -I components/drivers/board_battery/include \
 *      tests/battery/battery_model_test.c \
 *      components/drivers/board_battery/battery_model.c -o /tmp/battery_test
 *   /tmp/battery_test
 *
 * ★ 2026-10-31 精简后，被测对象只剩三件：去极值平均 / 电压查表 / 充电脚去抖。
 *   覆盖: 曲线端点与单调、插值准确度、越界与异常 ADC 值、去极值抗尖峰、
 *         CHRG 去抖与首读采信、睡眠唤醒后无历史残留（新实现本来就没有跨轮状态）。
 */
#include <stdio.h>
#include <string.h>

#include "battery_model.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...) do {                                   \
    if (cond) { g_pass++; }                                          \
    else { g_fail++; printf("  ✗ " fmt "\n", ##__VA_ARGS__); }        \
} while (0)

#define CHECK_EQ(got, want, what) CHECK((got) == (want), \
    "%s: got=%d want=%d (line %d)", what, (int)(got), (int)(want), __LINE__)

/* ---------------- 1 曲线: 端点 / 插值 / 单调 / 越界 ---------------- */
static void test_curve(void)
{
    puts("① 电压→电量: 端点 / 插值 / 单调 / 越界");
    CHECK_EQ(bm_soc_from_mv(4100), 100, "满电 4100");
    CHECK_EQ(bm_soc_from_mv(4500), 100, "超上限仍 100");
    CHECK_EQ(bm_soc_from_mv(3300), 0,   "空电 3300");
    CHECK_EQ(bm_soc_from_mv(3000), 0,   "低于下限仍 0");
    CHECK_EQ(bm_soc_from_mv(0),    0,   "0mV 不崩且为 0");

    /* 表上的点必须精确命中 */
    CHECK_EQ(bm_soc_from_mv(3900), 65, "3.9V → 65%（线性会给 75%）");
    CHECK_EQ(bm_soc_from_mv(3700), 28, "3.7V → 28%（线性会给 50%）");
    CHECK_EQ(bm_soc_from_mv(3820), 50, "3.82V → 50%");

    /* 段内线性插值: 3940..3980 是 72→79，3970 应 = 75 */
    CHECK_EQ(bm_soc_from_mv(3970), 77, "3.97V 段内插值");
    CHECK_EQ(bm_soc_from_mv(3960), 75, "3.96V 段内插值");

    /* 全程单调不减 + 相邻 5mV 的跳变不超过 2%（防止表有空洞/写反） */
    int prev = -1, mono = 1, jump = 0;
    for (uint16_t mv = 3300; mv <= 4100; mv += 5) {
        int p = bm_soc_from_mv(mv);
        if (p < prev) { mono = 0; printf("  ✗ 非单调 @%umV: %d<%d\n", mv, p, prev); break; }
        if (prev >= 0 && p - prev > jump) jump = p - prev;
        prev = p;
    }
    CHECK(mono, "3300..4100 全程单调不减");
    CHECK(jump <= 2, "相邻 5mV 内跳变 <=2%% (实测最大 %d)", jump);
}

/* ---------------- 2 去极值平均: 异常 ADC 值 ---------------- */
static void test_despeckle(void)
{
    puts("② 去极值平均: 尖峰 / 饱和 / 边界");
    int v[8];

    /* 全部相同 → 原值 */
    for (int i = 0; i < 8; i++) v[i] = 1553;
    CHECK_EQ(bm_despeckle_avg(v, 8), 1553, "8 个相同值");

    /* 单个高尖峰（4095 饱和）被丢掉 */
    for (int i = 0; i < 8; i++) v[i] = 1553;
    v[3] = 4095;
    CHECK_EQ(bm_despeckle_avg(v, 8), 1553, "单点 4095 饱和被剔除");

    /* 单个低尖峰（0）被丢掉 */
    for (int i = 0; i < 8; i++) v[i] = 1553;
    v[5] = 0;
    CHECK_EQ(bm_despeckle_avg(v, 8), 1553, "单点 0 被剔除");

    /* 一高一低各 1 个: 正好各丢 1 个 */
    for (int i = 0; i < 8; i++) v[i] = 1550;
    v[0] = 4095; v[7] = 0;
    CHECK_EQ(bm_despeckle_avg(v, 8), 1550, "一高一低各 1 个都被剔除");

    /* 两个高尖峰: 两端各丢 2 个 ⇒ 两个都吃掉（只丢 1 个时均值会偏 75 码 ≈190mV） */
    for (int i = 0; i < 8; i++) v[i] = 1550;
    v[0] = 2000; v[7] = 2000;
    CHECK_EQ(bm_despeckle_avg(v, 8), 1550, "两个高尖峰都被剔除");

    /* 真实压降（整体偏低）如实反映，不被当成尖峰吃掉 */
    for (int i = 0; i < 8; i++) v[i] = 1500;
    CHECK_EQ(bm_despeckle_avg(v, 8), 1500, "整体压降如实反映");

    /* 边界: n<=2 直接平均; n<=0 返回 0; 空指针安全 */
    int two[2] = { 1500, 1600 };
    CHECK_EQ(bm_despeckle_avg(two, 2), 1550, "n=2 直接平均");
    int one[1] = { 1500 };
    CHECK_EQ(bm_despeckle_avg(one, 1), 1500, "n=1 原值");
    CHECK_EQ(bm_despeckle_avg(v, 0), 0, "n=0 返回 0");
    CHECK_EQ(bm_despeckle_avg(NULL, 8), 0, "空指针返回 0");
}

/* ---------------- 3 充电脚去抖 ---------------- */
static void test_charge(void)
{
    puts("③ CHRG 去抖");
    bm_chrg_db_t c;
    bm_chrg_reset(&c);

    /* 上电首读立即采信（原来要等 3 个周期，前 3 分钟诊断一直显示"未充电"） */
    CHECK_EQ(bm_chrg_update(&c, 1), true, "首读 低电平 立即采信");
    bm_chrg_reset(&c);
    CHECK_EQ(bm_chrg_update(&c, 0), false, "首读 高电平 立即采信");

    /* 状态变化需要连续 3 次确认 */
    bm_chrg_reset(&c);
    bm_chrg_update(&c, 0);                 /* 首读: 未充电 */
    CHECK_EQ(bm_chrg_update(&c, 1), false, "第 1 次变化不立即翻转");
    CHECK_EQ(bm_chrg_update(&c, 1), false, "第 2 次仍不翻转");
    CHECK_EQ(bm_chrg_update(&c, 1), true,  "第 3 次确认充电中");

    /* 单次抖动不改变结论 */
    CHECK_EQ(bm_chrg_update(&c, 0), true, "单次抖动不翻转");
    CHECK_EQ(bm_chrg_update(&c, 1), true, "抖回原值仍是充电中");

    /* 本板未接（-1）：恒未充电，且不能把之前的状态留着 */
    bm_chrg_reset(&c);
    bm_chrg_update(&c, 1);
    CHECK_EQ(bm_chrg_update(&c, -1), false, "未接 CHRG 恒报未充电");

    /* 空指针安全 */
    CHECK_EQ(bm_chrg_update(NULL, 1), false, "空指针返回 false");
}

/* ---------------- 4 休眠唤醒: 无历史残留 ---------------- */
static void test_wake_no_history(void)
{
    puts("④ 睡眠/唤醒：第一次采样直接反映当前电压（无跨轮状态可残留）");
    /* 新实现里电量只由"本次电压"决定 —— 用同一个纯函数模拟两次相隔 30 分钟的采样 */
    uint8_t before = bm_soc_from_mv(4050);      /* 睡前: 高 */
    uint8_t after  = bm_soc_from_mv(3700);      /* 醒来: 真实低 */
    CHECK(before > after, "睡前 > 醒来（前提成立）");
    CHECK_EQ(after, 28, "醒来首采直接给真实值，不被睡前值拖住");
    CHECK_EQ(bm_soc_from_mv(4100), 100, "醒来电压升高也立即反映");
}

int main(void)
{
    puts("=== battery_model 主机测试（精简版）===");
    test_curve();
    test_despeckle();
    test_charge();
    test_wake_no_history();

    printf("\n通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    puts(g_fail == 0 ? "全部通过 ✅" : "有失败项 ❌");
    return g_fail == 0 ? 0 : 1;
}
