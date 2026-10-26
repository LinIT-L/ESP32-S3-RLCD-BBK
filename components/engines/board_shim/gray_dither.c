/**
 * gray_dither.c — 通用灰度分档 + 2x2 抖动图案 (GB/GBC/NES/... 共用)
 *
 * 见 gray_dither.h 的规则说明。这里只做两件事:
 *   ① RGB565 → 档位 (按"相邻档中点"分界)
 *   ② 档位 → 2x2 图案
 */
#include <string.h>
#include "gray_dither.h"
#include "st7305.h"      /* ST7305_COLOR_BLACK / WHITE */

/* 档位 → 黑点数 (一个 2x2 里几个黑点) */
static const uint8_t s_dots5[5] = { 0, 1, 2, 3, 4 };   /* 5 档: 0/1/2/3/4 */
static const uint8_t s_dots4[4] = { 0, 1, 2, 4 };      /* 4 档: 0/1/2/4 (原行为) */

/* 点聚图案: 黑点从左上开始往右、往下填 (聚成块) */
static const uint8_t s_pat_cluster[5][4] = {
    { 1, 1, 1, 1 },   /* 0 黑 */
    { 0, 1, 1, 1 },   /* 1 黑: 左上 */
    { 0, 0, 1, 1 },   /* 2 黑: 上排 */
    { 0, 0, 0, 1 },   /* 3 黑: 除右下 */
    { 0, 0, 0, 0 },   /* 4 黑 */
};

/* 散点(Bayer 2x2)图案: 黑点对角扩散, 层次更细 */
static const uint8_t s_pat_scatter[5][4] = {
    { 1, 1, 1, 1 },   /* 0 黑 */
    { 0, 1, 1, 1 },   /* 1 黑: 左上 */
    { 0, 1, 1, 0 },   /* 2 黑: 左上+右下 */
    { 0, 0, 1, 0 },   /* 3 黑: 左上+右下+右上 */
    { 0, 0, 0, 0 },   /* 4 黑 */
};

static gd_cfg_t s_cfg = { .levels = 4, .pattern = GD_PATTERN_CLUSTER, .bw = false };

void gd_set(const gd_cfg_t *cfg)
{
    if (!cfg) return;
    int lv = (cfg->levels == 5) ? 5 : 4;
    s_cfg.levels = lv;
    s_cfg.pattern = cfg->pattern;
    s_cfg.bw = cfg->bw;
}

int gd_levels(void) { return s_cfg.levels; }

int gd_threshold_count(void) { return s_cfg.levels - 1; }

/* 分界 = 相邻两档等效亮度的中点; 等效亮度 = (4 - 黑点数) * 1594 (2x2 四点的平均) */
uint16_t gd_threshold(int i)
{
    const uint8_t *dots = (s_cfg.levels == 5) ? s_dots5 : s_dots4;
    int n = s_cfg.levels;
    if (i < 0 || i >= n - 1) return 0;
    /* 档位 i 的等效亮度(白点数 * 1594), 与档位 i+1 的中点 */
    uint32_t lum_i  = (uint32_t)(4 - dots[i])     * 1594u;
    uint32_t lum_i1 = (uint32_t)(4 - dots[i + 1]) * 1594u;
    return (uint16_t)((lum_i + lum_i1) / 2u);
}

uint8_t gd_shade_rgb565_be(uint16_t rgb565_be)
{
    /* 统一按大端 565 解析 (调用方传的是面板字节序, 这里翻转) */
    uint16_t rgb = (uint16_t)((rgb565_be >> 8) | (rgb565_be << 8));
    uint8_t r5 = (uint8_t)((rgb >> 11) & 0x1FU);
    uint8_t g6 = (uint8_t)((rgb >> 5) & 0x3FU);
    uint8_t b5 = (uint8_t)(rgb & 0x1FU);
    uint16_t gray = (uint16_t)(r5 * 38U + g6 * 75U + b5 * 15U);   /* Rec.601 加权, 0..6375 */

    if (s_cfg.bw) return (gray >= gd_threshold(0)) ? 0 : (uint8_t)(s_cfg.levels - 1);

    int n = s_cfg.levels;
    for (int i = 0; i < n - 1; i++) {
        if (gray >= gd_threshold(i)) return (uint8_t)i;
    }
    return (uint8_t)(n - 1);
}

const uint8_t *gd_pattern(uint8_t shade)
{
    int n = s_cfg.levels;
    if (shade >= n) shade = (uint8_t)(n - 1);
    if (s_cfg.bw) shade = (shade == 0) ? 0 : 4;      /* 纯黑白: 只留最亮/全黑 */
    const uint8_t (*tbl)[4] = (s_cfg.pattern == GD_PATTERN_SCATTER) ? s_pat_scatter : s_pat_cluster;
    return tbl[shade];
}
