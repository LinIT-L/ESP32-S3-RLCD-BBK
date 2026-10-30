/* =====================================================================
 * gray_quant_test.c — 灰阶量化模块的主机侧验证
 *
 * 直接编译生产源码 components/engines/board_shim/gray_quant.c（无 ESP 依赖）。
 * 覆盖：Bayer 阈值的完整性、极值不产生噪点、填充率与 dots 曲线等价、
 *       坐标稳定性（图案不随局部绘制/滚动跳变）、块内位序。
 *
 * 编译：
 *   CC=/Library/Developer/CommandLineTools/usr/bin/clang
 *   SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
 *   INC=components/engines/board_shim/include
 *   $CC -isysroot $SDK -O2 -std=c11 -Wall -Wextra -I $INC \
 *       tests/gray_quant/gray_quant_test.c components/engines/board_shim/gray_quant.c \
 *       -o /tmp/gray_quant_test && /tmp/gray_quant_test
 * ===================================================================== */
#include "gray_quant.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, fmt, ...) do {                                        \
    if (cond) { g_pass++; printf("  PASS  " fmt "\n", ##__VA_ARGS__); }   \
    else      { g_fail++; printf("  FAIL  " fmt "\n", ##__VA_ARGS__); }   \
} while (0)

/* 取某源像素块里 4 个格子的黑点数 (0=黑 1=白 ⇒ 黑点数 = 4 - 白点数) */
static int block_black(uint8_t shade, int sx, int sy)
{
    const uint8_t *p = gray_quant_pat(shade, sx, sy);
    int n = 0;
    for (int i = 0; i < 4; i++) if (p[i] == 0) n++;
    return n;
}

int main(void)
{
    printf("=== 灰阶量化模块 · 主机测试（直接编译生产源码）===\n");
    printf("被测实现: components/engines/board_shim/gray_quant.c\n\n");

    /* ---------- T1 Bayer 矩阵是 0..15 的一个排列 ---------- */
    printf("T1 Bayer 4x4 阈值完整性\n");
    int seen[16] = {0};
    int dup = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int v = gray_quant_bayer(x, y);
            if (v < 0 || v > 15) dup++;
            else if (seen[v]++) dup++;
        }
    CHECK(dup == 0, "16 个阈值互不相同且都在 0..15 (异常 %d)", dup);
    CHECK(gray_quant_bayer(0, 0) == 0, "左上角是最小阈值 0 (暗部优先落点)");
    printf("\n");

    /* ---------- T2 极值：密度 0 全白、密度 4 全黑，且与位置无关 ---------- */
    printf("T2 纯黑/纯白边界\n");
    uint8_t dens[4] = { 0, 1, 3, 4 };
    gray_quant_set_alg(GRAY_ALG_BAYER_4X4);
    gray_quant_set_density(dens);
    gray_quant_rebuild();

    int white_bad = 0, black_bad = 0;
    for (int sy = 0; sy < 8; sy++)
        for (int sx = 0; sx < 8; sx++) {
            if (block_black(0, sx, sy) != 0) white_bad++;   /* shade0 = 密度0 → 全白 */
            if (block_black(3, sx, sy) != 4) black_bad++;   /* shade3 = 密度4 → 全黑 */
        }
    CHECK(white_bad == 0, "shade0 在所有位置 0 个黑点 (无噪点), 违例 %d", white_bad);
    CHECK(black_bad == 0, "shade3 在所有位置 4 个黑点 (纯黑), 违例 %d", black_bad);
    printf("\n");

    /* ---------- T3 填充率 = 密度 x 4 / 16（与 2x2 点聚等价） ---------- */
    printf("T3 填充率与 dots 曲线等价（4x4 物理区域 = 2x2 个源像素块）\n");
    for (int sh = 0; sh < 4; sh++) {
        int black = 0;
        for (int sy = 0; sy < 2; sy++)
            for (int sx = 0; sx < 2; sx++)
                black += block_black((uint8_t)sh, sx, sy);
        int expect = dens[sh] * 4;
        CHECK(black == expect, "shade%d: 16 个物理点里 %d 黑, 期望 %d", sh, black, expect);
    }
    printf("\n");

    /* ---------- T4 坐标稳定性：平移 2 个源像素(4 个物理像素)图案复现 ---------- */
    printf("T4 坐标稳定性（同一绝对位置的图案恒定, 不随调用顺序变化）\n");
    int unstable = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
            for (int sh = 1; sh < 3; sh++) {
                const uint8_t *a = gray_quant_pat((uint8_t)sh, sx, sy);
                uint8_t first[4];
                memcpy(first, a, 4);
                for (int k = 0; k < 5; k++) {          /* 反复取, 必须一模一样 */
                    const uint8_t *b = gray_quant_pat((uint8_t)sh, sx, sy);
                    if (memcmp(first, b, 4) != 0) unstable++;
                }
                const uint8_t *p2 = gray_quant_pat((uint8_t)sh, sx + 2, sy);
                const uint8_t *p4 = gray_quant_pat((uint8_t)sh, sx, sy + 2);
                if (memcmp(first, p2, 4) != 0) unstable++;   /* 周期 = 2 源像素 */
                if (memcmp(first, p4, 4) != 0) unstable++;
            }
    CHECK(unstable == 0, "图案只由绝对坐标决定且以 2 源像素为周期 (异常 %d)", unstable);
    printf("\n");

    /* ---------- T5 中间档空间分布：Bayer 到底有没有带来差别 ---------- */
    printf("T5 中间档空间分布（Bayer 的价值取决于曲线给出哪些填充率）\n");
    /* 事实(被测出来的)：4x4 Bayer 的阈值有"格点"性质 —— 0..3 落在偶/偶，12..15 落在偶/奇。
     *   ⇒ d=4 和 d=12 时, 每块的点数与"哪一格是白"都**与块位置无关**, 与点聚同构;
     *   只有 d=8(即 dots=2, 50%%) 这种中间档才会随块变化(棋盘), 这才是 Bayer 真正的差别。
     * 这直接决定"默认曲线 dots{0,1,3,4} 下 Bayer 是否值得开" —— 报告里按这个结论写。 */

    int bad1 = 0, same_pos_4 = 1, same_pos_12 = 1, vary_pos_8 = 0;
    int bm4 = 0, wm12 = 0, wm8 = 0;   /* d=4 看黑点位置, d=12 看白点位置 */

    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            const uint8_t *p1 = gray_quant_pat(1, sx, sy);    /* d=4  */
            const uint8_t *p2 = gray_quant_pat(2, sx, sy);    /* d=12 */
            int n1 = 0, n2 = 0;
            for (int i = 0; i < 4; i++) {
                if (p1[i] == 0) { n1++; bm4 |= (1 << i); }
                if (p2[i] == 0) n2++; else wm12 |= (1 << i);
            }
            if (n1 != 1) bad1++;
            if (n2 != 3) bad1++;
        }
    {
        int c4 = 0, c12 = 0;
        for (int i = 0; i < 4; i++) { if (bm4 & (1 << i)) c4++; if (wm12 & (1 << i)) c12++; }
        same_pos_4  = (c4 == 1);
        same_pos_12 = (c12 == 1);
    }
    CHECK(bad1 == 0, "d=4 每块 1 黑、d=12 每块 3 黑 (违例 %d)", bad1);
    CHECK(same_pos_4,  "d=4 : 黑点落在每块同一格(与点聚同构)");
    CHECK(same_pos_12, "d=12: 白点落在每块同一格(与点聚同构)");

    /* d=8 (dots=2, 50%) —— 只有这种中间档才体现 Bayer */
    {
        uint8_t d2[4] = { 0, 1, 2, 4 };
        gray_quant_set_density(d2);
        gray_quant_rebuild();
        int bad = 0;
        for (int sy = 0; sy < 4; sy++)
            for (int sx = 0; sx < 4; sx++) {
                const uint8_t *p = gray_quant_pat(2, sx, sy);
                int n = 0;
                for (int i = 0; i < 4; i++) { if (p[i] == 0) n++; else wm8 |= (1 << i); }
                if (n != 2) bad++;
            }
        int c8 = 0;
        for (int i = 0; i < 4; i++) if (wm8 & (1 << i)) c8++;
        vary_pos_8 = (c8 >= 2);
        CHECK(bad == 0, "d=8 每块恰好 2 黑 (违例 %d)", bad);
        CHECK(vary_pos_8, "d=8 : 白点位置随块变化 ⇒ 50%% 灰呈棋盘而非规则网纹 (出现 %d 种)", c8);
    }
    gray_quant_set_density(dens);
    gray_quant_rebuild();
    printf("        ⚠ 默认曲线 dots{0,1,3,4} ⇒ d 只有 {0,4,12,16}，**不经过 d=8**\n");
    printf("          ⇒ 默认曲线下 Bayer 与点聚视觉等价(仅排列不同)，报告按此结论写\n");
    printf("\n");

    /* ---------- T6 算法切换与默认值 ---------- */
    printf("T6 算法切换\n");
    gray_quant_set_alg(GRAY_ALG_CLUSTER_2X2);
    CHECK(gray_quant_alg() == GRAY_ALG_CLUSTER_2X2, "切到 CLUSTER_2X2");
    gray_quant_set_alg(GRAY_ALG_BAYER_4X4);
    CHECK(gray_quant_alg() == GRAY_ALG_BAYER_4X4, "切到 BAYER_4X4");
    gray_quant_set_alg((gray_alg_t)99);
    CHECK(gray_quant_alg() == GRAY_ALG_CLUSTER_2X2, "非法值回落到 CLUSTER (防误配)");
    printf("\n");

    /* ---------- T7 密度夹紧 ---------- */
    printf("T7 密度参数防御\n");
    uint8_t big[4] = { 9, 7, 255, 4 };
    gray_quant_set_density(big);
    gray_quant_set_alg(GRAY_ALG_BAYER_4X4);
    gray_quant_rebuild();
    CHECK(block_black(0, 0, 0) == 4, "密度 9 被夹成 4 ⇒ 全黑");
    CHECK(block_black(2, 0, 0) == 4, "密度 255 被夹成 4 ⇒ 全黑");
    CHECK(block_black(3, 0, 0) == 4, "密度 4 原样 ⇒ 全黑");
    printf("\n");

    printf("================ 结果: %d PASS / %d FAIL ================\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
