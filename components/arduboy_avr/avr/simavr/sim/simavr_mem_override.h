/*
 * simavr 内存分配覆盖: 强制把 simavr 核心内的 malloc/calloc/realloc/free
 * 优先指向 PSRAM (MALLOC_CAP_SPIRAM), 避免占满紧张的内部 SRAM.
 *
 * 背景: Arduboy 模拟核心 ~35-40KB (flash 32KB + data 2.5KB + avr 结构 + irq 池)
 * 全部走内部 RAM 极易耗尽. 通过与 gbc_mem_override.h 相同的 -include 机制,
 * 把 base 的堆分配转到 PSRAM, 仅当 PSRAM 分配失败时才回退到内部 RAM.
 *
 * 通过 CMakeLists 的 -include 对组件内所有源文件生效; 本头文件只做宏替换.
 */
#ifndef SIMAVR_MEM_OVERRIDE_H
#define SIMAVR_MEM_OVERRIDE_H

#include <stddef.h>
#include <string.h>
#include "esp_heap_caps.h"

static inline void *sim_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);   /* PSRAM 失败回退内部 RAM */
    return p;
}

static inline void *sim_calloc(size_t n, size_t s)
{
    size_t total = n * s;
    void *p = sim_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static inline void *sim_realloc(void *p, size_t n)
{
    void *np = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!np) np = heap_caps_realloc(p, n, MALLOC_CAP_8BIT);   /* PSRAM 失败回退内部 RAM */
    return np;
}

static inline void sim_free(void *p)
{
    if (p) heap_caps_free(p);
}

#define malloc sim_malloc
#define calloc sim_calloc
#define realloc sim_realloc
#define free sim_free

#endif /* SIMAVR_MEM_OVERRIDE_H */
