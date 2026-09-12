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

#ifdef __cplusplus
}
#endif
