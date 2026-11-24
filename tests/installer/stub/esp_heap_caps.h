/* 宿主机测试桩：把 heap_caps_* 映射到普通 malloc/free。
 * 生产代码（am_lpkg.c）只用到 malloc/free 这两个，语义足够。 */
#pragma once
#include <stdlib.h>
#include <stddef.h>

#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT     0

static inline void *heap_caps_malloc(size_t sz, int caps) { (void)caps; return malloc(sz); }
static inline void  heap_caps_free(void *p) { free(p); }
