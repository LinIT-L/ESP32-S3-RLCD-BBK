#include "font_switch.h"

#include <string.h>

#include "esp_log.h"
#include "font_part.h"
#include "font_zh.h"
#include "font_zh16.h"

static const char *TAG = "UIFONT";

/* 当前生效档位：0 = 默认槽，1 = 候选槽。初值即默认槽。 */
static int s_cur16 = F16_DEFAULT;
static int s_cur24 = F24_DEFAULT;

/* 两档的槽位表，下标与枚举一致 */
static const uint32_t k16_off[F16_COUNT] = {
    FONT_SLOT_16_OFF,   FONT_SLOT_ALT16_OFF,
};
static const uint32_t k16_len[F16_COUNT] = {
    FONT_SLOT_16_LEN,   FONT_SLOT_ALT16_LEN,
};
static const uint32_t k24_off[F24_COUNT] = {
    FONT_SLOT_ZH_OFF,   FONT_SLOT_ALT24_OFF,
};
static const uint32_t k24_len[F24_COUNT] = {
    FONT_SLOT_ZH_LEN,   FONT_SLOT_ALT24_LEN,
};

const char *font16_name(int i) {
    switch (i) {
    case F16_ALT: return "\xe9\x95\xbf\xe5\x9d\x82\xe7\x82\xb9\xe5\xae\x8b" "16";        /* 长坂点宋16 */
    default:      return "\xe6\x96\x87\xe6\xb3\x89\xe9\xa9\xbf\xe7\x82\xb9\xe9\x98\xb5\xe5\xae\x8b" "16"; /* 文泉驿点阵宋16 */
    }
}

const char *font24_name(int i) {
    switch (i) {
    case F24_ALT: return "\xe6\x96\xb9\xe6\xad\xa3\xe5\x83\x8f\xe7\xb4\xa0" "24(\xe6\xb5\x8b\xe8\xaf\x95)"; /* 方正像素24(测试) */
    default:      return "HZK24S\xe5\xae\x8b\xe4\xbd\x93";                                                   /* HZK24S宋体 */
    }
}

bool font16_available(int i) {
    if (i <= F16_DEFAULT) return true;
    if (i >= F16_COUNT) return false;
    int prev = s_cur16;
    bool ok = font_zh16_rebind(k16_off[i], k16_len[i]);
    font_zh16_rebind(k16_off[prev], k16_len[prev]);   /* 立刻绑回 */
    return ok;
}

bool font24_available(int i) {
    if (i <= F24_DEFAULT) return true;
    if (i >= F24_COUNT) return false;
    int prev = s_cur24;
    bool ok = font_zh_rebind(k24_off[i], k24_len[i]);
    font_zh_rebind(k24_off[prev], k24_len[prev]);
    return ok;
}

bool font16_apply(int i) {
    if (i < 0 || i >= F16_COUNT) i = F16_DEFAULT;
    if (i == s_cur16 && i == F16_DEFAULT) return true;

    if (font_zh16_rebind(k16_off[i], k16_len[i])) {
        s_cur16 = i;
        ESP_LOGI(TAG, "16px 字体 -> %s (槽 +0x%X)", font16_name(i), (unsigned)k16_off[i]);
        return true;
    }

    ESP_LOGW(TAG, "16px 槽 +0x%X 不可用, 回退默认", (unsigned)k16_off[i]);
    if (i != F16_DEFAULT) {
        font_zh16_rebind(k16_off[F16_DEFAULT], k16_len[F16_DEFAULT]);
        s_cur16 = F16_DEFAULT;
    }
    return false;
}

bool font24_apply(int i) {
    if (i < 0 || i >= F24_COUNT) i = F24_DEFAULT;
    if (i == s_cur24 && i == F24_DEFAULT) return true;

    if (font_zh_rebind(k24_off[i], k24_len[i])) {
        s_cur24 = i;
        ESP_LOGI(TAG, "24px 字体 -> %s (槽 +0x%X)", font24_name(i), (unsigned)k24_off[i]);
        return true;
    }

    ESP_LOGW(TAG, "24px 槽 +0x%X 不可用, 回退默认", (unsigned)k24_off[i]);
    if (i != F24_DEFAULT) {
        font_zh_rebind(k24_off[F24_DEFAULT], k24_len[F24_DEFAULT]);
        s_cur24 = F24_DEFAULT;
    }
    return false;
}

int font16_get(void) { return s_cur16; }
int font24_get(void) { return s_cur24; }
