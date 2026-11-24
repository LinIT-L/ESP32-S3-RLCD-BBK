/* esp_timer.h 桩 —— 时间在测试里可控, 用来驱动超时逻辑。 */
#ifndef STUB_ESP_TIMER_H
#define STUB_ESP_TIMER_H

#include <stdint.h>

int64_t esp_timer_get_time(void);       /* 由 sandbox_test.c 实现, 返回可设置的假时间 */
void    stub_timer_set(int64_t us);
void    stub_timer_advance(int64_t us);

#endif
