/*
 * gnuboy 内存分配覆盖: 强制把 core 内的 malloc/calloc/realloc/free
 * 指向 PSRAM (MALLOC_CAP_SPIRAM), 避免占用紧张的内部 SRAM 且支持大容量
 * GBC ROM (最大 8MB) 的 bank 缓冲.
 *
 * 通过 CMakeLists 的 -include 对所有 third_party/gnuboy 下的源文件生效;
 * 本头文件只做宏替换, 不改动 gnuboy 源码逻辑.
 */
#ifndef GBC_MEM_OVERRIDE_H
#define GBC_MEM_OVERRIDE_H

#include <stddef.h>
#include <string.h>
#include "esp_heap_caps.h"

static inline void *gbc_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline void *gbc_calloc(size_t n, size_t s)
{
    size_t total = n * s;
    void *p = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) memset(p, 0, total);
    return p;
}

static inline void *gbc_realloc(void *p, size_t n)
{
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline void gbc_free(void *p)
{
    if (p) heap_caps_free(p);
}

#define malloc gbc_malloc
#define calloc gbc_calloc
#define realloc gbc_realloc
#define free gbc_free

#endif /* GBC_MEM_OVERRIDE_H */