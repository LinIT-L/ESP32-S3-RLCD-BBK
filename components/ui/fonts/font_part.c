/**
 * font_part.c — 字库分区加载实现 (esp_partition_mmap).
 */
#include "font_part.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>

#define TAG "FONTP"

#define FONT_MAP_MAX 12   /* zh/light/16 + book16/24/24hz/32 + lav (+余量) */

static const esp_partition_t *s_part;
static esp_partition_mmap_handle_t s_handles[FONT_MAP_MAX];
static const uint8_t *s_addrs[FONT_MAP_MAX];
static size_t s_offs[FONT_MAP_MAX];
static size_t s_lens[FONT_MAP_MAX];
static int s_n;

const uint8_t *font_part_map(size_t off, size_t len)
{
    if (len == 0) return NULL;
    /* 已映射过同一区域: 直接返回 (字体粗细切换会重复 bind) */
    for (int i = 0; i < s_n; i++) {
        if (s_offs[i] == off && s_lens[i] == len) return s_addrs[i];
    }
    if (s_n >= FONT_MAP_MAX) {
        ESP_LOGE(TAG, "mmap 槽位已满 (%d), off=0x%x", FONT_MAP_MAX, (unsigned)off);
        return NULL;
    }
    if (!s_part) {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "appdata");
        if (!s_part) {
            ESP_LOGE(TAG, "找不到 appdata 分区 (off=0x%x len=0x%x)", (unsigned)off, (unsigned)len);
            return NULL;
        }
    }
    const void *p = NULL;
    esp_err_t e = esp_partition_mmap(s_part, off, len, ESP_PARTITION_MMAP_DATA,
                                     &p, &s_handles[s_n]);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap 失败: %s (0x%x), off=0x%x len=0x%x",
                 esp_err_to_name(e), e, (unsigned)off, (unsigned)len);
        return NULL;
    }
    s_addrs[s_n] = (const uint8_t *)p;
    s_offs[s_n]  = off;
    s_lens[s_n]  = len;
    ESP_LOGI(TAG, "mmap 成功: off=0x%x len=0x%x addr=%p", (unsigned)off, (unsigned)len, p);
    return s_addrs[s_n++];
}
