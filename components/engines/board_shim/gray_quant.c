/* gray_quant.c — 见 gray_quant.h。纯 C, 不依赖 ESP-IDF (主机测试直接编译)。 */
#include "gray_quant.h"

/* 4x4 有序 Bayer 矩阵 (标准形式, 值 0..15, 覆盖全部 16 个阈值各一次)。
 * 用绝对坐标取模, 不使用随机数、不做时间抖动 ⇒ 静态画面图案稳定, 不会闪。 */
static const uint8_t kBayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static gray_alg_t s_alg = GRAY_ALG_CLUSTER_2X2;
/* 默认密度 = vid_tune 默认曲线 dots{0,1,3,4}; board_shim 会用真实曲线覆盖。 */
static uint8_t s_dens[4] = { 0, 1, 3, 4 };

/* [shade][xph][yph][cell], cell 顺序 (dy<<1)|dx = [左上,右上,左下,右下]
 * xph = sx & 1, yph = sy & 1 (源像素奇偶)。16 项 x 4 字节 = 64 B。 */
static uint8_t s_pat[4][2][2][4];

gray_alg_t gray_quant_alg(void) { return s_alg; }

void gray_quant_set_alg(gray_alg_t alg)
{
    s_alg = (alg == GRAY_ALG_BAYER_4X4) ? GRAY_ALG_BAYER_4X4 : GRAY_ALG_CLUSTER_2X2;
}

void gray_quant_set_density(const uint8_t dens[4])
{
    if (!dens) return;
    for (int i = 0; i < 4; i++) s_dens[i] = (uint8_t)(dens[i] > 4 ? 4 : dens[i]);
}

uint8_t gray_quant_bayer(int x, int y)
{
    return kBayer4[((unsigned)y) & 3U][((unsigned)x) & 3U];
}

void gray_quant_rebuild(void)
{
    for (int sh = 0; sh < 4; sh++) {
        /* 填充率 0..16: 与 2x2 的 dots/4 等价 ⇒ 平均亮度不变, 只有分布不同。
         * d=0 全白, d=16 全黑 (16 > 任何阈值) ⇒ 纯黑纯白严格保持, 不会出噪点。 */
        const unsigned d = (unsigned)s_dens[sh] * 4U;
        for (int xph = 0; xph < 2; xph++) {
            for (int yph = 0; yph < 2; yph++) {
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        /* 物理坐标 = 块原点(2sx,2sy) + 单元偏移; 对 4 取模即得阈值位置。
                         * xph/yph 已含 sx/sy 的奇偶, 所以这里只需 (2*xph+dx) & 3。 */
                        const unsigned X = (unsigned)(2 * xph + dx) & 3U;
                        const unsigned Y = (unsigned)(2 * yph + dy) & 3U;
                        const unsigned th = kBayer4[Y][X];
                        /* 黑 = 填充率 > 阈值 (d 个点恰好落在阈值最小的 d 个格子) */
                        s_pat[sh][xph][yph][(dy << 1) | dx] = (d > th) ? 0 : 1;
                    }
                }
            }
        }
    }
}

const uint8_t *gray_quant_pat(uint8_t shade, int sx, int sy)
{
    return s_pat[shade & 3U][((unsigned)sx) & 1U][((unsigned)sy) & 1U];
}
