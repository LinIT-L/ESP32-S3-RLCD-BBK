#ifndef TEST_STUB_ESP_TIMER_H
#define TEST_STUB_ESP_TIMER_H
#include <stdint.h>
/* 单调微秒时钟：测试里只要求"不变负、可递增"，不要求真实时基 */
static inline int64_t esp_timer_get_time(void)
{
    static int64_t t = 1000;
    return (t += 1000);
}
#endif
