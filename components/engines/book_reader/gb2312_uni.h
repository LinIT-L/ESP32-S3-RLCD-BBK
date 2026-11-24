/* gb2312_uni.h — GB2312 四区码 → Unicode 转换表 (电子书改复用 UI 字体后,
 * 供 font_book_glyph_gb 把 GBK 双字节码换算成 Unicode 以查 font_zh/font_zh16).
 * 表: gb2312_to_uni[hi-0xA1][lo-0xA1]; 值 0 表示该码位无字(GB2312 扩展区不覆盖).
 * 由 python3 bytes([hi,lo]).decode('gb2312') 生成, 开发机一次性产出. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint16_t gb2312_to_uni[94][94];

#ifdef __cplusplus
}
#endif
