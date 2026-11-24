/* min_boot.h — 最小系统诊断入口 (只保留 LCD + 触摸) */
#ifndef MIN_BOOT_H
#define MIN_BOOT_H

#include <stdbool.h>
#include <stdint.h>
#include "app_board.h"

/* NVS 开关: lintos_diag/minboot = 1 时开机进最小系统 */
bool min_boot_requested(void);
void min_boot_set_flag(int on);

/* 最小系统主循环 (不返回)。b 为 app_board_init 已初始化的板级句柄。 */
void min_boot_run(app_board_t *b);

/* 逐项加回功能 (位见 .c 顶部注释) */
void min_boot_add(uint32_t mask);

/* 片内温度 (0.1°C); 传感器不可用时返回 -1000。正常系统 remcon `temp` 也调它。 */
int min_boot_temp10(void);

#endif
