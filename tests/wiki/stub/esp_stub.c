/* esp_stub.c — 主机测试桩的真符号实现
 *
 * 两个作用：
 *   1) heap_caps_* 映射到 malloc/free，并统计 PSRAM 占用峰值
 *      （wiki_load 的 clen+ulen 峰值到底多少，靠它给实测数字）
 *   2) g_stub_psram_largest 可改写，用来模拟"PSRAM 不足"路径
 */
#include "esp_heap_caps.h"
#include <string.h>

/* 默认 8MB：ESP32-S3 带 8MB PSRAM 的实机量级 */
size_t g_stub_psram_largest = 8u * 1024 * 1024;
size_t g_stub_psram_cur = 0;
size_t g_stub_psram_peak = 0;

#define HDR 16   /* 前置头部存原始长度；16 字节保持对齐 */

void *heap_caps_malloc(size_t n, unsigned caps) {
    (void)caps;
    unsigned char *p = (unsigned char *)malloc(n + HDR);
    if (!p) return NULL;
    *(size_t *)p = n;
    g_stub_psram_cur += n;
    if (g_stub_psram_cur > g_stub_psram_peak) g_stub_psram_peak = g_stub_psram_cur;
    return p + HDR;
}

void heap_caps_free(void *q) {
    if (!q) return;
    unsigned char *p = (unsigned char *)q - HDR;
    size_t n = *(size_t *)p;
    if (g_stub_psram_cur >= n) g_stub_psram_cur -= n;
    else g_stub_psram_cur = 0;
    free(p);
}

size_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return g_stub_psram_largest;
}

void stub_psram_reset_peak(void) {
    g_stub_psram_peak = g_stub_psram_cur;
}
