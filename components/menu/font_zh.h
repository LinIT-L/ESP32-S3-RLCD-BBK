#ifndef FONT_ZH_H
#define FONT_ZH_H

#include <stdint.h>
#include <stdbool.h>

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

/* V1.0.89: 切换字重 — light=false 用粗体(默认), light=true 用细体.
 * 下次任何取字操作按新字库绑定; 调用方再触发重绘即可. */
void font_zh_set_style(bool light);

/* 当前是否使用细字库 */
bool font_zh_is_light(void);

/* V1.0.92: 查找字库中 ASCII 字符(英文/数字/符号)的索引, 找不到返回 -1
 * 字库中 ASCII 以 "0x00 0x00 <ascii>" 三字节槽存储, 跟随粗/细字库(冬青 W6/W3)样式. */
int font_zh_find_ascii(unsigned char c);

/* 旧接口 (已废弃 - 保留兼容) */
int font_zh_find_char(char c);
const uint8_t *font_zh_get_bitmap(char c);

#ifdef __cplusplus
}
#endif

#endif
