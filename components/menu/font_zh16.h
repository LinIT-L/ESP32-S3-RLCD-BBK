#ifndef FONT_ZH16_H
#define FONT_ZH16_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* V1.0.86: 软件介绍用文泉驿点阵宋体 16px 紧凑二进制字库 (font_zh16.bin, Flash XIP)
 * 布局: [8B magic "ZH1FNT01"][u32 count][字符表 count*3][字形 count*32] */
extern const char *zh16_chars;                 /* 字符表 (每字 3 字节 UTF-8) */
extern const uint8_t (*zh16_font_data)[32];    /* 字形表 (每字 32 字节) */

#define ZH16_FONT_W 16
#define ZH16_FONT_H 16
#define ZH16_FONT_BYTES_PER_CHAR 32

int font_zh16_count(void);
/* 通过 UTF-8 字节序列查找字符索引 (比较 3 字节), 找不到返回 -1 */
int font_zh16_find_utf8(const char *str);
/* 通过字符索引获取位图数据 */
const uint8_t *font_zh16_get_bitmap_by_index(int idx);

#ifdef __cplusplus
}
#endif

#endif