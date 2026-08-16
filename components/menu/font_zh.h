#ifndef FONT_ZH_H
#define FONT_ZH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* V1.0.64: 字库改为紧凑二进制 (font_zh.bin, Flash XIP),
 * 布局: [8B magic][u32 count][字符表 count*3][字形 count*72] */
extern const char *zh_chars;                 /* 字符表 (每字 3 字节 UTF-8) */
extern const uint8_t (*zh_font_data)[72];    /* 字形表 (每字 72 字节) */

#define ZH_FONT_W 24
#define ZH_FONT_H 24
#define ZH_FONT_BYTES_PER_CHAR 72
int font_zh_count(void);                     /* 字库字符数 */

/* 通过 UTF-8 字节序列查找字符索引 (正确做法: 比较 3 字节)
 * str: 指向 UTF-8 字符首字节
 * 返回: 索引 (0..ZH_FONT_COUNT-1), 找不到返回 -1
 */
int font_zh_find_utf8(const char *str);

/* 通过字符索引获取位图数据 */
const uint8_t *font_zh_get_bitmap_by_index(int idx);

/* 旧接口 (已废弃 - 保留兼容) */
int font_zh_find_char(char c);
const uint8_t *font_zh_get_bitmap(char c);

#ifdef __cplusplus
}
#endif

#endif
