/**
 * gray_dither.h — 通用「灰度 → 1bpp 抖动」模块
 *
 * 目的 (2026-10-18 linit): 灰度方案只做**一份**, 各个模拟器(GB/GBC/NES/...)把 RGB565 画面
 * 丢进来即可, 不再各自实现阈值/图案表。
 *
 * 用法:
 *   1) 选档位与图案:
 *        gd_cfg_t c = { .levels = 5, .pattern = GD_PATTERN_CLUSTER, .bw = false };
 *        gd_set(&c);
 *   2) 每个像素:
 *        uint8_t shade = gd_shade_rgb565_be(be);      // 0..levels-1
 *        const uint8_t *pat = gd_pattern(shade);      // 2x2 四个点 (0=黑 1=白, ST7305 颜色)
 *
 * 阈值规则: **相邻两档等效亮度的中点**（不是等分）。
 *   5 档(黑点数 0/1/2/3/4): 等效亮度 6375/4781/3188/1594/0
 *     → 分界 5572 / 3980 / 2388 / 796
 *   4 档(黑点数 0/1/2/4):   等效亮度 6375/4781/3188/0
 *     → 分界 5572 / 3980 / 2388
 *   等分会把暗端门限抬高(如 796→1274), 暗绿文字/暗块会被吃成全黑而消失。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GD_PATTERN_CLUSTER = 0,   /* 点聚: 黑点聚成块, 静态不闪 */
    GD_PATTERN_SCATTER = 1,   /* 散点: Bayer 2x2, 层次细但可能微闪 */
} gd_pattern_t;

typedef struct {
    int          levels;     /* 4 或 5 */
    gd_pattern_t pattern;
    bool         bw;         /* true = 纯黑白(只有 0 / 全黑) */
} gd_cfg_t;

/* 设置当前档位/图案 (任一引擎调用一次即可, 全局生效) */
void    gd_set(const gd_cfg_t *cfg);

/* 当前状态 */
int     gd_levels(void);
int     gd_threshold_count(void);
uint16_t gd_threshold(int i);          /* 第 i 条分界 (0..count-1) */

/* RGB565(BE) → 档位索引 (0 = 最亮) */
uint8_t gd_shade_rgb565_be(uint16_t rgb565_be);

/* 档位 → 2x2 图案(4 个 ST7305 颜色, 顺序 左上/右上/左下/右下) */
const uint8_t *gd_pattern(uint8_t shade);

#ifdef __cplusplus
}
#endif
