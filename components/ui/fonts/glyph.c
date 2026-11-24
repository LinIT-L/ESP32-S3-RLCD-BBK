/**
 * glyph.c — 统一汉字字形合成 (实现见 glyph.h)
 *
 * ⚠ 本文件**不碰 NVS**: 守卫生效的分层是 ui → services 单向下行,
 *   fonts(ui 层) 不允许依赖 svc_nvs(services 层)。
 *   字体族的持久化由 page_settings (apps 层) 负责, 开机恢复在 main 的
 *   settings_font_apply() 里做 —— 那里和 settings_apply_dark 是同一个位置。
 */
#include "glyph.h"
#include "font_zh.h"
#include "font_zh16.h"
#include <string.h>

static glyph_fam_t s_fam = GLYPH_FAM_HEI;   /* 默认黑体 = 24px 原生, 与历史观感一致 */
/* ★ 2026-11-22: 字体族作用域。默认关 —— 只有被 OS 显式打开的页面
 *   (主菜单/游戏选择/游戏设置/阅读器) 才会用用户选的族, 其余一律黑体基线。 */
static bool s_fam_scope = false;

glyph_fam_t glyph_get_family(void)
{
    return s_fam;
}

void glyph_set_family(glyph_fam_t f)
{
    if (f >= GLYPH_FAM_MAX) f = GLYPH_FAM_HEI;
    s_fam = f;
}

void glyph_set_family_scope(bool on) { s_fam_scope = on; }
bool glyph_get_family_scope(void)    { return s_fam_scope; }

/* 本次绘制实际生效的字体族: 作用域关着一律黑体 (历史基线)。 */
static glyph_fam_t eff_family(void)
{
    return s_fam_scope ? s_fam : GLYPH_FAM_HEI;
}

/* 单像素取值 / 置位 (1bpp, 高位在左) */
static inline bool src_bit(const uint8_t *b, int stride, int r, int c)
{
    return (b[r * stride + (c >> 3)] & (uint8_t)(0x80u >> (c & 7))) != 0;
}
static inline void dst_bit(uint8_t *b, int stride, int r, int c)
{
    b[r * stride + (c >> 3)] |= (uint8_t)(0x80u >> (c & 7));
}

/* 源 ssz² → 目标 dsz², 每个目标像素 = 对应源区间内所有像素的 OR.
 * 缩小时不丢笔画(只会略粗), 放大时保持笔画连续. */
static void resample_or(const uint8_t *src, int ssz, uint8_t *dst, int dsz)
{
    int sstride = (ssz + 7) >> 3;
    int dstride = (dsz + 7) >> 3;
    for (int dy = 0; dy < dsz; dy++) {
        int y0 = dy * ssz / dsz;
        int y1 = (dy + 1) * ssz / dsz;
        if (y1 <= y0) y1 = y0 + 1;
        for (int dx = 0; dx < dsz; dx++) {
            int x0 = dx * ssz / dsz;
            int x1 = (dx + 1) * ssz / dsz;
            if (x1 <= x0) x1 = x0 + 1;
            bool on = false;
            for (int sy = y0; sy < y1 && !on; sy++)
                for (int sx = x0; sx < x1; sx++)
                    if (src_bit(src, sstride, sy, sx)) { on = true; break; }
            if (on) dst_bit(dst, dstride, dy, dx);
        }
    }
}

bool glyph_is_native(int size)
{
    if (eff_family() == GLYPH_FAM_SONG) return size == 16;
    return size == 24;
}

bool glyph_render(const char *u3, int size, uint8_t *out)
{
    if (!u3 || !out || size <= 0 || size > 48) return false;

    int dstride = (size + 7) >> 3;
    memset(out, 0, (size_t)dstride * size);

    const uint8_t *src = NULL;
    int ssz = 0;

    if (eff_family() == GLYPH_FAM_SONG) {
        /* 宋体: 优先 16px 原生点阵宋; 字库缺字再退 24px */
        int i = font_zh16_find_utf8(u3);
        if (i >= 0) { src = font_zh16_get_bitmap_by_index(i); ssz = 16; }
        else {
            int j = font_zh_find_utf8(u3);
            if (j >= 0) { src = font_zh_get_bitmap_by_index(j); ssz = 24; }
        }
    } else {
        /* 黑体: 优先 24px 原生点阵(笔画粗); 缺字再退 16px */
        int j = font_zh_find_utf8(u3);
        if (j >= 0) { src = font_zh_get_bitmap_by_index(j); ssz = 24; }
        else {
            int i = font_zh16_find_utf8(u3);
            if (i >= 0) { src = font_zh16_get_bitmap_by_index(i); ssz = 16; }
        }
    }
    if (!src) return false;

    if (ssz == size) {
        memcpy(out, src, (size_t)dstride * size);
    } else {
        resample_or(src, ssz, out, size);
    }
    return true;
}
