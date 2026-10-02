#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t voltage_mv;
    uint8_t percent;
} board_battery_status_t;

esp_err_t board_battery_init(void);
esp_err_t board_battery_read(board_battery_status_t *status);

/* 统一 60s 节流缓存读取 (linit 2026-09-20: 全系统单一采样节拍器).
 * 首次调用立即采样; 之后 60s 内直接返回缓存, ADC 至多 60s 打一次.
 * 状态栏/低电量告警/网页手柄/终端 battery 命令/Lua app.sys.battery()
 * 全部走本接口, 采样失败时不污染旧缓存 (返回错误, 缓存保持上次值). */
esp_err_t board_battery_read_cached(board_battery_status_t *status);

#ifdef __cplusplus
}
#endif
