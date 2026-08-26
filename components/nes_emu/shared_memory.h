#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* esp-box-emu shared_memory 组件的极简垫片: 全部走 PSRAM 堆分配,
 * 由 shared_mem_clear() 统一释放 (nes_free_shared_memory 使用). */

typedef enum {
    SHARED_MEM_DEFAULT = 0,
    SHARED_MEM_VECTOR,
    SHARED_MEM_CACHE_LINE
} shared_mem_region_t;

typedef enum {
    SHARED_MEM_PSRAM = 0,
    SHARED_MEM_INTERNAL = 1,
} shared_mem_storage_t;

typedef struct {
    size_t size;
    shared_mem_region_t region;
    shared_mem_storage_t storage;
} shared_mem_request_t;

typedef struct {
    size_t total_allocated;
    size_t total_free;
} shared_mem_stats_t;

void *shared_mem_get_instance(void);
void *shared_mem_allocate(const shared_mem_request_t *request);
void *shared_malloc(size_t size);
size_t shared_num_bytes_allocated(void);
void shared_mem_clear(void);
shared_mem_stats_t shared_mem_get_stats(void);

#ifdef __cplusplus
}
#endif
