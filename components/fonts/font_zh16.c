#include "font_zh16.h"
#include "font_part.h"
#include <string.h>
#include <stdbool.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "FONTZH16";

/* V1.0.86: 文泉驿点阵宋体 16px 紧凑二进制字库 (font_zh16.bin)
 * V1.0.89+: 字库从 appdata 分区字库区 mmap 加载 (esp_partition_mmap, XIP 只读)
 * 布局: [8B magic "ZH1FNT01"][u32 count][字符表 count*3 UTF-8][字形 count*32] */

static uint32_t s_zh16_count = 0;
static const uint8_t *s_zh16_chars = NULL;   /* 字符表 (count*3) */
static const uint8_t *s_zh16_glyphs = NULL;  /* 字形 (count*32) */
static bool s_zh16_bound = false;

const char *zh16_chars = NULL;
const uint8_t (*zh16_font_data)[32] = NULL;

/* V1.0.90: UTF-8 首字节分桶索引 (同 font_zh, 声明先于 bind 使用) */
static uint16_t *s_zh16_order = NULL;
static uint16_t s_zh16_b0_start[256];
static uint16_t s_zh16_b0_cnt[256];   /* 用 uint16_t: b0 计数可能 >255, uint8_t 会溢出 */

static void font_zh16_bind(void) {
    const uint8_t *d = font_part_map(FONT_SLOT_16_OFF, FONT_SLOT_16_LEN);
    size_t len = FONT_SLOT_16_LEN;
    if (!d || len < 12 || memcmp(d, "ZH1FNT01", 8) != 0) return;
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
    if (s_zh16_order) { heap_caps_free(s_zh16_order); s_zh16_order = NULL; }  /* 重绑定失效索引 */
}

/* V1.0.90: 索引构建 (声明见文件顶部) */
static void zh16_build_index(void) {
    if (s_zh16_order) { heap_caps_free(s_zh16_order); s_zh16_order = NULL; }
    memset(s_zh16_b0_cnt, 0, sizeof(s_zh16_b0_cnt));
    for (uint32_t i = 0; i < s_zh16_count; i++)
        s_zh16_b0_cnt[s_zh16_chars[i * 3]]++;
    uint16_t acc = 0;
    for (int b = 0; b < 256; b++) { s_zh16_b0_start[b] = acc; acc += s_zh16_b0_cnt[b]; }
    s_zh16_order = heap_caps_malloc(s_zh16_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_zh16_order) return;
    uint16_t fill[256];
    memset(fill, 0, sizeof(fill));
    for (uint32_t i = 0; i < s_zh16_count; i++) {
        uint8_t b = s_zh16_chars[i * 3];
        s_zh16_order[s_zh16_b0_start[b] + fill[b]++] = (uint16_t)i;
    }
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
    if (!s_zh16_order) zh16_build_index();
    if (s_zh16_order) {
        uint16_t start = s_zh16_b0_start[b0];
        uint16_t cnt = s_zh16_b0_cnt[b0];
        for (uint16_t k = 0; k < cnt; k++) {
            uint32_t pos = (uint32_t)s_zh16_order[start + k] * 3;
            if ((uint8_t)s_zh16_chars[pos]     == b0 &&
                (uint8_t)s_zh16_chars[pos + 1] == b1 &&
                (uint8_t)s_zh16_chars[pos + 2] == b2)
                return (int)(pos / 3);
        }
        return -1;
    }
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