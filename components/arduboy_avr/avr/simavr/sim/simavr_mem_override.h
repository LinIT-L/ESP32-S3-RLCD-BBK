/*
 * simavr 内存分配覆盖: 把 simavr 核心内的 malloc/calloc/realloc/free
 * 优先指向内部 SRAM/DMA (MALLOC_CAP_INTERNAL), PSRAM 仅作回退.
 *
 * 背景: Arduboy 模拟核心 ~35-40KB (flash 32KB + data 2.5KB + avr 结构 + irq 池).
 * simavr 是纯 CPU 解释执行, 每条 AVR 指令都要随机读 avr->flash / avr->data,
 * 之前强制分到 PSRAM 时随机访问延迟比内部 SRAM 高数倍, 导致吞吐不足、
 * 游戏卡顿甚至"不动". 核心总量仅 ~40KB, 内部 RAM 空闲 (~90KB+) 放得下,
 * 故改为优先内部 DMA 内存; 内部不足时回退 PSRAM 保证可用.
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
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   /* 内部不足回退 PSRAM */
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
    void *np = heap_caps_realloc(p, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!np) np = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   /* 内部不足回退 PSRAM */
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
