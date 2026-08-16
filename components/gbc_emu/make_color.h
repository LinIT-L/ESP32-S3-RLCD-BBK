#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* RGB888 -> RGB565 (小端, 与 esp-box-emu make_color 一致, 去掉 LVGL 依赖) */
uint16_t make_color(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
