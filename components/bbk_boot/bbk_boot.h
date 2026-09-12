#ifndef BBK_BOOT_H
#define BBK_BOOT_H

#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 开机画面: 大号 "LinTOS" 文字 (FONT8X12 放大), 替代 400x300 位图, 省 flash */
void bbk_boot_draw_logo(st7305_handle_t *lcd);

#ifdef __cplusplus
}
#endif

#endif