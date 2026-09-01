#include "font_zh.h"
#include "font_part.h"
#include <string.h>
#include <stdbool.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "FONTZH";

/* V1.0.64: 紧凑二进制字库 (font_zh.bin / font_zh_light.bin)
 * V1.0.89+: 字库从 appdata 分区字库区 mmap 加载 (esp_partition_mmap, XIP 只读)
 * 布局: [8B magic "ZH1FNT01"][u32 count][字符表 count*3 UTF-8][字形 count*72]
 * 粗细两字重: font_zh.bin(粗) / font_zh_light.bin(细), 经 font_zh_set_style 运行时切换 */

static uint32_t s_zh_count = 0;
static const uint8_t *s_zh_chars = NULL;   /* 字符表 (count*3) */
static const uint8_t *s_zh_glyphs = NULL;  /* 字形 (count*72) */
static bool s_zh_bound = false;
static bool s_zh_light = false;            /* V1.0.89: 当前是否用细字库 */

const char *zh_chars = NULL;
const uint8_t (*zh_font_data)[72] = NULL;

/* V1.0.90: UTF-8 首字节分桶索引 — 加速 font_zh_find_utf8.
 * 字符表 b0 非单调, 构建「按 b0 分组」的索引, 查找从全表 ~6800 次降到桶内几十次.
 * 内存: order 表 count*2 (内部 RAM, ~13.6KB), 绑定字库时构建一次. */
static uint16_t *s_zh_order = NULL;
static uint16_t s_zh_b0_start[256];
static uint16_t s_zh_b0_cnt[256];   /* 用 uint16_t: b0=229 有 1490 字, uint8_t 会溢出导致桶错位 */

static void font_zh_bind(void) {
    const uint8_t *d;
    size_t len;
    if (s_zh_light) {
        d = font_part_map(FONT_SLOT_LIGHT_OFF, FONT_SLOT_ZH_LEN);
        len = FONT_SLOT_ZH_LEN;
    } else {
        d = font_part_map(FONT_SLOT_ZH_OFF, FONT_SLOT_ZH_LEN);
        len = FONT_SLOT_ZH_LEN;
    }
    if (!d || len < 12 || memcmp(d, "ZH1FNT01", 8) != 0) {
        ESP_LOGW(TAG, "zh 绑定失败: d=%p len=%u", (void *)d, (unsigned)len);
        return;
    }
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
    if (s_zh_order) { heap_caps_free(s_zh_order); s_zh_order = NULL; }  /* 重绑定失效索引 */
    ESP_LOGI(TAG, "zh 绑定成功 count=%u (light=%d)", (unsigned)n, (int)s_zh_light);
}

/* V1.0.90: 索引构建 (声明见文件顶部) */
static void zh_build_index(void) {
    if (s_zh_order) { heap_caps_free(s_zh_order); s_zh_order = NULL; }
    memset(s_zh_b0_cnt, 0, sizeof(s_zh_b0_cnt));
    for (uint32_t i = 0; i < s_zh_count; i++)
        s_zh_b0_cnt[s_zh_chars[i * 3]]++;
    uint16_t acc = 0;
    for (int b = 0; b < 256; b++) { s_zh_b0_start[b] = acc; acc += s_zh_b0_cnt[b]; }
    s_zh_order = heap_caps_malloc(s_zh_count * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_zh_order) return;
    uint16_t fill[256];
    memset(fill, 0, sizeof(fill));
    for (uint32_t i = 0; i < s_zh_count; i++) {
        uint8_t b = s_zh_chars[i * 3];
        s_zh_order[s_zh_b0_start[b] + fill[b]++] = (uint16_t)i;
    }
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
    if (!s_zh_order) zh_build_index();
    if (s_zh_order) {
        uint16_t start = s_zh_b0_start[b0];
        uint16_t cnt = s_zh_b0_cnt[b0];
        for (uint16_t k = 0; k < cnt; k++) {
            uint32_t pos = (uint32_t)s_zh_order[start + k] * 3;
            if ((uint8_t)s_zh_chars[pos]     == b0 &&
                (uint8_t)s_zh_chars[pos + 1] == b1 &&
                (uint8_t)s_zh_chars[pos + 2] == b2)
                return (int)(pos / 3);
        }
        return -1;
    }
    /* 索引不可用 (分配失败): 回退全表线性 */
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

/* V1.0.89: 切换字重 (粗/细). 将绑定标记置为无效, 下次取字重新绑定对应字库. */
void font_zh_set_style(bool light) {
    if (s_zh_light == light) return;
    s_zh_light = light;
    s_zh_bound = false;
}

bool font_zh_is_light(void) {
    return s_zh_light;
}

/* V1.0.92: 查找 ASCII 字符索引. 字库中 ASCII 以 "0x00 0x00 <ascii>" 3 字节槽存放. */
int font_zh_find_ascii(unsigned char c) {
    if (!s_zh_bound) font_zh_bind();
    if (!s_zh_bound || !s_zh_chars) return -1;
    for (uint32_t i = 0; i < s_zh_count; i++) {
        int pos = i * 3;
        if ((uint8_t)s_zh_chars[pos]     == 0x00 &&
            (uint8_t)s_zh_chars[pos + 1] == 0x00 &&
            (uint8_t)s_zh_chars[pos + 2] == c) {
            return (int)i;
        }
    }
    return -1;
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
