/* gray_quant.h — LinTOS 灰阶量化统一入口 (GB/GBC/NES 共用)
 *
 * 为什么要有这个文件:
 *   改之前, 两套引擎各自持有一份"2x2 图案表":
 *     - GBC: board_shim.c 的 s_tune_pat[4][4] (按 shade 取 4 个象限)
 *     - NES: board_shim.c 的 s_rowbits[4][2][16] (把 2x2 图案预先拼成字节)
 *   两者的**图案来源**是同一份, 但"怎么用"各写一遍。要加第二种算法(Bayer)时,
 *   如果两边各写一套, 必然出现"GBC 用 Bayer / NES 还是点聚"的不一致。
 *   所以把"**给定灰阶 + 屏幕位置 → 2x2 块的 4 个点**"抽到这里, 两个引擎都只读它。
 *
 * 约定(与 ST7305 fb 一致):
 *   - 颜色值用 ST7305 的语义: 0 = 黑, 1 = 白 (见 st7305.h 的 ST7305_COLOR_*);
 *   - 4 个 cell 的顺序固定为 [左上, 右上, 左下, 右下] = 索引 (dy<<1)|dx;
 *   - 位置用**绝对屏幕坐标的源像素**(sx, sy): 物理块 = (2sx..2sx+1) x (2sy..2sy+1)。
 *     用屏幕坐标而不是"本行第几个像素", 图案才不会随裁剪/滚动/局部绘制跳变。
 *
 * 两种算法:
 *   GRAY_ALG_CLUSTER_2X2 (默认, 原实现): 黑点按固定顺序聚成块/散开, 与位置无关。
 *   GRAY_ALG_BAYER_4X4 (实验): 4x4 有序 Bayer 阈值, 黑点按位置分散 ⇒ 大面积
 *     中间灰更平滑; 代价是极小块区域可能"块内点数不均"(见 §报告)。
 *     填充率与 CLUSTER **完全相同**(都等于 dots[shade]/4), 只有空间分布不同,
 *     所以 A/B 对比时整体亮度不变, 不会因为"换了算法所以看起来更亮"而误判。
 *
 * 本文件不依赖 ESP-IDF, 主机测试可直接编译 (tests/gray_quant)。
 */
#ifndef GRAY_QUANT_H
#define GRAY_QUANT_H

#include <stdint.h>

typedef enum {
    GRAY_ALG_CLUSTER_2X2 = 0,   /* 原实现: 点聚/散点 (由 board_shim 的 cluster/scatter 决定顺序) */
    GRAY_ALG_BAYER_4X4   = 1,   /* 实验: 4x4 Bayer 有序抖动 */
} gray_alg_t;

/* 当前算法 (默认 CLUSTER_2X2)。切换后调用方必须重建自己的表并强制整屏重画。 */
gray_alg_t gray_quant_alg(void);
void        gray_quant_set_alg(gray_alg_t alg);

/* 每档灰的"黑点数"(0..4), 来自 vid_tune 的用户曲线。
 * Bayer 会把它换算成 0..16 的填充率(dots*4), 与 2x2 的 k/4 等价。 */
void gray_quant_set_density(const uint8_t dens[4]);

/* 重建内部表 (set_alg / set_density 之后必须调一次)。 */
void gray_quant_rebuild(void);

/* Bayer 模式下, 源像素 (sx,sy) 处 shade 档的 2x2 图案指针 (4 字节, 0=黑 1=白)。
 * 仅在 alg == GRAY_ALG_BAYER_4X4 时有意义; CLUSTER 模式请用调用方自己的表。 */
const uint8_t *gray_quant_pat(uint8_t shade, int sx, int sy);

/* 供测试/诊断: 4x4 Bayer 矩阵的阈值 (0..15)。 */
uint8_t gray_quant_bayer(int x, int y);

#endif /* GRAY_QUANT_H */
