/**
 * glyph.h — 统一汉字字形入口 (字体族 × 目标像素)
 *
 * 2026-11-22 linit「设置里加字体修改: 宋体/黑体 + 16px/24px」
 *
 * 背景: 系统里只有两套原生点阵字库, 尺寸固定, 不能凭空多出 22px 那种档位:
 *   · font_zh16.bin  16×16  文泉驿点阵宋  → **宋体的源**
 *   · font_zh.bin    24×24  HZK24S (笔画粗) → **黑体的源**
 * 因此四个组合按下面的对应关系合成, 不新增任何字库、不占额外 flash:
 *
 *   宋体 16px = 16px 原生直拷
 *   宋体 24px = 16px **放大**到 24      ← linit 指定
 *   黑体 24px = 24px 原生直拷
 *   黑体 16px = 24px **缩小**到 16      ← linit 指定
 *
 * 缩放一律用「源区间 OR 聚合」, 不用最近邻丢弃:
 *   最近邻在 24→16 时会整行整列跳过, 横画/竖画成条消失(2026-11-22 已实证);
 *   OR 聚合把被覆盖的像素全部并进来, 笔画只会略粗, 绝不会断。
 *
 * 字体族在首次调用时从 NVS 懒加载, 改完设置立刻生效, 不需要任何初始化调用点。
 */
#ifndef FONT_GLYPH_H
#define FONT_GLYPH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GLYPH_FAM_SONG = 0,   /* 宋体: 以 16px 原生点阵宋为源 */
    GLYPH_FAM_HEI  = 1,   /* 黑体: 以 24px 原生点阵(笔画粗)为源 */
    GLYPH_FAM_MAX
} glyph_fam_t;

/* 设置/读取当前字体族. set 会写 NVS 并清空内部缓存. */
void        glyph_set_family(glyph_fam_t f);
glyph_fam_t glyph_get_family(void);

/* ★ 2026-11-22 linit「字体切换只作用于主菜单 / 游戏选择引擎菜单 / 阅读器,
 *   其他内置应用和 Lua 应用一律不动」。
 *
 *   这是**作用域开关**, 和上面的字体族是两个独立维度:
 *     · glyph_set_family   = 用户选了什么 (持久化, 长期有效)
 *     · glyph_family_scope = 当前这段绘制允不允许用用户选的那个族
 *
 *   由 OS 层在每帧渲染前按当前页面设一次 (os_render)。scope=false 时
 *   glyph_render 一律按黑体(= 24px 原生字库, 也就是字体切换功能上线前的
 *   唯一行为) 出字, 用户在设置里切成宋体也不会渗透到这些页面。
 *
 *   默认 false: 任何**没被 OS 显式打开**的绘制路径 (含开机首帧、异常兜底)
 *   都退回历史基线, 不会出现"切了宋体结果全系统都变"的事故。 */
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
