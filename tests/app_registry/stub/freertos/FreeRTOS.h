#ifndef TEST_STUB_FREERTOS_H
#define TEST_STUB_FREERTOS_H
#include <stdint.h>
#include <stddef.h>

typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffu

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline int  xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return 1; }
static inline int  xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }

#endif
