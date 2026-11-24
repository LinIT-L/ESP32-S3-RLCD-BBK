/* 测试专用 esp_heap_caps.h —— 堆数字由测试设定, 用于覆盖基线/泄漏复核分支。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_INTERNAL  (1 << 11)
#define MALLOC_CAP_SPIRAM    (1 << 10)
#define MALLOC_CAP_8BIT      (1 << 2)

size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);

/* 测试控制: 设定下一步两个池子的"空闲量", 以及是否真的释放内存 */
void t_heap_set(size_t internal_free, size_t psram_free);
