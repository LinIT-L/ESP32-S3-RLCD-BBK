/* 主机测试用桩: 只满足 ui_scroll.c 的编译/链接需要, 不模拟任何硬件行为。 */
#pragma once
#include <stdint.h>

/* ui_scroll.c 只用 esp_timer_get_time() 做"触摸静默超时"兜底。 */
static inline int64_t esp_timer_get_time(void) { return 0; }
