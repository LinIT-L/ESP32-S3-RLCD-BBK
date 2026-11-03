/* esp_heap_caps.h — 主机测试桩：把 caps 分配映射到普通 malloc/free
 *
 * 这里故意用**真函数**而不是 static inline：要能统计 PSRAM 占用峰值
 * （固件里 MALLOC_CAP_SPIRAM 对应 8MB PSRAM，主机上就用计数器模拟）。
 * 全项目只有 tests/wiki 用这个头，改成本不影响生产代码。
 */
#ifndef STUB_ESP_HEAP_CAPS_H
#define STUB_ESP_HEAP_CAPS_H
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_SPIRAM  (1u << 10)
#define MALLOC_CAP_8BIT    (1u << 2)

void  *heap_caps_malloc(size_t n, unsigned caps);
void   heap_caps_free(void *p);
size_t heap_caps_get_largest_free_block(uint32_t caps);

/* 统计与模拟开关（定义在 esp_stub.c） */
extern size_t g_stub_psram_largest;   /* 0 以外的值 = 模拟"最大连续块"上限 */
extern size_t g_stub_psram_cur;       /* 当前占用 */
extern size_t g_stub_psram_peak;      /* 历史峰值 */
void stub_psram_reset_peak(void);

#endif
