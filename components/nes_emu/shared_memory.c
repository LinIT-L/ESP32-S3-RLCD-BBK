#include "shared_memory.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

/* 简单跟踪表: shared_mem_clear 一次性释放所有 PSRAM 块 */
#define MAX_SHARED_ALLOCATIONS 256

static void *s_blocks[MAX_SHARED_ALLOCATIONS];
static size_t s_block_sizes[MAX_SHARED_ALLOCATIONS];
static int s_block_count = 0;
static size_t s_total_allocated = 0;

static void *shared_malloc_impl(size_t size, int internal)
{
    if (size == 0) return NULL;
    if (s_block_count >= MAX_SHARED_ALLOCATIONS) return NULL;
    void *p = heap_caps_malloc(size, (internal ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    memset(p, 0, size);
    s_blocks[s_block_count] = p;
    s_block_sizes[s_block_count] = size;
    s_block_count++;
    s_total_allocated += size;
    return p;
}

void *shared_mem_get_instance(void)
{
    return s_block_count ? s_blocks[0] : NULL;
}

void *shared_mem_allocate(const shared_mem_request_t *request)
{
    if (!request) return NULL;
    return shared_malloc_impl(request->size, request->storage == SHARED_MEM_INTERNAL);
}

void *shared_malloc(size_t size)
{
    return shared_malloc_impl(size, 0);
}

size_t shared_num_bytes_allocated(void)
{
    return s_total_allocated;
}

shared_mem_stats_t shared_mem_get_stats(void)
{
    shared_mem_stats_t st = { .total_allocated = s_total_allocated, .total_free = 0 };
    return st;
}

void shared_mem_clear(void)
{
    for (int i = 0; i < s_block_count; i++) {
        if (s_blocks[i]) heap_caps_free(s_blocks[i]);
        s_blocks[i] = NULL;
    }
    s_block_count = 0;
    s_total_allocated = 0;
}
