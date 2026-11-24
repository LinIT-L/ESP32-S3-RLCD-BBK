/* host 测试用 shim: esp_timer.h —— 用单调时钟顶替, 单位仍是微秒 */
#pragma once
#include <stdint.h>
#include <time.h>
static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
