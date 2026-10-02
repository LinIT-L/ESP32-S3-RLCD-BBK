#ifndef DISPLAY_TEST_H
#define DISPLAY_TEST_H

#include "esp_err.h"
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 显示刷新自检: 固定图案 × 不同刷新率, 每档 2 秒, 串口打印档位.
 * 用于排查"整屏刷新率闪烁"问题 (观察哪一档/哪些图案在闪). */
void display_test_run(st7305_handle_t *lcd);

/* 快速诊断: 密集 2x2 灰度静止/交替 @30/60Hz */
void display_test_quick(st7305_handle_t *lcd);

/* 只跑动态画面测试 (滚动条/灰度交替 @30/60Hz) */
void display_test_motion_only(st7305_handle_t *lcd);

#ifdef __cplusplus
}
#endif

#endif
