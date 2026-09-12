#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "st7305.h"

#define TAG "ST7305"

#define FB_BYTES  ((ST7305_WIDTH / 4) * (ST7305_HEIGHT / 2))  /* 15000 */
#define FB_STRIDE (ST7305_HEIGHT >> 2)                     /* 75 */

/* LCD SPI 互斥: 模拟器任务与 OS 主循环可能并发刷屏, 防止
 * "spi_device_polling_transmit: previous polling transaction not terminated"
 * 导致画面撕裂("出来一部分")/卡顿. */
static SemaphoreHandle_t s_lcd_lock = NULL;
static void lcd_lock_ensure(void) {
    if (!s_lcd_lock) s_lcd_lock = xSemaphoreCreateMutex();
}
/* 屏保统一刷屏门禁: 屏保激活时 true → 所有 st7305_flush* 变成 no-op,
 * 任何程序/引擎都无法往屏上写(视觉上全部暂停), 壁纸用 override 独占刷屏. */
static volatile bool s_flush_blocked = false;
void st7305_set_flush_blocked(bool block) { s_flush_blocked = block; }

#define CASET_START  0x12
#define CASET_END    0x2A
#define RASET_START  0x00
#define RASET_END    0xC7

/* MADCTL (0x36): 横屏默认 0x48 = MX|BGR; 竖屏行/列交换 + BGR */
#define MADCTL_LANDSCAPE  0x48
#define MADCTL_PORTRAIT   0x28   /* MV|BGR (若方向/镜像不对, 调 MY/MX 位) */
#define MADCTL_180        0xC8   /* 横屏翻转 XY 两轴 (0x48|x0x80): 硬件 180°, 非软件拷贝 */

static inline void dc_set(st7305_handle_t *dev, int level) {
    gpio_set_level(dev->dc_gpio, level);
}

static inline void rst_set(st7305_handle_t *dev, int level) {
    gpio_set_level(dev->rst_gpio, level);
}

static void spi_write_byte(st7305_handle_t *dev, uint8_t byte) {
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &byte,
    };
    spi_device_polling_transmit(dev->spi, &t);
}

static void send_cmd(st7305_handle_t *dev, uint8_t cmd) {
    dc_set(dev, 0);
    spi_write_byte(dev, cmd);
    dc_set(dev, 1);
}

static void send_data(st7305_handle_t *dev, const uint8_t *data, size_t len) {
    if (!len) return;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(dev->spi, &t);
}

static void send_data_byte(st7305_handle_t *dev, uint8_t d) {
    send_data(dev, &d, 1);
}

