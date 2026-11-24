/* 测试专用 esp_heap_caps.h —— 主机上直接映射到 malloc/free。 */
#pragma once
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT     (1 << 2)

static inline void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static inline void  heap_caps_free(void *p) { free(p); }
