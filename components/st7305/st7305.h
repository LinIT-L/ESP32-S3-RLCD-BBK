#ifndef ST7305_H
#define ST7305_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7305_WIDTH          400
#define ST7305_HEIGHT         300

#define ST7305_COLOR_BLACK    0
#define ST7305_COLOR_WHITE    1

typedef uint8_t st7305_color_t;

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t          dc_gpio;
    gpio_num_t          rst_gpio;
    spi_host_device_t   spi_host;
    int                 spi_mosi;
    int                 spi_sclk;
    int                 spi_cs;
    int                 spi_freq;
    uint8_t            *fb;
} st7305_handle_t;

typedef struct {
    gpio_num_t        dc_gpio;
    gpio_num_t        rst_gpio;
    spi_host_device_t spi_host;
    int               spi_mosi;
    int               spi_sclk;
    int               spi_cs;
    int               spi_freq;
} st7305_config_t;

st7305_config_t st7305_default_config(void);

esp_err_t st7305_init(st7305_handle_t *out, const st7305_config_t *config);

void st7305_deinit(st7305_handle_t *dev);

void st7305_clear(st7305_handle_t *dev, st7305_color_t color);

void st7305_draw_pixel(st7305_handle_t *dev, int x, int y, st7305_color_t color);

void st7305_draw_bitmap_1bit(st7305_handle_t *dev,
                              int x, int y, int w, int h,
                              const uint8_t *bitmap);

void st7305_draw_text(st7305_handle_t *dev, int x, int y, const char *text);

esp_err_t st7305_flush(st7305_handle_t *dev);

/* 从指定缓冲发送整帧 (与 st7305_flush 相同, 但数据源可独立于 dev->fb).
 * 用于双缓冲/快照刷新: 模拟任务持续写 fb, 刷新任务把快照拷出后异步送 SPI. */
esp_err_t st7305_flush_from(st7305_handle_t *dev, const uint8_t *fb);

/* 软件旋转整帧输出 (电子书阅读器用, 主菜单始终保持横屏)
 * rot: 0=上(横屏原样), 1=下(180°), 2=左(90°逆时针, 竖屏), 3=右(90°顺时针, 竖屏)
 * work_fb: 调用方提供的 15KB 备用缓冲 (PSRAM), 旋转时用于存放中间帧
 * 注意: 每次 flush 都会把面板扫描方向设回对应方向, 菜单的普通 flush 也会恢复横屏 */
esp_err_t st7305_flush_rotated(st7305_handle_t *dev, uint8_t rot, uint8_t *work_fb);

/* === 高性能 API === */

/* 1bpp bitmap 直接批量写入 ST7305 fb, 跳过 draw_pixel 调用.
 * bitmap 每字节 8 个水平像素 (MSB 在左), 内存中按行存储 (bytes_per_row = (w+7)/8).
 * 坐标 (x,y) 和尺寸 (w,h) 都是 1bpp bitmap 自身的; 整张图被绘制到 ST7305 屏幕.
 * ⚠️ 不做边界检查, 调用方需保证 x+w <= 400 且 y+h <= 300.
 * 比 st7305_draw_bitmap_1bit 快 5-10x (批量 PSRAM 写, 无函数调用开销). */
void st7305_blit_1bit(st7305_handle_t *dev,
                       int x, int y, int w, int h,
                       const uint8_t *bitmap);

/* 与 st7305_blit_1bit 相同, 但 bitmap 位=1 的像素写入"白" (置位 fb),
 * 位=0 保持原样. 用于白字黑底场景 (如 AVR 模拟器游戏画面). */
void st7305_blit_1bit_white(st7305_handle_t *dev,
                            int x, int y, int w, int h,
                            const uint8_t *bitmap);

/* 设置 ST7305 局部窗口 (CASET + RASET 一次), 之后 flush() 只刷这个矩形.
 * 用于只更新变化区域, 大幅减少 SPI 数据量.
 * ⚠️ 必须在 flush() 之前调用, 之后窗口仍保留 (不会自动恢复全屏). */
void st7305_set_window(st7305_handle_t *dev, int x, int y, int w, int h);

/* 恢复全屏窗口 */
void st7305_set_full_window(st7305_handle_t *dev);

/* 硬件对比度调整 (ST7305 0xC1 寄存器 VOP)
 * contrast: 0-63, 默认 0x41(65) 但 6-bit 范围内有效, 值越大像素越深
 * 实际范围限制在 0x20-0x3F 防止过深/过浅
 */
esp_err_t st7305_set_contrast(st7305_handle_t *dev, uint8_t contrast);

/* 反色显示开关 (0x21 = 反色, 0x20 = 正常) */
esp_err_t st7305_set_inversion(st7305_handle_t *dev, bool on);

/* 进入/退出低功耗模式 (0xD1 0x01 = 低功耗, 0xD1 0x00 = 高功耗) */
esp_err_t st7305_set_low_power(st7305_handle_t *dev, bool on);

#ifdef __cplusplus
}
#endif

#endif
