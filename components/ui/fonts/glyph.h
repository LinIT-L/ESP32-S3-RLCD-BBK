/**
 * glyph.h — 统一汉字字形入口 (目标像素 → 原生点阵)
 *
 * 2026-11-24 linit「设置里只保留一个字体项」后改定的行为:
 * 系统里只有两套原生点阵字库, 尺寸固定:
 *   · font_zh16.bin  16×16  文泉驿点阵宋  → **16px 的源**
 *   · font_zh.bin    24×24  HZK24S 宋     → **24px 的源**
 *
 *   16px → font_zh16 原生**直拷**
 *   24px → font_zh   原生**直拷**
 *
 * 也就是说「字体:大小」这一项切的是**两套原生字库**, 两个档位都不缩放。
 * (2026-11-22 曾经用"字体族"决定源: 黑体→24px 源、宋体→16px 源, 于是
 *  16px 变成把 24px 缩小、24px 变成把 16px 放大; 宋/黑切换入口删除后,
 *  族不再参与选源, 见 glyph.c 的说明。)
 *
 * 其它字号: ≥20px 用 24px 源, <20px 用 16px 源, 缺字才退另一套再缩放。
 * 缩放一律用「源区间 OR 聚合」, 不用最近邻丢弃:
 *   最近邻在 24→16 时会整行整列跳过, 横画/竖画成条消失(2026-11-22 已实证);
 *   OR 聚合把被覆盖的像素全部并进来, 笔画只会略粗, 绝不会断。
 */
#ifndef FONT_GLYPH_H
#define FONT_GLYPH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GLYPH_FAM_SONG = 0,   /* 16px 源 (文泉驿点阵宋 16) —— 历史枚举值, 已不参与选源 */
    GLYPH_FAM_HEI  = 1,   /* 24px 源 (HZK24S 宋, 笔画较粗) —— 名字是历史误称, 实为宋体 */
    GLYPH_FAM_MAX
} glyph_fam_t;

/* 设置/读取当前字体族. set 会写 NVS 并清空内部缓存. */
void        glyph_set_family(glyph_fam_t f);
glyph_fam_t glyph_get_family(void);

/* ⚠ 2026-11-24: 字体族 API 与作用域开关都**保留但已不影响选源**。
 *   设置里的宋/黑切换入口已删除, 用户无法再选族; 但 os_core 每帧仍会按页面白名单
 *   调一次 glyph_set_family_scope(), main 开机仍会调 glyph_set_family()。
 *   保留它们的唯一理由是"删掉要动 os_core / main / page_settings 三处跨层代码,
 *   收益为零"。取哪个点阵请按 glyph_render() 的目标字号判断, 不要再依赖族。
 *
 *   · glyph_set_family   = 用户曾经选了什么 (NVS font_fam, 已无 UI 入口)
 *   · glyph_family_scope = 当前页面是否在白名单 (os_core 每帧设置, 现已无效果)
 *
 *   默认 false: 任何**没被 OS 显式打开**的绘制路径都落到"不跟随"这一侧。 */
void glyph_set_family_scope(bool on);
bool glyph_get_family_scope(void);

/* 取汉字字形, 合成到 size×size 的 1bpp 位图.
 *   u3   指向 UTF-8 三字节首字节 (调用方保证是 3 字节汉字)
 *   size 目标像素边长 (16 / 24, 其它值也支持但无实际意义)
 *   out  输出缓冲, 尺寸 = ((size+7)/8) * size 字节, 高位在左, 1 = 前景
 * 返回 false 表示两套字库都没有该字 (out 已被清零)。
 * 直拷路径 (源尺寸 == size) 输出与原字库完全一致。 */
bool glyph_render(const char *u3, int size, uint8_t *out);

/* 直拷快捷判断: 当前字体族在该尺寸下是否走原生直拷 (无缩放)。
 * 供渲染层决定要不要走更快的老路径。 */
bool glyph_is_native(int size);

#ifdef __cplusplus
}
#endif

#endif /* FONT_GLYPH_H */
