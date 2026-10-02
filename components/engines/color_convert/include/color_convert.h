#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* RGB888 -> RGB565 (NES/GBC 引擎共用, 从各自 make_color.c 抽离) */
uint16_t make_color(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
