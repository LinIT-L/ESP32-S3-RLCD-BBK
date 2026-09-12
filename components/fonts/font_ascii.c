/* font_ascii.c — ASCII 8x16 等宽点阵 (标准 VGA, 数据来自 font8x16_zh.h).
 * 0x20-0x7E 共 95 字符, 每字符 16 字节 (每行 1 字节 MSB 左).
 * 为兼容旧引用仍用符号名 FONT8X12; 实际高度 16 (与 16px 中文字体等高). */
#include "font_zh.h"
#include "font8x16_zh.h"

typedef const uint8_t (*ascii16_row_t)[16];
ascii16_row_t FONT8X12 = (ascii16_row_t)font8x16_zh;