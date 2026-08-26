#include "font_zh16.h"
#include <string.h>
#include <stdbool.h>

/* V1.0.86: 文泉驿点阵宋体 16px 紧凑二进制字库 (components/menu/font_zh16.bin,
 *         由 CMake target_add_binary_data 嵌入, GB2312 全量 6813 字)
 * 布局: [8B magic "ZH1FNT01"][u32 count][字符表 count*3 UTF-8][字形 count*32] */
extern const uint8_t font_zh16_bin_start[] asm("_binary_font_zh16_bin_start");
extern const uint8_t font_zh16_bin_end[]   asm("_binary_font_zh16_bin_end");

static uint32_t s_zh16_count = 0;
static const uint8_t *s_zh16_chars = NULL;   /* 字符表 (count*3) */
static const uint8_t *s_zh16_glyphs = NULL;  /* 字形 (count*32) */
static bool s_zh16_bound = false;

const char *zh16_chars = NULL;
const uint8_t (*zh16_font_data)[32] = NULL;

static void font_zh16_bind(void) {
    const uint8_t *d = font_zh16_bin_start;
    size_t len = (size_t)(font_zh16_bin_end - font_zh16_bin_start);
    if (len < 12 || memcmp(d, "ZH1FNT01", 8) != 0) return;
    uint32_t n = (uint32_t)d[8] | ((uint32_t)d[9] << 8) |
                 ((uint32_t)d[10] << 16) | ((uint32_t)d[11] << 24);
    if (n == 0 || n > 20000) return;
    if (len < 12 + (size_t)n * 3 + (size_t)n * 32) return;
    s_zh16_count = n;
    s_zh16_chars = d + 12;
    s_zh16_glyphs = d + 12 + (size_t)n * 3;
    zh16_chars = (const char *)s_zh16_chars;
    zh16_font_data = (const uint8_t (*)[32])s_zh16_glyphs;
    s_zh16_bound = true;
}

int font_zh16_count(void) {
    if (!s_zh16_bound) font_zh16_bind();
    return (int)s_zh16_count;
}

int font_zh16_find_utf8(const char *str) {
    if (!str) return -1;
    if (!s_zh16_bound) font_zh16_bind();
    if (!s_zh16_bound || !s_zh16_chars) return -1;
    uint8_t b0 = (uint8_t)str[0];
    uint8_t b1 = (uint8_t)str[1];
    uint8_t b2 = (uint8_t)str[2];
    for (uint32_t i = 0; i < s_zh16_count; i++) {
        int pos = i * 3;
        if ((uint8_t)s_zh16_chars[pos]     == b0 &&
            (uint8_t)s_zh16_chars[pos + 1] == b1 &&
            (uint8_t)s_zh16_chars[pos + 2] == b2) {
            return (int)i;
        }
    }
    return -1;
}

const uint8_t *font_zh16_get_bitmap_by_index(int idx) {
    if (idx < 0) return NULL;
    if (!s_zh16_bound) font_zh16_bind();
    if (!s_zh16_bound || (uint32_t)idx >= s_zh16_count) return NULL;
    return s_zh16_glyphs + (size_t)idx * 32;
}