static void st7305_hw_reset(st7305_handle_t *dev) {
    rst_set(dev, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    rst_set(dev, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    rst_set(dev, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    dc_set(dev, 1);
}

static esp_err_t st7305_init_sequence(st7305_handle_t *dev) {
    send_cmd(dev, 0xD6); { uint8_t d[] = {0x17, 0x02}; send_data(dev, d, 2); }
    send_cmd(dev, 0xD1);   send_data_byte(dev, 0x01);

    send_cmd(dev, 0xC0); { uint8_t d[] = {0x11, 0x04}; send_data(dev, d, 2); }   /* 本工程原始: VGH/VGL */
    send_cmd(dev, 0xC1); { uint8_t d[] = {0x69, 0x69, 0x69, 0x69}; send_data(dev, d, 4); }  /* VSHP (原始) */
    send_cmd(dev, 0xC2); { uint8_t d[] = {0x19, 0x19, 0x19, 0x19}; send_data(dev, d, 4); }  /* VSLP (原始) */
    send_cmd(dev, 0xC4); { uint8_t d[] = {0x4B, 0x4B, 0x4B, 0x4B}; send_data(dev, d, 4); }  /* VSHN (原始) */
    send_cmd(dev, 0xC5); { uint8_t d[] = {0x19, 0x19, 0x19, 0x19}; send_data(dev, d, 4); }  /* VSLN (原始) */

    send_cmd(dev, 0xD8); { uint8_t d[] = {0x80, 0xE9}; send_data(dev, d, 2); }    /* 原始 */

    send_cmd(dev, 0xB2);   send_data_byte(dev, 0x02);                            /* 帧率 (原始, 勿改) */
    send_cmd(dev, 0xB3); { uint8_t d[] = {0xE5,0xF6,0x05,0x46,0x77,0x77,0x77,0x77,0x76,0x45};
                           send_data(dev, d, 10); }                               /* 原始 HPM EQ */
    send_cmd(dev, 0xB4); { uint8_t d[] = {0x05,0x46,0x77,0x77,0x77,0x77,0x76,0x45};
                           send_data(dev, d, 8); }                               /* 原始 LPM EQ */
    send_cmd(dev, 0x62); { uint8_t d[] = {0x32, 0x03, 0x1F}; send_data(dev, d, 3); }

    send_cmd(dev, 0xB7);   send_data_byte(dev, 0x13);
    send_cmd(dev, 0xB0);   send_data_byte(dev, 0x64);                            /* 原始 */

    send_cmd(dev, 0x11);
    vTaskDelay(pdMS_TO_TICKS(200));

    send_cmd(dev, 0xC9);   send_data_byte(dev, 0x00);
    send_cmd(dev, 0x36);   send_data_byte(dev, 0x48);
    send_cmd(dev, 0x3A);   send_data_byte(dev, 0x11);
    send_cmd(dev, 0xB9);   send_data_byte(dev, 0x20);
    send_cmd(dev, 0xB8);   send_data_byte(dev, 0x29);
    send_cmd(dev, 0x21);

    send_cmd(dev, 0x2A); { uint8_t d[] = {CASET_START, CASET_END}; send_data(dev, d, 2); }
    send_cmd(dev, 0x2B); { uint8_t d[] = {RASET_START, RASET_END}; send_data(dev, d, 2); }

    send_cmd(dev, 0x35);   send_data_byte(dev, 0x00);
    send_cmd(dev, 0xD0);   send_data_byte(dev, 0xFF);
    send_cmd(dev, 0x38);
    send_cmd(dev, 0x29);

    return ESP_OK;
}

esp_err_t st7305_init(st7305_handle_t *out, const st7305_config_t *config) {
    if (!out || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = (st7305_handle_t){
        .dc_gpio = config->dc_gpio,
        .rst_gpio = config->rst_gpio,
        .spi_host = config->spi_host,
        .spi_mosi = config->spi_mosi,
        .spi_sclk = config->spi_sclk,
        .spi_cs = config->spi_cs,
        .spi_freq = config->spi_freq,
    };

    /* 帧缓冲: PSRAM 优先 (省内部 SRAM; 绘制逐像素写入 PSRAM 虽稍慢, 主菜单卡顿根因是
     * 渲染时每帧反复读 NVS, 与 fb 位置无关 — 已在 page_main 用可见应用缓存修复). */
    out->fb = (uint8_t *)heap_caps_calloc(1, FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out->fb) {
        ESP_LOGW(TAG, "PSRAM full, fallback to SRAM");
        out->fb = (uint8_t *)heap_caps_calloc(1, FB_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!out->fb) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "fb=%p size=%d [%s]",
             (void*)out->fb, FB_BYTES,
             esp_ptr_internal(out->fb) ? "SRAM" : "PSRAM");

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << out->dc_gpio) | (1ULL << out->rst_gpio),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    st7305_hw_reset(out);

    spi_bus_config_t bus = {
        .mosi_io_num = out->spi_mosi,
        .miso_io_num = -1,
        .sclk_io_num = out->spi_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FB_BYTES + 4,
    };
    /* SD 卡可能先于 LCD 初始化, SPI2 已被 SD 占用, 这里允许 ESP_ERR_INVALID_STATE */
    esp_err_t init_ret = spi_bus_initialize(out->spi_host, &bus, SPI_DMA_CH_AUTO);
    if (init_ret != ESP_OK && init_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize 失败: %s (0x%x)", esp_err_to_name(init_ret), init_ret);
        return init_ret;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = out->spi_freq * 1000 * 1000,
        .mode = 0,
        .spics_io_num = out->spi_cs,
        .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(out->spi_host, &dev, &out->spi));

    esp_err_t ret = st7305_init_sequence(out);
    if (ret != ESP_OK) {
        return ret;
    }

    st7305_clear(out, ST7305_COLOR_WHITE);
    st7305_flush(out);

    ESP_LOGI(TAG, "ST7305 initialized (%dx%d, fb=%zu bytes)",
             ST7305_WIDTH, ST7305_HEIGHT, (size_t)FB_BYTES);
    return ESP_OK;
}

void st7305_deinit(st7305_handle_t *dev) {
    if (!dev) return;
    if (dev->fb) free(dev->fb);
    if (dev->spi) spi_bus_remove_device(dev->spi);
}

/* ---- 竖屏绘制目标 (书库旋转左/右) ----
 * pfb: 300 宽 x 400 高 逻辑缓冲, 每字节 2 列 x 4 行 (与阅读器竖屏缓冲同布局, 位=1 黑). */
#define PT_W 300
#define PT_H 400
#define PT_ROW_BYTES 40
static void pt_px(uint8_t *fb, int x, int y, int black) {
    if (x < 0 || x >= PT_W || y < 0 || y >= PT_H) return;
    uint8_t *p = &fb[(size_t)y * PT_ROW_BYTES + (size_t)(x >> 3)];
    uint8_t m = (uint8_t)(0x80u >> (x & 7));
    if (black) *p |= m; else *p &= (uint8_t)~m;
}
static int pt_get(const uint8_t *fb, int x, int y) {
    if (x < 0 || x >= PT_W || y < 0 || y >= PT_H) return 0;
    return (int)((fb[(size_t)y * PT_ROW_BYTES + (size_t)(x >> 3)] >> (7 - (x & 7))) & 1u);
}

void st7305_set_portrait(st7305_handle_t *dev, uint8_t *pfb, int rot) {
    if (!dev) return;
    dev->pt_pfb = pfb;
    dev->pt_rot = (rot == 3) ? 3 : 2;
}

/* 统一旋转: 书架与阅读器共用. rot0/原样 rot1/横屏180 flip 缓冲 rot2,3/竖屏 pfb */
void st7305_set_rotation(st7305_handle_t *dev, int rot, uint8_t *buf) {
    if (!dev) return;
    dev->pt_pfb = NULL;
    dev->r_flip1 = NULL;
    if (rot == 1) {
        dev->r_flip1 = buf;
    } else if (rot == 2 || rot == 3) {
        dev->pt_pfb = buf;
        dev->pt_rot = rot;
    }
}

void st7305_clear(st7305_handle_t *dev, st7305_color_t color) {
    if (!dev || !dev->fb) return;
    if (dev->pt_pfb) {
        memset(dev->pt_pfb, (color == ST7305_COLOR_WHITE) ? 0x00 : 0xFF, PT_ROW_BYTES * PT_H);
        return;
    }
    memset(dev->fb, (color == ST7305_COLOR_WHITE) ? 0xFF : 0x00, FB_BYTES);
}

void st7305_draw_pixel(st7305_handle_t *dev, int x, int y, st7305_color_t color) {
    if (!dev || !dev->fb) return;
    if (dev->pt_pfb) {
        pt_px(dev->pt_pfb, x, y, (color == ST7305_COLOR_BLACK) ? 1 : 0);
        return;
    }
    if ((uint32_t)x >= (uint32_t)ST7305_WIDTH || (uint32_t)y >= (uint32_t)ST7305_HEIGHT) return;

    int inv_y = ST7305_HEIGHT - 1 - y;
    uint32_t idx = (uint32_t)(x >> 1) * FB_STRIDE + (uint32_t)(inv_y >> 2);
    uint8_t bit = 7u - (uint8_t)(((inv_y & 3) << 1) | (x & 1));

    if (color == ST7305_COLOR_BLACK) {
        dev->fb[idx] &= ~(uint8_t)(1u << bit);
    } else {
        dev->fb[idx] |= (uint8_t)(1u << bit);
    }
}

static void draw_char_scaled(st7305_handle_t *dev, int px, int py, char c, int scale) {
    if (!dev || !dev->fb) return;

    static const uint8_t FONT5X7[][5] = {
        {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
        {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
        {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
        {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
        {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
        {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
        {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
        {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
        {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
        {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
        {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x01,0x01},
        {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
        {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
        {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
        {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
        {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
        {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
        {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
        {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
        {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
        {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
        {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x7C},
        {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
        {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
        {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x40,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
        {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
        {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
        {0x00,0x41,0x36,0x08,0x00}, {0x10,0x08,0x08,0x10,0x08},
    };

    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) c = '?';
    const uint8_t *g = FONT5X7[(uint8_t)c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        st7305_draw_pixel(dev, px + col * scale + dx, py + row * scale + dy, ST7305_COLOR_BLACK);
            }
        }
    }
}

void st7305_draw_text(st7305_handle_t *dev, int x, int y, const char *text) {
    if (!dev || !dev->fb || !text) return;

    int scale = 2;
    int stride = 6 * scale;
    for (; *text; text++, x += stride) {
        if (x + 5 * scale > ST7305_WIDTH) break;
        draw_char_scaled(dev, x, y, *text, scale);
    }
}

/* 字形 1px 二值膨胀 (白色描边): 对每个笔画像素, 将其 3x3 邻域全部画白.
 * 之后再画黑色实体, 即在黑色笔画外形成 1px 白边. */
static void draw_char_outline(st7305_handle_t *dev, int px, int py, char c, int scale) {
    if (!dev || !dev->fb) return;
    static const uint8_t FONT5X7[][5] = {
        {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
        {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
        {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
        {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
        {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
        {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
        {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
        {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
        {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
        {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
        {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x01,0x01},
        {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
        {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
        {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
        {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
        {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
        {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
        {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
        {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
        {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
        {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
        {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x7C},
        {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
        {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
        {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x40,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
        {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
        {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
        {0x00,0x41,0x36,0x08,0x00}, {0x10,0x08,0x08,0x10,0x08},
    };

    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) c = '?';
    const uint8_t *g = FONT5X7[(uint8_t)c - 0x20];
    /* 先画膨胀的白边 */
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        for (int sy = 0; sy < scale; sy++)
                            for (int sx = 0; sx < scale; sx++)
                                st7305_draw_pixel(dev, px + col * scale + dx + sx,
                                                  py + row * scale + dy + sy, ST7305_COLOR_WHITE);
            }
        }
    }
    /* 再画黑色实体 */
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        st7305_draw_pixel(dev, px + col * scale + dx, py + row * scale + dy, ST7305_COLOR_BLACK);
            }
        }
    }
}

void st7305_draw_text_outlined(st7305_handle_t *dev, int x, int y, const char *text) {
    if (!dev || !dev->fb || !text) return;
    int scale = 2;
    int stride = 6 * scale;
    for (; *text; text++, x += stride) {
        if (x + 5 * scale > ST7305_WIDTH) break;
        draw_char_outline(dev, x, y, *text, scale);
    }
}

void st7305_draw_bitmap_1bit(st7305_handle_t *dev, int x, int y, int w, int h, const uint8_t *bitmap) {
    if (!dev || !dev->fb || !bitmap) return;

    int bytes_per_row = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint8_t byte = bitmap[row * bytes_per_row + (col / 8)];
            uint8_t bit = 7 - (col % 8);
            if (byte & (1u << bit)) {
                st7305_draw_pixel(dev, x + col, y + row, ST7305_COLOR_BLACK);
            }
        }
    }
}

/* === 高性能 1bpp 批量写入 ===
 * 直接写 ST7305 fb (PSRAM), 跳过 st7305_draw_pixel 的函数调用 + 边界检查.
 * 速度比 st7305_draw_bitmap_1bit 快 5-10x.
 *
 * ST7305 fb 字节序: 400 列 / 2 = 200 bytes per "vertical group",
 *                  300 行 / 4 = 75  "vertical groups" (1 byte = 2 列 x 4 行像素).
 * fb 总大小 200*75 = 15000 bytes. */
void st7305_blit_1bit(st7305_handle_t *dev,
                       int x0, int y0, int w, int h,
                       const uint8_t *bitmap) {
    if (!dev || !dev->fb || !bitmap) return;
    if (dev->pt_pfb) {
        /* 竖屏目标: 逐点写 pfb (慢但正确) */
        const int bpr = (w + 7) >> 3;
        for (int row = 0; row < h; row++) {
            const uint8_t *src = bitmap + row * bpr;
            for (int col = 0; col < w; col++)
                if (src[col >> 3] & (0x80u >> (col & 7)))
                    pt_px(dev->pt_pfb, x0 + col, y0 + row, 1);   /* 黑 */
        }
        return;
    }
    /* 裁剪到屏幕范围 */
    if (x0 < 0) { bitmap += (-x0) >> 3; w += x0; x0 = 0; }
    if (y0 < 0) { bitmap += (-y0) * ((w + 7) >> 3); h += y0; y0 = 0; }
    if (x0 + w > ST7305_WIDTH)  w = ST7305_WIDTH  - x0;
    if (y0 + h > ST7305_HEIGHT) h = ST7305_HEIGHT - y0;
    if (w <= 0 || h <= 0) return;

    const int bytes_per_row = (w + 7) >> 3;
    /* ST7305 的 fb 行索引: 每 4 个屏幕行 = 1 byte 高度的"位组", 跨 75 个组.
     * y 倒置 (ST7305 顶部 = fb 末尾) */
    for (int row = 0; row < h; row++) {
        int y = y0 + row;
        int inv_y = ST7305_HEIGHT - 1 - y;
        int y_group = inv_y >> 2;          /* 0..74 */
        int y_sub   = inv_y & 3;           /* 0..3 */
        const uint8_t *src = bitmap + row * bytes_per_row;
        for (int col = 0; col < w; col++) {
            uint8_t b = src[col >> 3];
            if (!(b & (0x80u >> (col & 7)))) continue;  /* 白点跳过 */
            int x = x0 + col;
            /* fb[idx] 的位 = 7 - ((y_sub<<1) | (x&1)) */
            int x_pair = x >> 1;
            uint8_t mask = (uint8_t)(1u << (7u - ((y_sub << 1) | (x & 1))));
            dev->fb[(uint32_t)x_pair * (ST7305_HEIGHT >> 2) + (uint32_t)y_group] &= ~mask;
        }
    }
}

/* 白像素版本: bitmap 位=1 -> 置位 fb (白), 位=0 -> 保持原样 (黑). */
void st7305_blit_1bit_white(st7305_handle_t *dev,
                            int x0, int y0, int w, int h,
                            const uint8_t *bitmap) {
    if (!dev || !dev->fb || !bitmap) return;
    if (dev->pt_pfb) {
        /* 竖屏目标: 位=1 写白 (清 pfb 位) */
        const int bpr = (w + 7) >> 3;
        for (int row = 0; row < h; row++) {
            const uint8_t *src = bitmap + row * bpr;
            for (int col = 0; col < w; col++)
                if (src[col >> 3] & (0x80u >> (col & 7)))
                    pt_px(dev->pt_pfb, x0 + col, y0 + row, 0);   /* 白 */
        }
        return;
    }
    if (x0 < 0) { bitmap += (-x0) >> 3; w += x0; x0 = 0; }
    if (y0 < 0) { bitmap += (-y0) * ((w + 7) >> 3); h += y0; y0 = 0; }
    if (x0 + w > ST7305_WIDTH)  w = ST7305_WIDTH  - x0;
    if (y0 + h > ST7305_HEIGHT) h = ST7305_HEIGHT - y0;
    if (w <= 0 || h <= 0) return;

    const int bytes_per_row = (w + 7) >> 3;
    for (int row = 0; row < h; row++) {
        int y = y0 + row;
        int inv_y = ST7305_HEIGHT - 1 - y;
        int y_group = inv_y >> 2;
        int y_sub   = inv_y & 3;
        const uint8_t *src = bitmap + row * bytes_per_row;
        for (int col = 0; col < w; col++) {
            uint8_t b = src[col >> 3];
            if (!(b & (0x80u >> (col & 7)))) continue;  /* 黑点跳过 */
            int x = x0 + col;
            int x_pair = x >> 1;
            uint8_t mask = (uint8_t)(1u << (7u - ((y_sub << 1) | (x & 1))));
            dev->fb[(uint32_t)x_pair * (ST7305_HEIGHT >> 2) + (uint32_t)y_group] |= mask;
        }
    }
}

/* 局部窗口: ST7305 接受 8-列粒度的列地址 (0..199, 1 step = 2 像素)
 * 和行地址 (0..199, 但 300 行实际 wrap). 我们直接传屏幕坐标. */
static int s_win_x0 = 0, s_win_y0 = 0, s_win_x1 = ST7305_WIDTH - 1, s_win_y1 = ST7305_HEIGHT - 1;

void st7305_set_window(st7305_handle_t *dev, int x, int y, int w, int h) {
    if (!dev) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ST7305_WIDTH)  w = ST7305_WIDTH  - x;
    if (y + h > ST7305_HEIGHT) h = ST7305_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    s_win_x0 = x; s_win_y0 = y;
    s_win_x1 = x + w - 1; s_win_y1 = y + h - 1;
    send_cmd(dev, 0x2A); { uint8_t d[] = {(uint8_t)s_win_x0, (uint8_t)s_win_x1}; send_data(dev, d, 2); }
    send_cmd(dev, 0x2B); { uint8_t d[] = {(uint8_t)s_win_y0, (uint8_t)s_win_y1}; send_data(dev, d, 2); }
}

void st7305_set_full_window(st7305_handle_t *dev) {
    if (!dev) return;
    s_win_x0 = 0; s_win_y0 = 0;
    s_win_x1 = ST7305_WIDTH - 1; s_win_y1 = ST7305_HEIGHT - 1;
    send_cmd(dev, 0x2A); { uint8_t d[] = {(uint8_t)s_win_x0, (uint8_t)s_win_x1}; send_data(dev, d, 2); }
    send_cmd(dev, 0x2B); { uint8_t d[] = {(uint8_t)s_win_y0, (uint8_t)s_win_y1}; send_data(dev, d, 2); }
}

static esp_err_t st7305_flush_from_raw(st7305_handle_t *dev, const uint8_t *fb); /* 前向声明 */
esp_err_t st7305_flush_from(st7305_handle_t *dev, const uint8_t *fb) {
    if (s_flush_blocked) return ESP_OK;   /* 屏保门禁: 屏保期间一律不刷屏 */
    return st7305_flush_from_raw(dev, fb);
}
/* 屏保壁纸独占强刷: 不受门禁限制 (屏保期间唯一允许写屏的通道) */
esp_err_t st7305_flush_override(st7305_handle_t *dev) {
    if (!dev || !dev->fb) return ESP_ERR_INVALID_ARG;
    return st7305_flush_from_raw(dev, dev->fb);
}
static esp_err_t st7305_flush_from_raw(st7305_handle_t *dev, const uint8_t *fb) {
    if (!dev || !fb) {
        return ESP_ERR_INVALID_ARG;
    }
    lcd_lock_ensure();
    xSemaphoreTake(s_lcd_lock, portMAX_DELAY);
    uint32_t t0 = esp_timer_get_time();

    /* 恢复横屏扫描方向 (阅读器旋转后可能停留在竖屏模式) */
    send_cmd(dev, 0x36); send_data_byte(dev, MADCTL_LANDSCAPE);
    send_cmd(dev, 0x2A); { uint8_t d[] = {CASET_START, CASET_END}; send_data(dev, d, 2); }
    send_cmd(dev, 0x2B); { uint8_t d[] = {RASET_START, RASET_END}; send_data(dev, d, 2); }
    send_cmd(dev, 0x2C);

    spi_transaction_t t = {
        .length = FB_BYTES * 8,
        .tx_buffer = fb,
        /* fb 在 PSRAM: SPI DMA 直接从 PSRAM 读, 免内部 15000B DMA 暂存 */
        .flags = SPI_TRANS_DMA_USE_PSRAM,
    };
    spi_device_polling_transmit(dev->spi, &t);

    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - t0);
    if (elapsed > 15000) {  /* >15ms 时打日志, 正常应该是 12-15ms */
        ESP_LOGW(TAG, "flush slow: %lu us", (unsigned long)elapsed);
    }
    xSemaphoreGive(s_lcd_lock);
    return ESP_OK;
}

/* 软件旋转用的横屏 fb 像素读写 (定义在文件尾部, 此处前向声明) */
static inline uint8_t st7305_fb_get_px(const uint8_t *fb, int x, int y);
static inline void    st7305_fb_set_px(uint8_t *fb, int x, int y, uint8_t white);

esp_err_t st7305_flush(st7305_handle_t *dev) {
    if (s_flush_blocked) return ESP_OK;   /* 屏保门禁: 屏保期间一律不刷屏 */
    if (!dev || !dev->fb) return ESP_ERR_INVALID_ARG;
    if (dev->r_flip1) {
        /* 横屏 180°: 压缩屏(MY行翻转会打散每字节4行)不能走硬件MADCTL,
         * 须在内存按像素翻转回横屏缓冲再送, 保证无白缝/不虚 */
        uint8_t *wk = dev->r_flip1;
        memset(wk, 0xFF, FB_BYTES);
        for (int y = 0; y < ST7305_HEIGHT; y++)
            for (int x = 0; x < ST7305_WIDTH; x++) {
                uint8_t w = st7305_fb_get_px(dev->fb, x, y);
                st7305_fb_set_px(wk, ST7305_WIDTH - 1 - x, ST7305_HEIGHT - 1 - y, w);
            }
        return st7305_flush_from(dev, wk);
    }
    if (dev->pt_pfb) {
        /* 竖屏绘制目标: 把 300×400 pfb 旋转回物理 400×300 fb 再送屏 (阅读器用自带 pfb, 不在此) */
        memset(dev->fb, 0xFF, FB_BYTES);   /* 全白底 */
        for (int Y = 0; Y < PT_H; Y++) {
            for (int X = 0; X < PT_W; X++) {
                int black = pt_get(dev->pt_pfb, X, Y);
                int fx = (dev->pt_rot == 2) ? (ST7305_WIDTH - 1 - Y) : Y;
                int fy = (dev->pt_rot == 2) ? X : (ST7305_HEIGHT - 1 - X);
                int inv_y = ST7305_HEIGHT - 1 - fy;
                uint32_t idx = (uint32_t)(fx >> 1) * FB_STRIDE + (uint32_t)(inv_y >> 2);
                uint8_t bit = 7u - (uint8_t)(((inv_y & 3) << 1) | (fx & 1));
                if (black) dev->fb[idx] &= (uint8_t)~(1u << bit);
                else       dev->fb[idx] |= (uint8_t)(1u << bit);
            }
        }
    }
    return st7305_flush_from(dev, dev->fb);
}

/* === 软件旋转输出 (阅读器) === */

/* 读横屏 fb 像素: 返回 1=白, 0=黑 */
static inline uint8_t st7305_fb_get_px(const uint8_t *fb, int x, int y) {
    int inv_y = ST7305_HEIGHT - 1 - y;
    uint32_t idx = (uint32_t)(x >> 1) * FB_STRIDE + (uint32_t)(inv_y >> 2);
    uint8_t bit = 7u - (uint8_t)(((inv_y & 3) << 1) | (x & 1));
    return (uint8_t)((fb[idx] >> bit) & 1u);
}

/* 写横屏 fb 像素 (1=白) */
static inline void st7305_fb_set_px(uint8_t *fb, int x, int y, uint8_t white) {
    int inv_y = ST7305_HEIGHT - 1 - y;
    uint32_t idx = (uint32_t)(x >> 1) * FB_STRIDE + (uint32_t)(inv_y >> 2);
    uint8_t bit = 7u - (uint8_t)(((inv_y & 3) << 1) | (x & 1));
    if (white) fb[idx] |= (uint8_t)(1u << bit);
    else       fb[idx] &= (uint8_t)~(1u << bit);
}

/* 写竖屏 fb 像素 (300 宽 x 400 高, 每字节 2 列 x 4 行) */
static inline void st7305_portrait_set_px(uint8_t *fb, int X, int Y, uint8_t white) {
    int inv_y = 399 - Y;
    uint32_t idx = (uint32_t)(X >> 1) * 100u + (uint32_t)(inv_y >> 2);
    uint8_t bit = 7u - (uint8_t)(((inv_y & 3) << 1) | (X & 1));
    if (white) fb[idx] |= (uint8_t)(1u << bit);
    else       fb[idx] &= (uint8_t)~(1u << bit);
}

esp_err_t st7305_flush_rotated(st7305_handle_t *dev, uint8_t rot, uint8_t *work_fb) {
    if (s_flush_blocked) return ESP_OK;   /* 屏保门禁 */
    if (!dev || !dev->fb) return ESP_ERR_INVALID_ARG;
    (void)work_fb;
    const uint8_t *src = dev->fb;

    if (rot == 0) {
        /* 横屏原样 */
        return st7305_flush(dev);
    }

    if (rot == 1) {
        /* 180°: 压缩屏不能走硬件 MADCTL(MY 会打散每字节4行→白缝),
         * 内存按像素翻转回横屏缓冲再送, 干净无白缝 */
        for (int y = 0; y < ST7305_HEIGHT; y++) {
            for (int x = 0; x < ST7305_WIDTH; x++) {
                uint8_t w = st7305_fb_get_px(src, x, y);
                st7305_fb_set_px(work_fb, ST7305_WIDTH - 1 - x, ST7305_HEIGHT - 1 - y, w);
            }
        }
        return st7305_flush_from(dev, work_fb);
    }

    /* 90° 竖屏: rot2=左(逆时针), rot3=右(顺时针)
     * 逻辑横屏像素 (x,y) -> 竖屏坐标 (X,Y), 再按竖屏 2列x4行格式重排 */
    memset(work_fb, 0xFF, FB_BYTES);   /* 白底 */
    for (int y = 0; y < ST7305_HEIGHT; y++) {
        for (int x = 0; x < ST7305_WIDTH; x++) {
            uint8_t w = st7305_fb_get_px(src, x, y);
            int X, Y;
            if (rot == 2) {
                X = y;              /* 左转: 横屏 x 轴 -> 竖屏 y 轴 */
                Y = ST7305_WIDTH - 1 - x;
            } else {
                X = ST7305_HEIGHT - 1 - y;
                Y = x;
            }
            st7305_portrait_set_px(work_fb, X, Y, w);
        }
    }

    /* 切竖屏扫描 + 竖屏窗口 (列=0..299, 行=0..399), 再发竖屏缓冲 */
    send_cmd(dev, 0x36); send_data_byte(dev, MADCTL_PORTRAIT);
    send_cmd(dev, 0x2A); { uint8_t d[] = {RASET_START, RASET_END}; send_data(dev, d, 2); }
    send_cmd(dev, 0x2B); { uint8_t d[] = {CASET_START, CASET_END}; send_data(dev, d, 2); }
    send_cmd(dev, 0x2C);

    spi_transaction_t t = {
        .length = FB_BYTES * 8,
        .tx_buffer = work_fb,
        .flags = SPI_TRANS_DMA_USE_PSRAM,
    };
    esp_err_t ret = spi_device_polling_transmit(dev->spi, &t);

    /* 发完恢复横屏扫描, 避免下次菜单绘制错乱 */
    send_cmd(dev, 0x36); send_data_byte(dev, MADCTL_LANDSCAPE);
    send_cmd(dev, 0x2A); { uint8_t d[] = {CASET_START, CASET_END}; send_data(dev, d, 2); }
    send_cmd(dev, 0x2B); { uint8_t d[] = {RASET_START, RASET_END}; send_data(dev, d, 2); }
    return ret;
}

st7305_config_t st7305_default_config(void) {
    return (st7305_config_t){
        .dc_gpio = 5,
        .rst_gpio = 41,
        .spi_host = SPI2_HOST,
        .spi_mosi = 12,
        .spi_sclk = 11,
        .spi_cs = 40,
        .spi_freq = 60,   /* ST7305 最大支持 60MHz, 提高刷新率 */
    };
}

/* 硬件对比度调整 - 通过 0xC1 寄存器修改 VOP 电压
 * ST7305 0xC1 接收 4 字节, 对应 4 个 gate driver 的 VOP[5:0]
 * VOP 越高, LCD 驱动电压越高, 像素越深 (对比度越大)
 * 默认值 0x41 - 我们限制在 0x20..0x3F (6-bit 范围内)
 */
esp_err_t st7305_set_contrast(st7305_handle_t *dev, uint8_t contrast) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    /* 限制到 6-bit 范围, 默认 0x21 (33) */
    if (contrast > 0x3F) contrast = 0x3F;
    uint8_t d[4] = {contrast, contrast, contrast, contrast};
    send_cmd(dev, 0xC1);
    send_data(dev, d, 4);
    return ESP_OK;
}

/* 反色显示开关
 * V1.0.68 fix: 开机初始化已发 0x21 (INVON, bit=1 白). "反色" 应相对此默认:
 *   on(夜间/反色) → 0x20 (INVOFF) 才真正翻转画面;
 *   off(正常)     → 0x21 (INVON) 恢复默认.
 * 旧代码 on→0x21/off→0x20 与默认同向: 夜间开无效, 夜间关反而整屏反色并泄漏到主菜单. */
esp_err_t st7305_set_inversion(st7305_handle_t *dev, bool on) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    /* 0x21 = INVON (默认, 正常), 0x20 = INVOFF (反色) */
    send_cmd(dev, on ? 0x20 : 0x21);
    return ESP_OK;
}

/* 低功耗模式开关 - 0xD1 寄存器
 * on=true: 低功耗 (慢速, 省电), on=false: 高功耗 (清晰)
 */
esp_err_t st7305_set_low_power(st7305_handle_t *dev, bool on) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    send_cmd(dev, 0xD1);
    send_data_byte(dev, on ? 0x01 : 0x00);
    return ESP_OK;
}

/* 运行时设置帧率控制 (0xB2) — 测试不同刷新率对细纹/棋盘闪烁的影响 */
esp_err_t st7305_set_frame_rate(st7305_handle_t *dev, uint8_t v) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    send_cmd(dev, 0xB2);
    send_data_byte(dev, v);
    return ESP_OK;
}
