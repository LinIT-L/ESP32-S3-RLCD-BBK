#ifndef BBK_BOOT_H
#define BBK_BOOT_H

#include <stdint.h>

/* 步步高开机界面 (1-bit 位图, 400x300, 行优先, MSB=左) */
extern const uint8_t *bbk_boot_logo;
extern const int bbk_boot_logo_w;
extern const int bbk_boot_logo_h;

#endif
