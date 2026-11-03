/**
 * battery_model.c — 电池纯算法实现（无 IDF 依赖，可主机测试）
 * 设计说明见 battery_model.h。
 *
 * 只有三个函数，加起来不到 100 行 —— 这是刻意的：
 * 电池显示的问题大多出在"算法叠算法"，而不是"算法不够"。
 */
#include "battery_model.h"

#include <string.h>

/* ==================== ① 去极值平均 ==================== */

int bm_despeckle_avg(const int *v, int n)
{
    if (!v || n <= 0) return 0;
    if (n <= 2) {                       /* 太短，没有可丢的极值 */
        int s = 0;
        for (int i = 0; i < n; i++) s += v[i];
        return s / n;
    }
    if (n > 16) n = 16;                 /* 防御：本工程只用 8 */

    /* 插入排序（n=8，栈上 64B） */
    int s[16];
    for (int i = 0; i < n; i++) {
        int x = v[i], j = i - 1;
        while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; }
        s[j + 1] = x;
    }

    /* 两端各丢 k 个再平均。k=2（8 采样 ⇒ 取中间 4 个）而不是 1：
     * 实测"两个坏点"时只丢 1 个会让均值偏 75 个 ADC 码（≈190mV 电池电压），
     * 而 ADC 本身的随机噪声只有 ±2~3 码 —— 少 2 个样本换掉 190mV 的误判，划算。 */
    int k = (n >= 6) ? 2 : 1;
    if (n - 2 * k < 2) k = 1;
    int sum = 0, cnt = 0;
    for (int i = k; i < n - k; i++) { sum += s[i]; cnt++; }
    return (cnt > 0) ? (sum / cnt) : s[n / 2];
}

/* ==================== ② 电压 → 电量 ====================
 * 单节锂电典型放电曲线（25°C、低倍率），端点对齐本机口径 4100=100% / 3300=0%。
 * 表按电压从高到低排列；相邻点之间整数线性插值。
 * ⚠ 改表必须同步更新 tests/battery 的期望值，并在文档里说明依据。
 */
static const struct { uint16_t mv; uint8_t pct; } k_ocv[] = {
    { 4100, 100 },
    { 4060,  93 },
    { 4020,  86 },
    { 3980,  79 },
    { 3940,  72 },
    { 3900,  65 },
    { 3860,  57 },
    { 3820,  50 },
    { 3780,  42 },
    { 3740,  35 },
    { 3700,  28 },
    { 3660,  21 },
    { 3620,  15 },
    { 3560,   9 },
    { 3480,   4 },
    { 3400,   1 },
    { 3300,   0 },
};
#define BM_OCV_N (sizeof(k_ocv) / sizeof(k_ocv[0]))

uint8_t bm_soc_from_mv(uint16_t mv)
{
    if (mv >= BM_MV_FULL)  return 100;
    if (mv <= BM_MV_EMPTY) return 0;

    for (unsigned i = 0; i + 1 < BM_OCV_N; i++) {
        if (mv < k_ocv[i].mv && mv >= k_ocv[i + 1].mv) {
            int dv = (int)k_ocv[i].mv - (int)k_ocv[i + 1].mv;
            int dp = (int)k_ocv[i].pct - (int)k_ocv[i + 1].pct;
            if (dv <= 0) return k_ocv[i + 1].pct;      /* 表写错时的兜底，不除零 */
            int up = (int)mv - (int)k_ocv[i + 1].mv;
            return (uint8_t)(k_ocv[i + 1].pct + (up * dp) / dv);
        }
    }
    return 0;                                          /* 表空隙：安全兜底 */
}

/* ==================== ③ CHRG 去抖 ==================== */

void bm_chrg_reset(bm_chrg_db_t *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->prev = -1;
}

bool bm_chrg_update(bm_chrg_db_t *c, int level)
{
    if (!c) return false;

    /* 本板没接 CHRG：恒为"未充电"。不做任何电压推断（那会把负载变化误判成充电）。 */
    if (level < 0) {
        c->prev  = -1;
        c->same  = 0;
        c->state = false;
        return false;
    }

    int8_t lv = (int8_t)(level ? 1 : 0);

    if (lv != c->prev) {
        bool first = (c->prev < 0);      /* 上电第一次读到：先采信，别等 3 个周期 */
        c->prev = lv;
        c->same = 1;
        if (first) c->state = (lv == 1);
    } else if (c->same < 0xFF) {
        c->same++;
        if (c->same >= BM_CHRG_DEBOUNCE) c->state = (lv == 1);
    }
    return c->state;
}
