/* host 测试用 shim: freertos/FreeRTOS.h —— 临界区退化成空操作 (测试是单线程) */
#pragma once
#include <stdint.h>

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux)      do { (void)(mux); } while (0)
#define portEXIT_CRITICAL(mux)       do { (void)(mux); } while (0)

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
