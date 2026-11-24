/* host 测试用 shim: freertos/task.h —— 不真的起任务 */
#pragma once
#include <stddef.h>
#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline int xTaskCreate(void (*fn)(void *), const char *name,
                              uint32_t stack, void *arg, unsigned prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; if (out) *out = NULL;
    return 1;
}
