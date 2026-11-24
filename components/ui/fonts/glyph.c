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

/* 2026-11-24 linit 更正: 两套字库**都是宋体** ——
 *   16px 源 = font_zh16.bin 文泉驿点阵宋体 16px
 *   24px 源 = font_zh.bin   HZK24S 宋体 (笔画较粗)
 * 历史代码把 24px 那套叫"黑体"是错的 (固件里从来没有过黑体字库),
 * 这里连同注释一起改回宋体。枚举名 GLYPH_FAM_HEI 因被多处引用 (含 NVS 值 1)
 * 暂不改名, 但已不参与选源, 见下面 glyph_render 上方的说明。 */
static glyph_fam_t s_fam = GLYPH_FAM_HEI;   /* 默认=24px 宋体源 (历史基线, 已不影响选源) */
/* 字体族作用域。默认关 —— 只有被 OS 显式打开的页面才会用用户选的族。
 * ★ 2026-11-24: 族已不参与选源, 这个开关现在只剩"记录 os_core 判定结果"的作用。 */
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

/* ★ 2026-11-24 linit: 选源不再看字体族, 改按**目标字号**取最近的原生点阵。
 *
 *   旧行为 (2026-11-22): 族=SONG → 源 16px, 族=HEI → 源 24px。默认族是 HEI,
 *   于是「16px」永远是把 24px 点阵**缩小**, 用户看到的是缩小版而不是原生 16px 宋体;
 *   反过来切成 SONG 时 24px 是把 16px **放大**。两个字号本来各自都有原生点阵,
 *   却被族绑成"只能好一个"。
 *
 *   设置里的宋/黑切换入口已删除 (2026-11-24), 字体族不再有用户可见含义,
 *   所以这里干脆按字号选源:
 *     目标 < 20px → font_zh16 (16px 原生, 文泉驿点阵宋)
 *     目标 ≥ 20px → font_zh   (24px 原生, HZK24S 宋)
 *   两档都命中原生字库直拷, 任何缩放都不再发生; 缺字才退另一套再缩放。
 *
 *   glyph_set_family / set_family_scope 两个 API 保留 (os_core 的页面白名单
 *   与 main 的开机恢复仍在调用), 只是不再影响取哪个源。 */

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
    /* 16 / 24 两档都直接命中各自的原生字库, 与字体族无关 (2026-11-24)。 */
    return size == 16 || size == 24;
}

bool glyph_render(const char *u3, int size, uint8_t *out)
{
    if (!u3 || !out || size <= 0 || size > 48) return false;

    int dstride = (size + 7) >> 3;
    memset(out, 0, (size_t)dstride * size);

    const uint8_t *src = NULL;
    int ssz = 0;

    /* 选源: 目标 ≥20px 用 24px 原生点阵, <20px 用 16px 原生点阵; 缺字退另一套。 */
    if (size >= 20) {
        int j = font_zh_find_utf8(u3);
        if (j >= 0) { src = font_zh_get_bitmap_by_index(j); ssz = 24; }
        else {
            int i = font_zh16_find_utf8(u3);
            if (i >= 0) { src = font_zh16_get_bitmap_by_index(i); ssz = 16; }
        }
    } else {
        int i = font_zh16_find_utf8(u3);
        if (i >= 0) { src = font_zh16_get_bitmap_by_index(i); ssz = 16; }
        else {
            int j = font_zh_find_utf8(u3);
            if (j >= 0) { src = font_zh_get_bitmap_by_index(j); ssz = 24; }
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
