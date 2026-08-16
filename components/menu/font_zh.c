#include "font_zh.h"
#include <string.h>
#include <stdbool.h>

/* V1.0.64: 紧凑二进制字库 (components/menu/font_zh.bin, 由 CMake target_add_binary_data 嵌入)
 * 布局: [8B magic "ZH1FNT01"][u32 count][字符表 count*3 UTF-8][字形 count*72] */
extern const uint8_t font_zh_bin_start[] asm("_binary_font_zh_bin_start");
extern const uint8_t font_zh_bin_end[]   asm("_binary_font_zh_bin_end");

static uint32_t s_zh_count = 0;
static const uint8_t *s_zh_chars = NULL;   /* 字符表 (count*3) */
static const uint8_t *s_zh_glyphs = NULL;  /* 字形 (count*72) */
static bool s_zh_bound = false;

const char *zh_chars = NULL;
const uint8_t (*zh_font_data)[72] = NULL;

static void font_zh_bind(void) {
    const uint8_t *d = font_zh_bin_start;
    size_t len = (size_t)(font_zh_bin_end - font_zh_bin_start);
    if (len < 12 || memcmp(d, "ZH1FNT01", 8) != 0) return;
    uint32_t n = (uint32_t)d[8] | ((uint32_t)d[9] << 8) |
                 ((uint32_t)d[10] << 16) | ((uint32_t)d[11] << 24);
    if (n == 0 || n > 20000) return;
    if (len < 12 + (size_t)n * 3 + (size_t)n * 72) return;
    s_zh_count = n;
    s_zh_chars = d + 12;
    s_zh_glyphs = d + 12 + (size_t)n * 3;
    zh_chars = (const char *)s_zh_chars;
    zh_font_data = (const uint8_t (*)[72])s_zh_glyphs;
    s_zh_bound = true;
}

int font_zh_count(void) {
    if (!s_zh_bound) font_zh_bind();
    return (int)s_zh_count;
}

/* UTF-8 中文字符在 zh_chars[] 中是 3 字节序列.
 * zh_chars 是 "步步高..." 这样的字符串, 每个中文占 3 字节.
 * 所以第 i 个字符的字节起始位置是 i*3.
 */
int font_zh_find_utf8(const char *str) {
    if (!str) return -1;
    if (!s_zh_bound) font_zh_bind();
    if (!s_zh_bound || !s_zh_chars) return -1;
    uint8_t b0 = (uint8_t)str[0];
    uint8_t b1 = (uint8_t)str[1];
    uint8_t b2 = (uint8_t)str[2];

    for (uint32_t i = 0; i < s_zh_count; i++) {
        int pos = i * 3;
        if ((uint8_t)s_zh_chars[pos]     == b0 &&
            (uint8_t)s_zh_chars[pos + 1] == b1 &&
            (uint8_t)s_zh_chars[pos + 2] == b2) {
            return (int)i;
        }
    }
    return -1;
}

const uint8_t *font_zh_get_bitmap_by_index(int idx) {
    if (idx < 0) return NULL;
    if (!s_zh_bound) font_zh_bind();
    if (!s_zh_bound || (uint32_t)idx >= s_zh_count) return NULL;
    return s_zh_glyphs + (size_t)idx * 72;
}

/* 旧接口 - 已废弃, 仅做兼容 */
int font_zh_find_char(char c) {
    (void)c;
    return -1;
}

const uint8_t *font_zh_get_bitmap(char c) {
    (void)c;
    return NULL;
}
