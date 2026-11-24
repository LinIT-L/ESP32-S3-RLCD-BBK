/**
 * os_time.h — 基础时间工具 (最底层, 任何层都可依赖)
 *
 * 为什么单独成组件: 同一个 "毫秒时间戳" 曾经在 6 个 .c 里各写一份
 *   (am_asset_multi / page_calculator / r_carousel / gps_nmea / keyvault / lpk_vfs),
 * 实现都是 esp_timer_get_time()/1000 —— 完全重复。时间源若要改(换 tick 源 /
 * 加单调性补偿), 6 处各改一遍必然漏。
 *
 * 放在 libs/ 是因为它是 rank 0: drivers 层的 gps 也要用, 不能放在 ui/ 或 services/。
 */
#pragma once

#include <stdint.h>
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 开机以来的毫秒数 (32 位, 约 49.7 天回绕)。用于超时判断请用差值比较。 */
static inline uint32_t lintos_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

#ifdef __cplusplus
}
#endif
