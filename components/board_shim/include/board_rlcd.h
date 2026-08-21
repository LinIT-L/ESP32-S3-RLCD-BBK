/**
 * @file board_rlcd.h
 * @brief board_rlcd 兼容层 — 适配 gb_emu 到 st7305 驱动
 *
 * 参考项目 (esp32-s3-rlcd-gb-emulator) 的 gb_emu 组件依赖 board_rlcd
 * 组件提供 RLCD 显示接口. 本头文件提供同名接口, 内部转发到 st7305 驱动,
 * 使 gb_emu 源码无需修改即可编译.
 *
 * 使用前需调用 board_shim_set_lcd() 设置 st7305 句柄.
 */
#ifndef BOARD_RLCD_H
#define BOARD_RLCD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 兼容宏: 映射到 st7305 常量 */
#define BOARD_RLCD_WIDTH       ST7305_WIDTH
#define BOARD_RLCD_HEIGHT      ST7305_HEIGHT
#define BOARD_RLCD_COLOR_BLACK ST7305_COLOR_BLACK
#define BOARD_RLCD_COLOR_WHITE ST7305_COLOR_WHITE

/* 设置 st7305 句柄 (在 main.c 初始化时调用) */
void board_shim_set_lcd(st7305_handle_t *lcd);

/* V1.0.46+: GB/GBC 灰度模式三档 (0=纯黑白, 1=4档点聚, 2=5档点聚) */
void board_shim_set_gb_gray(int mode);

/* GB (Peanut-GB) 2x 绘制暂存: board_rlcd_draw_gb_line_2x 画到 s_fb_stage (内部 RAM).
 * 进入 GB 引擎时调用 alloc, 退出时调用 free (须等视频任务停止后). */
esp_err_t board_shim_gb_stage_alloc(void);
void      board_shim_gb_stage_free(void);

/* 返回 GB 暂存指针 (1x 模式 board_rlcd_draw_gb_line_to 用) */
uint8_t  *board_shim_gb_stage_get(void);
/* 提交 GB 暂存到 LCD FB (锁保护), 之后调 board_rlcd_flush_async() */
void      board_shim_gb_stage_commit(void);

/* === board_rlcd 兼容接口 === */
bool     board_rlcd_is_initialized(void);
esp_err_t board_rlcd_clear(uint8_t color);
esp_err_t board_rlcd_flush(void);

/* === 异步刷新 (V1.0.53): 解决 GB/GBC 音频被同步刷屏阻塞 ===
 * 模拟任务每帧画完后 board_rlcd_fb_lock/unlock 保护整帧写入,
 * 然后 board_rlcd_flush_async() 通知独立刷新任务 (core0) 拷贝快照 + SPI 发送.
 * 刷新任务自己按 ~30Hz 节流, 不再占用模拟任务的音频生产时间. */
void      board_rlcd_fb_lock(void);
void      board_rlcd_fb_unlock(void);
esp_err_t board_rlcd_flush_async(void);
void      board_rlcd_video_task_start(void);
void      board_rlcd_video_task_stop(void);
void      board_rlcd_video_task_set_interval_us(uint32_t us);

/* NES: 把模拟任务产出的灰度帧交给视频任务 (core0) 做缩放+刷屏.
 * shade 必须保持有效; 传 NULL 表示 NES 结束, 释放内部暂存. */
void      board_rlcd_set_nes_shade_source(const uint8_t *shade, int w, int h, int mode);   /* mode: 0=点对点, 1=全屏, 2=拉伸 */

/* NES: 返回视频任务持有的内部 RAM 显示缓冲 (2bit 打包, w*h/4 字节),
 * 模拟任务把打包帧拷到这里后通知刷屏. */
uint8_t  *board_rlcd_nes_disp_buffer(void);

/* NES: 模拟任务拷贝完成后调用, 原子切换到新缓冲 (避免核心帧节奏被显示锁阻塞) */
void      board_rlcd_nes_disp_publish(void);

/* GB/GBC: 把 gnuboy 当前 RGB565 双缓冲帧交给视频任务 (core0) 做灰度转换+刷屏.
 * mode: 0=1x点对点, 1=2x全屏, 2=强制拉伸全屏.
 * buf 必须保持有效直到下一帧切换 (双缓冲); 传 NULL 表示结束. */
void      board_rlcd_set_gbc_frame_source(const uint16_t *buf, int w, int h, int mode);

/* GB 模拟器: Peanut-GB 2bit 色阶 (0-3) → 1bit, 2x 缩放绘制一行
 * pixels: 160 字节, 每字节低 2 位为色阶 (0=黑,3=白)
 * width: 通常 160 */
esp_err_t board_rlcd_draw_gb_line_2x(int x, int y, const uint8_t *pixels, int width);

/* V1.0.46: GB 模拟器 1x 点对点绘制 (游戏全屏关闭时用) */
esp_err_t board_rlcd_draw_gb_line(int x, int y, const uint8_t *pixels, int width);

/* GB 1x 点对点绘制到指定 fb (Peanut-GB 用 s_fb_stage 暂存, 避免直接写 s_lcd->fb) */
esp_err_t board_rlcd_draw_gb_line_to(uint8_t *fb, int x, int y,
                                     const uint8_t *pixels, int width);

/* NES 拉伸全屏: 最近邻缩放 2bit 打包帧到整个 400x300 屏幕 (内部 RAM 快速路径) */
esp_err_t board_rlcd_draw_nes_scaled(const uint8_t *shade, int src_w, int src_h);

/* GBC (gnuboy): RGB565 BE → 4 级灰度 + 2x2 抖动, 2x 缩放绘制一行
 * pixels: 160 个 uint16_t (RGB565 big-endian), 由 gnuboy 以 GB_PIXEL_565_BE 输出 */
esp_err_t board_rlcd_draw_gbc_line_2x_rgb565_be(int x, int y, const uint16_t *pixels,
                                                int width);

/* 局部区域刷新 (当前实现为全屏刷新, 简化适配) */
esp_err_t board_rlcd_flush_area(int x1, int y1, int x2, int y2);

#ifdef __cplusplus
}
#endif

#endif
