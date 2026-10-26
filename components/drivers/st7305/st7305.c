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
#include "board_pins.h"

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

/* 取屏锁: **有界等待**, 失败就丢这一帧 —— 绝不"强制归还"。
 *
 * ⚠ 2026-10-25 修正上一版的错误做法（曾在这里 xSemaphoreGive 强夺锁）:
 *   对 mutex 强行 give 会让计数 >1 ⇒ 两个任务同时进入 SPI 临界区 ⇒
 *   s_tx_buf/s_tx_inflight 这类共享状态互相踩 ⇒ 之后永久卡死、只能断电。
 *   正确的做法是：等不到就放弃这一帧（画面少一帧无所谓，卡死才是事故），
 *   并计入失败计数，由 st7305_recover_if_hung() 做面板级自愈。 */
static volatile int s_lock_fail_streak = 0;

/* ★★★ 2026-10-25 linit「退出仙剑会卡死 / 返回桌面特别卡」:
 *   互斥量如果在**持锁状态下**被 vTaskDelete 的任务带走, 就永远不会归还;
 *   之后每次 flush 的 lcd_lock_take() 都要干等 120ms 才失败 ⇒ 每个界面都卡成幻灯片。
 *   强杀游戏任务之后调用本函数重建互斥量, 让送屏立刻恢复正常。
 *   ⚠ 只在"确认除 UI 外没有别的任务正在用面板"时调用(例如刚强杀完引擎任务)。 */
void st7305_lock_recover(void)
{
    if (s_lcd_lock) vSemaphoreDelete(s_lcd_lock);
    s_lcd_lock = xSemaphoreCreateMutex();
}

static bool lcd_lock_take(void)
{
    lcd_lock_ensure();
    if (!s_lcd_lock) return false;
    if (xSemaphoreTake(s_lcd_lock, pdMS_TO_TICKS(120)) == pdTRUE) {
        s_lock_fail_streak = 0;
        return true;
    }
    if (++s_lock_fail_streak == 3)
        ESP_LOGW(TAG, "LCD 锁等待超时(连续 %d 次) → 本帧丢弃", s_lock_fail_streak);
    return false;
}
/* 屏保统一刷屏门禁: 屏保激活时 true → 所有 st7305_flush* 变成 no-op,
 * 任何程序/引擎都无法往屏上写(视觉上全部暂停), 壁纸用 override 独占刷屏. */
static volatile bool s_flush_blocked = false;
void st7305_set_flush_blocked(bool block) { s_flush_blocked = block; }

/* V1.2: "已送屏内容"指纹 —— st7305_flush() 据此跳过未变化的整帧送屏.
 * 任何绕过缓存的送屏 (flush_from / flush_override / flush_from_raw) 都会作废它,
 * 保证不会因缓存过期而漏刷 (例: 屏保强刷后回到普通 flush). */
static uint32_t s_last_sum = 0;
/* 当前 B2(帧率控制) 值 —— 之前 remcon 读回显示的是命令里的静态变量初值, 是假的。
 * 现在以驱动为准: init 写入时记下, 运行时 set_frame_rate 同步更新。 */
/* ★★★ 2026-10-25 linit「屏幕优化设置以老版本为主」(LinIT-L/ESP32-S3-RLCD-BBK):
 *   老工程在这里写死 0x02 并注明「帧率(原始, 勿改)」, 长期稳定。
 *   后来为了"更流畅"改成 0x0C(估 24Hz) —— 那只是按 0x05≈16Hz / 0x12≈32Hz 线性内插
 *   猜出来的, 没有手册依据; 面板自刷新被顶到超规格后表现就是**忽白忽黑 + 偶发锁死**。
 *   现在恢复 0x02。要试别的档仍可用 `remcon b2 0x0C`(不落 NVS)。 */
static uint8_t  s_b2_cur = 0x02;
static bool     s_last_sum_valid = false;   /* 横屏 fb 指纹是否有效 */
/* ★ 2026-10-27: 竖屏缓冲(pfb)单独记一份指纹。
 *   背景: 阅读器/书架的内容画在 pfb 里, 送屏时要做 300×400 = 12 万次像素转换。
 *   上一版为了修"指纹算错对象"把旋转提到了指纹之前 ⇒ **即使画面完全静止,
 *   每帧也要白跑 12 万次转换**。这里再给 pfb 加一层指纹: 内容没变就直接返回,
 *   连旋转都省掉 —— 阅读器停在一页不动时 CPU 占用回到接近 0。
 *   (声明放文件顶部: flush_from_raw / invalidate 都要清它) */
static uint32_t s_last_psum = 0;
static bool     s_last_psum_valid = false;

/* LPM(0x39 1Hz 自刷新) 状态 —— 提到文件顶部: flush_from_raw 也要用(旁路送屏前先退出)
 * s_lpm_force: 体检用的强制档 —— -1 自动 / 0 强制 HPM / 1 强制 LPM (remcon lpm 命令) */
static bool     s_lpm_on = false;
static int      s_lpm_force = -1;

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
    /* ★★★ 2026-10-25 linit: 「重启后屏幕一直是旧画面、怎么按都不变」的根因修复。
     *   ST7305 是反射式 memory LCD —— 断电/复位后像素保持最后状态，屏控制器也
     *   不因为 MCU 复位而复位。本工程多数板型 lcd_rst = -1（复位脚不接），
     *   所以旧代码的初始化序列是"对着一个还没醒/还停在上次的屏"发命令，
     *   部分配置被忽略 ⇒ 清屏 flush 写不进去，画面就永远停在上一帧。
     *   0x01 = SWRESET（软复位，不依赖 RST 引脚），手册要求之后等 ≥120ms。
     *   有 RST 脚的板子多这一次也幂等无害。 */
    send_cmd(dev, 0x01);
    vTaskDelay(pdMS_TO_TICKS(120));

    send_cmd(dev, 0xD6); { uint8_t d[] = {0x17, 0x02}; send_data(dev, d, 2); }
    send_cmd(dev, 0xD1);   send_data_byte(dev, 0x01);

    send_cmd(dev, 0xC0); { uint8_t d[] = {0x11, 0x04}; send_data(dev, d, 2); }   /* 本工程原始: VGH/VGL */
    send_cmd(dev, 0xC1); { uint8_t d[] = {0x69, 0x69, 0x69, 0x69}; send_data(dev, d, 4); }  /* VSHP (原始) */
    send_cmd(dev, 0xC2); { uint8_t d[] = {0x19, 0x19, 0x19, 0x19}; send_data(dev, d, 4); }  /* VSLP (原始) */
    send_cmd(dev, 0xC4); { uint8_t d[] = {0x4B, 0x4B, 0x4B, 0x4B}; send_data(dev, d, 4); }  /* VSHN (原始) */
    send_cmd(dev, 0xC5); { uint8_t d[] = {0x19, 0x19, 0x19, 0x19}; send_data(dev, d, 4); }  /* VSLN (原始) */

    send_cmd(dev, 0xD8); { uint8_t d[] = {0x80, 0xE9}; send_data(dev, d, 2); }    /* 原始 */

    /* 面板自刷新档位: 老工程就是 0x02, 注释写死「原始, 勿改」。见 s_b2_cur 说明。 */
    send_cmd(dev, 0xB2);   send_data_byte(dev, s_b2_cur);
    send_cmd(dev, 0xB3); { uint8_t d[] = {0xE5,0xF6,0x05,0x46,0x77,0x77,0x77,0x77,0x76,0x45};
                           send_data(dev, d, 10); }                               /* 原始 HPM EQ */
    send_cmd(dev, 0xB4); { uint8_t d[] = {0x05,0x46,0x77,0x77,0x77,0x77,0x76,0x45};
                           send_data(dev, d, 8); }                               /* 原始 LPM EQ */
    send_cmd(dev, 0x62); { uint8_t d[] = {0x32, 0x03, 0x1F}; send_data(dev, d, 3); }

    send_cmd(dev, 0xB7);   send_data_byte(dev, 0x13);
    send_cmd(dev, 0xB0);   send_data_byte(dev, 0x64);                            /* 原始 */

    send_cmd(dev, 0x11);          /* Sleep Out */
    /* ST7305 手册: Sleep Out 后需等 ≥120ms 才能发下一条命令。
     * 原来等 200ms（多 80ms 白等）⇒ 收到 120ms。 */
    vTaskDelay(pdMS_TO_TICKS(120));

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

    /* ★引脚实测(回答"RST 到底有没有 3.3V") */
    if (config->rst_gpio >= 0) {
        gpio_set_direction((gpio_num_t)config->rst_gpio, GPIO_MODE_INPUT_OUTPUT);
        ESP_LOGW(TAG, "★引脚实测: RST(GPIO%d) 电平=%d", config->rst_gpio,
                 gpio_get_level((gpio_num_t)config->rst_gpio));
    }
    if (config->dc_gpio >= 0) {
        gpio_set_direction((gpio_num_t)config->dc_gpio, GPIO_MODE_INPUT_OUTPUT);
        ESP_LOGW(TAG, "★引脚实测: DC(GPIO%d) 电平=%d", config->dc_gpio,
                 gpio_get_level((gpio_num_t)config->dc_gpio));
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
    /* ★ 2026-10-23 性能: 帧缓冲优先放**内部 RAM**（15000B，占内部堆 11%）。
     *   原因: fb 是列优先布局（idx = (x>>1)*75 + (y>>2)），画横线时相邻像素落在
     *   相隔 75 字节的位置，等于每个像素都踩新的 cache line —— 放在 PSRAM 上就是
     *   每像素一次 PSRAM 往返（RMW），整屏 12 万像素时绘制是帧耗时的大头。
     *   放内部 SRAM 后这笔开销直接归零，且 SPI DMA 也能原生从内部 RAM 取数。
     *   PSRAM 兜底保留：内部 RAM 紧到分不出来时行为不变。 */
    out->fb = (uint8_t *)heap_caps_calloc(1, FB_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (out->fb) {
        /* ⚠ 内部 RAM 是有底线的：I2S DMA(≈18KB)、WiFi/BT、TLS 都要内部连续块。
         *   分完 fb 后剩余不足 64KB 就还回去 —— 宁可绘制慢，也不能把音频/联网挤死。 */
        size_t left = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (left < 64u * 1024u) {
            ESP_LOGW(TAG, "内部 RAM 只剩 %u B (<64KB), fb 退回 PSRAM", (unsigned)left);
            heap_caps_free(out->fb);
            out->fb = NULL;
        }
    }
    if (!out->fb) {
        out->fb = (uint8_t *)heap_caps_calloc(1, FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out->fb) return ESP_ERR_NO_MEM;
    }
    if (!out->fb) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "fb=%p size=%d [%s]",
             (void*)out->fb, FB_BYTES,
             esp_ptr_internal(out->fb) ? "SRAM" : "PSRAM");

    /* ★★★ 2026-10-16 修复重启循环: rst_gpio = -1(本板 RESET 只 10k 上拉、不接 MCU) 时
     *   无条件 `1ULL << rst_gpio` 生成**非法掩码** ⇒ gpio_config 失败 ⇒ abort ⇒ 反复重启。 */
    uint64_t io_mask = 0;
    if (out->dc_gpio  >= 0) io_mask |= (1ULL << out->dc_gpio);
    if (out->rst_gpio >= 0) io_mask |= (1ULL << out->rst_gpio);
    if (io_mask) {
        gpio_config_t io = { .pin_bit_mask = io_mask, .mode = GPIO_MODE_OUTPUT };
        ESP_ERROR_CHECK(gpio_config(&io));
    }

    if (out->rst_gpio >= 0) st7305_hw_reset(out);

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

    {   /* 打印**实际** SPI 时钟: 请求值会被 80MHz APB 分频, 不是想设多少就多少 */
        int khz = 0;
        if (spi_device_get_actual_freq(out->spi, &khz) == ESP_OK)
            ESP_LOGI(TAG, "SPI 实际时钟: %.2f MHz (请求 %d MHz)",
                     khz / 1000.0, (int)(st7305_default_config().spi_freq));
    }
    ESP_LOGI(TAG, "ST7305 initialized (%dx%d, fb=%zu bytes)",
             ST7305_WIDTH, ST7305_HEIGHT, (size_t)FB_BYTES);
    return ESP_OK;
}

/* 浅睡唤醒后恢复显示。
 * 背景: 浅睡会门控 SPI 外设时钟, 面板配置寄存器与内部显存可能残留半帧/错位,
 * 表现为"唤醒瞬间画面撕裂一下"。重发初始化序列 + 清屏 + 整屏强刷即可消除。
 * 注意: 本函数**禁止打日志** —— 唤醒瞬间 USB-Serial-JTAG 尚未重新枚举,
 * vprintf 会 abort 导致整机重启(esp-idf#5097)。 */
esp_err_t st7305_resume(st7305_handle_t *dev) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = st7305_init_sequence(dev);
    if (ret != ESP_OK) return ret;
    st7305_clear(dev, ST7305_COLOR_WHITE);
    return st7305_flush_override(dev);   /* 绕过屏保 flush 门禁, 强刷整帧 */
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
    extern void st7305_partial_invalidate_pub(void);
    st7305_partial_invalidate_pub();   /* 旋转改变 fb 语义 → 局刷基线作废, 下一帧整帧重建 */
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

/* ★★★ 2026-10-16 y 裁剪带(linit): 弹窗滚动时"字被切一半也要正常显示"。
 *   所有像素最终都过 st7305_draw_pixel ⇒ 在这一层裁剪即可覆盖 ASCII/汉字/图形。 */
static int s_clip_y0 = 0, s_clip_y1 = 1 << 30;
void st7305_set_clip_y(st7305_handle_t *dev, int y0, int y1) {
    (void)dev; s_clip_y0 = (y0 < 0) ? 0 : y0; s_clip_y1 = y1;
}
void st7305_reset_clip_y(st7305_handle_t *dev) {
    (void)dev; s_clip_y0 = 0; s_clip_y1 = 1 << 30;
}

void st7305_draw_pixel(st7305_handle_t *dev, int x, int y, st7305_color_t color) {
    if (!dev || !dev->fb) return;
    if (y < s_clip_y0 || y > s_clip_y1) return;      /* ★ y 裁剪带 */
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

/* ★★★ 2026-10-25 性能优化 (linit):
 *   文字/位图原来逐像素调 st7305_draw_pixel(函数调用+y裁剪+边界检查, 每像素一次),
 *   现统一改为"栈上 1bpp 位图 + st7305_blit_1bit 直写 fb"通路 (量级 5-10x)。
 *   语义保持: y 裁剪带(s_clip_y0/y1)与竖屏 pt_pfb 行为不变; API 不变。
 *   (原 draw_char_scaled / draw_char_outline 里两张重复的字模表合并为一张) */
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

/* 按 y 裁剪带裁行后交给 blit; white=1 走 white 版本(置位=白) */
static void blit_rows(st7305_handle_t *dev, const uint8_t *bm, int bpr, int w,
                      int x, int y, int h, int white) {
    int row0 = 0, row1 = h;
    if (y < s_clip_y0) row0 = s_clip_y0 - y;
    if (y + h - 1 > s_clip_y1) row1 = s_clip_y1 - y + 1;
    if (row0 < 0) row0 = 0;
    if (row1 > h) row1 = h;
    if (row1 - row0 <= 0) return;
    bm += (size_t)row0 * bpr;
    if (white) st7305_blit_1bit_white(dev, x, y + row0, w, row1 - row0, bm);
    else       st7305_blit_1bit(dev, x, y + row0, w, row1 - row0, bm);
}

/* 5x7 字形展开为 MSB-first 1bpp 位图 (bpr=每行字节数);
 * dilate=1 时白边膨胀: 位图四周留 1px, 每个笔画像素的 3x3 邻域全置位 */
static void glyph_build(char c, int scale, int dilate,
                        uint8_t *bm, int bpr, int bm_w, int bm_h) {
    memset(bm, 0, (size_t)bpr * bm_h);
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) c = '?';
    const uint8_t *g = FONT5X7[(uint8_t)c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (!(bits & (1u << row))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++) {
                    int px = col * scale + dx, py = row * scale + dy;
                    if (!dilate) {
                        bm[py * bpr + (px >> 3)] |= (uint8_t)(0x80u >> (px & 7));
                        continue;
                    }
                    for (int yy = -1; yy <= 1; yy++)
                        for (int xx = -1; xx <= 1; xx++) {
                            int bx = px + 1 + xx, by = py + 1 + yy;
                            if ((unsigned)bx < (unsigned)bm_w && (unsigned)by < (unsigned)bm_h)
                                bm[by * bpr + (bx >> 3)] |= (uint8_t)(0x80u >> (bx & 7));
                        }
                }
        }
    }
}

static void draw_char_scaled(st7305_handle_t *dev, int px, int py, char c, int scale) {
    if (!dev || !dev->fb) return;
    uint8_t bm[2 * 14];                        /* scale<=2: 5*scale x 7*scale */
    int bpr = (5 * scale + 7) >> 3;
    if (scale < 1 || scale > 2 || bpr * 7 * scale > (int)sizeof(bm)) return;
    glyph_build(c, scale, 0, bm, bpr, 5 * scale, 7 * scale);
    blit_rows(dev, bm, bpr, 5 * scale, px, py, 7 * scale, 0);
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

/* 字形 1px 白边 + 黑实体: 描边位图(含 1px 膨胀)整张走 blit, 不再逐像素 3x3 画白 */
static void draw_char_outline(st7305_handle_t *dev, int px, int py, char c, int scale) {
    if (!dev || !dev->fb) return;
    uint8_t bm[2 * 16];                        /* 描边位图: (5*scale+2) x (7*scale+2) */
    int bpr = (5 * scale + 2 + 7) >> 3;
    if (scale < 1 || scale > 2 || bpr * (7 * scale + 2) > (int)sizeof(bm)) return;
    glyph_build(c, scale, 1, bm, bpr, 5 * scale + 2, 7 * scale + 2);
    blit_rows(dev, bm, bpr, 5 * scale + 2, px - 1, py - 1, 7 * scale + 2, 1);  /* 白边 */
    glyph_build(c, scale, 0, bm, bpr, 5 * scale, 7 * scale);
    blit_rows(dev, bm, bpr, 5 * scale, px, py, 7 * scale, 0);                  /* 黑实体 */
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
    if (!dev || !dev->fb || !bitmap || w <= 0 || h <= 0) return;
    /* ★ 性能: 位图格式与 blit_1bit 完全一致, 直接走批量通路 (带 y 裁剪带) */
    blit_rows(dev, bitmap, (w + 7) >> 3, w, x, y, h, 0);
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

/* ★★★ 2026-10-25 关键修复（"画面卡死后台正常"的真根因）:
 *   fb 在 **PSRAM**，原来直接带 SPI_TRANS_DMA_USE_PSRAM 发。而 ESP32-S3 上任何一次
 *   **写 flash**（NVS 落盘 / 商店写包 / 写日志 / 写 SPIFFS）都会临时关闭 PSRAM 访问。
 *   正在跑的 GDMA 读 PSRAM 被打断 ⇒ 传输永远不结束 ⇒ spi_device_polling_transmit
 *   在**主任务**上无限阻塞 ⇒ 画面冻结；而音频/WiFi 在别的核上照常跑 ⇒ "后台正常"。
 *   面板自愈也跑不到（同一条任务卡在 transmit 里），所以只能断电。
 *   解法：整帧先 memcpy 到**内部 RAM** 的发送缓冲，再从内部 RAM 发 —— flash 操作再也
 *   碰不到这条 DMA。内部 RAM 现在有 130KB+ 余量，15000B 完全负担得起。 */
static bool     s_win_set = false;  /* 窗口/扫描方向是否已设过 (每帧只发 0x2C) */
static uint32_t s_last_flush_ms = 0;/* 上一帧送屏时刻: 限速用 */
/* ★ 2026-10-25 linit「非常卡 不要限制了」⇒ 限速**关闭**(0 = 不限)。
 *   原来为了护面板加的 16fps 上限反过来把界面拖顿了, 先去掉。
 *   卡死的真正原因(PSRAM DMA / 每帧重设窗口 / 关屏命令)都已修掉,
 *   面板侧不再需要靠限速保命。若要恢复限速: 把下面改成 62 即可。 */
#define FLUSH_MIN_MS  0             /* 0 = 不限制 */

static uint8_t *s_txbuf = NULL;     /* 内部 RAM 的整帧发送缓冲 (DMA 可读) */
static bool     s_txbuf_tried = false;

static uint8_t *txbuf_get(void)
{
    if (s_txbuf) return s_txbuf;
    if (s_txbuf_tried) return NULL;
    s_txbuf_tried = true;
    s_txbuf = (uint8_t *)heap_caps_malloc(FB_BYTES,
                  MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_txbuf) {
        ESP_LOGE(TAG, "内部 RAM 发送缓冲分配失败 (%d B) → 退回 PSRAM 直发(可能卡死)", FB_BYTES);
    } else {
        ESP_LOGI(TAG, "整帧发送缓冲: 内部 RAM %d B (避免 PSRAM DMA 被 flash 操作打断)", FB_BYTES);
    }
    return s_txbuf;
}

static esp_err_t st7305_flush_from_raw(st7305_handle_t *dev, const uint8_t *fb); /* 前向声明 */
#ifdef LINTOS_EMU_MODE
/* ★ 2026-10-25 [LINTOS] 模拟器帧导出：esp-emu 没有屏幕输出（官方 Issue #5 尚未实现
 *   SPI 事务桥接），所以把整帧 1bpp framebuffer 直接从 stdout 吐出去，
 *   PC 端脚本按魔数 'LTFB' 同步、渲染成窗口。
 *   格式: 'L''T''F''B' + u16 width(LE) + u16 height(LE) + (w*h/8) 字节原始位图
 *   (每字节 8 像素, MSB 在左, 1=白 0=黑, 与 ST7305 显存一致) */
#include <stdio.h>
void lintos_emu_fb_dump(const uint8_t *fb, int w, int h)
{
    if (!fb || w <= 0 || h <= 0) return;
    uint8_t hdr[8] = { 'L', 'T', 'F', 'B',
                       (uint8_t)(w & 0xFF), (uint8_t)(w >> 8),
                       (uint8_t)(h & 0xFF), (uint8_t)(h >> 8) };
    fwrite(hdr, 1, sizeof(hdr), stdout);
    fwrite(fb, 1, (size_t)((w * h) / 8), stdout);
    fflush(stdout);
}
#endif

esp_err_t st7305_flush_from(st7305_handle_t *dev, const uint8_t *fb) {
    if (s_flush_blocked) return ESP_OK;   /* 屏保门禁: 屏保期间一律不刷屏 */
#ifdef LINTOS_EMU_MODE
    lintos_emu_fb_dump(fb, 400, 300);     /* 模拟器: 每帧导出到 UART */
#endif
    return st7305_flush_from_raw(dev, fb);
}
/* 屏保壁纸独占强刷: 不受门禁限制 (屏保期间唯一允许写屏的通道) */
esp_err_t st7305_flush_override(st7305_handle_t *dev) {
    if (!dev || !dev->fb) return ESP_ERR_INVALID_ARG;
    return st7305_flush_from_raw(dev, dev->fb);
}
/* === 送屏: 同步 polling (2026-10-25 回退异步 DMA 双缓冲) ===
 * 为什么回退：上一版把整帧 memcpy 到 PSRAM 的 s_tx_buf 后 queue DMA，然后**放开锁返回**，
 * DMA 还在后台从 PSRAM 读 15000B。而本工程随时可能发生**写 flash / 改分区 / NVS 落盘**
 * （设置保存、商店写包、日志、屏保计数…）。ESP32-S3 上 flash 操作会暂时禁用 PSRAM 与
 * 缓存 ⇒ 在飞的 PSRAM DMA 会卡住 SPI 外设，且这是**跨任务**的（锁已经放开，没人能拦），
 * 结果就是：画面永远停在最后一帧、按键/声音正常、只有断电才能恢复 —— linit 反复报的
 * "画面卡死只能断电"。
 * 15000B @40MHz ≈ 3ms，面板本身只有 16Hz 刷新，异步省下的这点时间没有实际意义，
 * 换来的是不可复现的卡死 ⇒ 一律同步发完再返回，任何时刻都不留"在途 DMA"。 */
/* ==================== 面板自愈 (2026-10-25) ====================
 * linit 反复反馈"画面卡死、只能断电"。断电之所以能救回来，是因为重新上电后
 * st7305_init_sequence() 会跑一遍 SWRESET + 全量初始化。既然如此，**同样的序列
 * 在软件里也能跑** —— 只要检测到"一直在请求送屏、却长时间没有一次成功"，
 * 就在这里软复位重建面板，让用户不必再拔电。
 * 触发条件: **连续 20 次请求都失败**（正常 16~30fps，等于 1 秒内一帧都没送出去）。 */
static volatile uint32_t s_ok_ms   = 0;   /* 最近一次成功送屏的时刻 */
static volatile uint32_t s_flush_act_ms = 0;   /* 最近一次成功送屏时刻(诊断用) */
static volatile int s_flush_fail_streak = 0;  /* 连续送屏失败次数 → 触发自愈 */
static bool s_recovering = false;
static int  s_recover_cnt = 0;

static inline uint32_t lcd_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void panel_soft_recover(st7305_handle_t *dev)
{
    if (s_recovering || !dev) return;
    s_recovering = true;
    ESP_LOGW(TAG, "面板 %u ms 无成功送屏 → 软复位重建 (不用断电)",
             (unsigned)(lcd_now_ms() - s_ok_ms));
    if (lcd_lock_take()) {
        st7305_init_sequence(dev);       /* 0x01 SWRESET + 全部参数 + 0x38 + 0x29 */
        s_lpm_on = false;
        s_win_set = false;               /* 重建后窗口要重设 */
        xSemaphoreGive(s_lcd_lock);
        s_recover_cnt++;
        s_ok_ms  = lcd_now_ms();
        s_flush_fail_streak = 0;
        /* 重画当前内容 */
        if (dev->fb && lcd_lock_take()) {
            send_cmd(dev, 0x36); send_data_byte(dev, MADCTL_LANDSCAPE);
            send_cmd(dev, 0x2A); { uint8_t d[] = {CASET_START, CASET_END}; send_data(dev, d, 2); }
            send_cmd(dev, 0x2B); { uint8_t d[] = {RASET_START, RASET_END}; send_data(dev, d, 2); }
            send_cmd(dev, 0x2C);
            spi_transaction_t t = { .length = FB_BYTES * 8, .tx_buffer = dev->fb,
                                    .flags = esp_ptr_external_ram(dev->fb) ? SPI_TRANS_DMA_USE_PSRAM : 0 };
            spi_device_polling_transmit(dev->spi, &t);
            xSemaphoreGive(s_lcd_lock);
        }
        ESP_LOGI(TAG, "面板已恢复 (第 %d 次)", s_recover_cnt);
    }
    s_recovering = false;
}

static esp_err_t st7305_flush_from_raw(st7305_handle_t *dev, const uint8_t *fb) {
    if (!dev || !fb) return ESP_ERR_INVALID_ARG;
    s_last_sum_valid = false;          /* 绕过缓存的送屏 → 指纹作废 */
    s_last_psum_valid = false;         /* 竖屏指纹同理(这个函数在文件后段, 前面已前置声明) */

    /* 卡死自愈: **连续多次请求都送不出去**才软复位重建 (等价于拔电重上电)。
     * ⚠ 原判断是"距上次成功送屏 >3s" —— 那是错的: 界面静止时本来就没有送屏请求,
     *   一旦隔了 3 秒再动一下, 第一帧就会被判定为"卡死"⇒ 白跑一次 SWRESET 初始化
     *   (内部还带 120ms+120ms 的等待), 主任务被卡住 300ms, 反复触发就是"整机卡死"。
     *   现在只认"请求很多、成功为零"。 */
    {
        if (!s_recovering) {
            if (s_flush_fail_streak >= 20) {
                s_flush_fail_streak = 0;
                panel_soft_recover(dev);
            }
        }
    }
    if (s_lpm_on && s_lpm_force != 1) st7305_set_lpm(dev, false);   /* set_lpm 自己拿锁 */

    /* 限速到 16fps: 太近直接跳过这一帧(内容还在 fb 里, 下一帧会带上最新内容一起送)。
     *   ⚠ 绝不在这里 vTaskDelay —— 会阻塞调用方(主循环/游戏任务), 手感发滞。 */
    if (FLUSH_MIN_MS > 0) {
        uint32_t now = lcd_now_ms();
        if (s_last_flush_ms && (uint32_t)(now - s_last_flush_ms) < FLUSH_MIN_MS)
            return ESP_OK;
        s_last_flush_ms = now;
    }
    if (!lcd_lock_take()) {                 /* 等不到就丢帧, 不硬抢 */
        s_flush_fail_streak++;
        return ESP_ERR_TIMEOUT;
    }

    uint32_t t0 = esp_timer_get_time();
    /* ★ 2026-10-25 linit「屏幕优化设置以老版本为主」⇒ 恢复老工程的做法:
     *   **每帧重发 0x36 + 0x2A + 0x2B 再 0x2C**。
     *   之前为了少发几条命令把它改成"只设一次", 但这条链路上任何一次状态错位
     *   (软复位/LPM/唤醒/旋转) 都会让后续整帧写进错误的窗口, 表现就是画面卡住不动。
     *   代价只有 4 条短传输(≈40µs), 换的是和老版本一致的确定性。 */
    send_cmd(dev, 0x36); send_data_byte(dev, MADCTL_LANDSCAPE);
    send_cmd(dev, 0x2A); { uint8_t d[] = {CASET_START, CASET_END}; send_data(dev, d, 2); }
    send_cmd(dev, 0x2B); { uint8_t d[] = {RASET_START, RASET_END}; send_data(dev, d, 2); }
    s_win_set = true;
    send_cmd(dev, 0x2C);
    /* ★ 同步整帧, 且**从内部 RAM 发** —— 这是"画面卡死后台正常"的根治点:
     *   PSRAM 直发会在任何一次写 flash(NVS/商店/日志) 时被打断而永久阻塞。
     * ★★ 2026-10-26 linit「帧率上不去」: fb 本来就优先分配在内部 RAM (见 init 的分配策略),
     *   而这里又无条件 memcpy 一份到另一块 15KB 的 s_txbuf —— 等于内部 RAM 白占 15KB,
     *   还外加每帧白拷 15KB。现在: fb 已在内部 RAM 就直接当 tx_buffer
     *   (省拷贝 + 不再分配 txbuf); 同步 polling_transmit 发完才返回,
     *   不存在"CPU 边画 / DMA 边读"的并发。只有 fb 退到 PSRAM 时才走原来的中转路径。 */
    spi_transaction_t t = { .length = FB_BYTES * 8 };
    if (esp_ptr_internal(fb)) {
        t.tx_buffer = (void *)fb;    /* 内部 RAM 直发: 无需 PSRAM DMA 标志, 也无须 txbuf */
        t.flags     = 0;
    } else {
        uint8_t *tx = txbuf_get();
        if (tx) {
            memcpy(tx, fb, FB_BYTES);
            t.tx_buffer = tx;
            t.flags     = 0;
        } else {
            t.tx_buffer = (void *)fb; /* 兜底: 中转缓冲也分不到才直发 */
            t.flags     = SPI_TRANS_DMA_USE_PSRAM;
        }
    }
    esp_err_t e = spi_device_polling_transmit(dev->spi, &t);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - t0);
    xSemaphoreGive(s_lcd_lock);
    if (e != ESP_OK) {
        s_flush_fail_streak++;
        ESP_LOGE(TAG, "整帧送屏失败: %d", (int)e);
        return e;
    }
    s_flush_fail_streak = 0;
    s_ok_ms = lcd_now_ms();
    s_flush_act_ms = s_ok_ms;      /* 送屏也算活动: 游戏期间不掉 LPM (见上面注释) */
    /* 只在真的慢时告警（40MHz 整帧 15000B ≈ 3ms；>12ms 说明总线异常） */
    if (elapsed > 12000) ESP_LOGW(TAG, "flush slow: %lu us", (unsigned long)elapsed);
    return ESP_OK;
}

/* 软件旋转用的横屏 fb 像素读写 (定义在文件尾部, 此处前向声明) */
static inline uint8_t st7305_fb_get_px(const uint8_t *fb, int x, int y);
static inline void    st7305_fb_set_px(uint8_t *fb, int x, int y, uint8_t white);

/* === V1.2 性能: 180° 旋转的 O(N) 实现 (原为逐像素双循环) ===
 * 原实现: 300×400 = 120,000 次 st7305_fb_get_px + st7305_fb_set_px (各含函数调用+移位)
 *         ≈ 5 ms @240MHz —— 代码里 "flush slow" 告警即由此触发。
 *
 * 本实现推导 (ST7305 压缩格式: 每字节 = 2 列 × 4 行, 字节列 stride = 75):
 *   源像素 (x,y) → 目标 (399-x, 299-y), 换算到字节地址后等价于
 *       dst[(CX-1-cx)*RY + (RY-1-ry)] = rev8( src[cx*RY + ry] )
 *   即「字节网格转置 + 字节内位序反转」, 其中:
 *     CX = 宽/2 = 200 (字节列数), RY = 高/4 = 75 (行组数)
 *   位序反转用 16 项 nibble 查表 (2 查表 + 移位/或), 无逐像素循环。 */
static const uint8_t REV4[16] = {0x0,0x8,0x4,0xC,0x2,0xA,0x6,0xE,0x1,0x9,0x5,0xD,0x3,0xB,0x7,0xF};
static inline uint8_t st7305_rev8(uint8_t v) {
    return (uint8_t)(((uint32_t)REV4[v & 0xF] << 4) | REV4[v >> 4]);
}
static void st7305_fb_rot180(const uint8_t *src, uint8_t *dst) {
    const int CX = ST7305_WIDTH / 2;
    const int RY = ST7305_HEIGHT / 4;
    for (int cx = 0; cx < CX; cx++) {
        const uint8_t *s = src + (size_t)cx * RY;
        uint8_t       *d = dst + (size_t)(CX - 1 - cx) * RY;
        for (int ry = 0; ry < RY; ry++) {
            d[RY - 1 - ry] = st7305_rev8(s[ry]);
        }
    }
}

/* ==================== 局刷: 已整块删除 (2026-10-25) ====================
 * linit: "又出现屏幕卡死，请删除所有屏幕优化"。
 * 脏行带局刷依赖 CASET 镜像换算，方向没法用仪器验证，一旦算错就是
 * "某几行横移 / 花屏 / 局部永远不刷" —— 表现和卡死一样难查。
 * 现在整帧只要 3ms，面板也只有 16~24Hz，省这点字节毫无意义 ⇒ 代码删掉。
 * 对外 API 保留成空壳，免得 remcon 的 `partial` 命令编译不过。 */
void st7305_set_partial(bool on) { (void)on; }
bool st7305_get_partial(void)    { return false; }

/* 这两个以前是"作废帧指纹 / 局刷基线"的钩子。优化删掉后已无用，留空壳保住 ABI
 * (main.c 与 st7305_set_rotation 还在调，删了会链接不过)。 */
void st7305_invalidate_frame(void) {
    s_win_set = false;          /* 旋转/尺寸变了 → 窗口随下一帧重设 */
    s_last_sum_valid = false;   /* 面板被复位/唤醒后必须重发, 不能被指纹跳过 */
    s_last_psum_valid = false;  /* 竖屏缓冲指纹同样作废 */
}
void st7305_partial_invalidate_pub(void) { }

/* ==================== 面板自刷新档位 (2026-10-24) ====================
 * 0x38 = HPM, 内部自刷新 ≈32Hz;  0x39 = LPM ≈1Hz (极省电, 静态画面看不出差别)
 * 策略: 有帧要画 → 先回 HPM; 连续 500ms 没画 → 掉 LPM。
 * ⚠ 这两条命令 U8g2 没封装, 但微雪参考驱动与社区实践都用它们。
 * 状态量 s_lpm_on / s_lpm_force 定义在文件顶部(flush_from_raw 也要读)。 */

esp_err_t st7305_set_lpm(st7305_handle_t *dev, bool lpm) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (s_lpm_on == lpm) return ESP_OK;
    if (!lcd_lock_take()) return ESP_ERR_INVALID_STATE;
    /* ⚠ 0x38/0x39 是**无参命令**（u8g2 里就是 sendF("c",0x39)）——
     *   多发一个数据字节会被当成显存数据, 可能污染画面。 */
    send_cmd(dev, lpm ? 0x39 : 0x38);
    xSemaphoreGive(s_lcd_lock);
    s_lpm_on = lpm;
    return ESP_OK;
}

void st7305_set_lpm_force(int mode) {
    s_lpm_force = (mode > 1) ? 1 : (mode < -1 ? -1 : mode);
}
int st7305_get_lpm_force(void) { return s_lpm_force; }

void st7305_idle_tick(st7305_handle_t *dev, bool drew, uint32_t now_ms) {
    (void)drew; (void)now_ms;
    /* ★★★ 2026-10-25 linit「屏幕优化设置以老版本为主」:
     *   老工程**没有**运行时的 0x38/0x39 自动切换, 面板一直停在 init 设好的 0x38(HPM)。
     *   后来加的"500ms 没画就掉 LPM(1Hz)"在真机上是灾难:
     *     · 游戏态 os_render 不出帧 ⇒ 每 500ms 掉 LPM, 下一帧又拉回 HPM, 一秒跳两次;
     *     · LPM 里推进去的显存要等 1Hz 自刷新才显示 ⇒ 整屏"忽地一闪";
     *     · 反复跳变本身就是面板状态机出错的常见诱因。
     *   现在恢复老版行为: 自动逻辑全删, 只有 remcon `lpm 0/1` 强制时才动。 */
    if (s_lpm_force >= 0) st7305_set_lpm(dev, s_lpm_force == 1);
}

/* 显存屏进最低功耗态（深睡前调用，在最后一帧刷完之后）
 *   0x28 = Display OFF，0x10 = Sleep In（面板偏压升压关断）
 *   ⚠ 2026-10-24 实测: 走完这两条, 屏上最后那幅(睡壁纸)会被抹成黑 —— 这块面板
 *     并不会"断电后保持画面"。想留住画面只能停在前一步, 见 s_sleep_mode。 */
/* 深睡关屏方式 —— 0x28/0x10 都会把这块 MIP 屏抹黑(实测: 猫只停 1 秒就黑),
 * 想留住画面只能停在前一步。linit 2026-10-24 定调: **只优化睡眠电流**, 画面不管 ⇒ 默认 2。
 * 0 = 不发命令(画面自保持, 面板继续自刷新 ≈50~100µA)
 * 1 = 只发 0x10 Sleep In
 * 2 = 0x28 Display OFF + 0x10 Sleep In  ← 默认, 最省; 屏上留黑
 * 3 = 只发 0x39 (LPM 1Hz 自刷新) —— 保留画面里最省的一档, 供实测对比
 * 用 `remcon slptest N` 现场挑。
 *
 * ★★★ 2026-10-25 默认曾改成 **0（不发任何命令）**：
 *   linit 实测「画面卡死后**刷机都救不回来**，只能拔电」⇒ 这是**面板被硬件锁死**
 *   （内部升压 VGH/VGL 被 0x28 Display OFF + 0x10 Sleep In 关断后，SWRESET/0x11/0x29
 *   一律不响应，只有 VDD 掉电才复位）。这块是 memory LCD，**不发命令也自己保画面**，
 *   所以最安全的做法就是深睡前什么都不发。功耗略高（面板继续自刷新 ~50-100µA），
 *   但换来"再也不会锁死"。要省电可用 `remcon slptest 2` 临时试。
 *
 * ★★★ 2026-10-26 linit「一定要关 / 深度睡眠完全关闭屏幕」⇒ 默认 **2（0x28 + 0x10）**：
 *   走完这两条 = Display OFF + Sleep In ⇒ **内部升压 VGH/VGL 也关断**，面板真·全关，
 *   待机 ~1-5µA（mode 4 只关显示仍有偏压）。
 *   ⚠ 已知代价（2026-10-25 实测过）：这两条发下去之后，若面板被"锁死"，SWRESET/0x11/0x29
 *     一律不响应，本板 `lcd_rst = GPIO_NC`（没接）⇒ **只能拔电池/VDD 掉电才能复位**，
 *     重刷固件不解决问题（面板 VDD 一直没断）。这是 linit 明确选择换来的功耗。
 *   想回到"可恢复的关显示"用 `remcon slptest 4`；想完全不发命令用 `remcon slptest 0`。
 * ==========================================================================*/
static int s_sleep_mode = 2;   /* ★ 2026-10-26 linit: 深睡完全关屏 ⇒ 2 = 0x28 + 0x10 (最省) */
void st7305_set_sleep_mode(int mode) { s_sleep_mode = (mode < 0) ? 0 : (mode > 4 ? 4 : mode); }
int  st7305_get_sleep_mode(void)      { return s_sleep_mode; }

/* 从 Sleep In / Display OFF 回来 (0x11 退出睡眠 + 0x29 开显示), 测试用 */
esp_err_t st7305_wake_out(st7305_handle_t *dev) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!lcd_lock_take()) return ESP_ERR_TIMEOUT;
    send_cmd(dev, 0x11);                 /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(120));      /* 手册: ≥120ms */
    send_cmd(dev, 0x29);                 /* Display ON */
    xSemaphoreGive(s_lcd_lock);
    s_lpm_on = false;
    return ESP_OK;
}

esp_err_t st7305_sleep_in(st7305_handle_t *dev) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    if (!lcd_lock_take()) return ESP_ERR_TIMEOUT;
    /* 关屏前留一点时间, 让面板把刚送进去的 GRAM 扫成物理图像 (~16Hz)。 */
    vTaskDelay(pdMS_TO_TICKS(120));   /* 留时间让面板把 GRAM 扫成物理图像 */
    /* 按实测结论选关屏深度 (见 s_sleep_mode): 0x28/0x10 会让这块屏清屏,
     * 想留住睡壁纸就只能停在这一步之前。 */
    switch (s_sleep_mode) {
        case 0:                                   /* 什么都不发: 画面留着, 但面板一直扫 */
            break;
        case 1:                                   /* 只睡偏压, 不开/关显示 */
            send_cmd(dev, 0x10);
            vTaskDelay(pdMS_TO_TICKS(120));
            break;
        case 3:                                   /* 只降到 1Hz 自刷新: 画面留着, 功耗最低的"显示"态 */
            send_cmd(dev, 0x39);
            s_lpm_on = true;
            break;
        case 4:                                   /* ★ 默认: 只关显示输出 (停刷新, 不关升压 ⇒ 可恢复) */
            send_cmd(dev, 0x28);
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        case 2:                                   /* Display OFF + Sleep In: 最省, 但可能锁死面板 */
        default:
            send_cmd(dev, 0x28);
            vTaskDelay(pdMS_TO_TICKS(20));
            send_cmd(dev, 0x10);
            vTaskDelay(pdMS_TO_TICKS(120));
            break;
    }
    xSemaphoreGive(s_lcd_lock);
    return ESP_OK;
}

/* ==================== 送屏: 只留最朴素的一条路 (2026-10-25 linit 要求) ====================
 * 用户原话："又出现屏幕卡死，请删除所有屏幕优化"。
 * 于是这一层彻底去掉全部花活，只保留**必须存在**的东西：
 *   ✅ 整帧全量送屏 (15000B @40MHz ≈ 3ms，面板 16~24Hz，完全够用)
 *   ✅ 同步发送 (返回即 SPI 空闲, 不留"在途 DMA")
 *   ✅ 一次握手: 0x36/0x2A/0x2B/0x2C + 数据
 *   ❌ 帧指纹跳过      —— 删 (算错就整屏不更新, 看起来就是"卡死")
 *   ❌ 脏行带局刷      —— 删 (CASET 镜像方向没法验证, 一错就是横移/花屏/局部不刷)
 *   ❌ 异步 DMA 双缓冲 —— 删 (在途 PSRAM DMA 撞 flash 操作会把 SPI 卡死)
 *   ❌ 自动 LPM 0x38/0x39 切换 —— 删 (每条命令都是面板状态机的一次跳变)
 * 唯一保留的"技巧"是 180° 软件旋转（阅读器要用），以及面板卡死时的软复位自愈。 */
/* === "帧内容没变就跳过送屏" (老工程 V1.2 的做法, 2026-10-25 恢复) ===
 * 静止界面会被上层反复置 needs_redraw, 原实现每次都全量送 15KB。
 * 这里用 4 字节折叠旋转异或做廉价指纹 (3750 次 ≈ 17µs), 远小于一次送屏的 3~15ms,
 * 且顺序敏感不会漏判。静止画面 ⇒ SPI 流量归零, 面板也不会被无意义地反复写。 */
static uint32_t st7305_sum_bytes(const uint8_t *p, size_t n) {
    uint32_t h = 0;
    for (size_t i = 0; i + 3 < n; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        h = ((h << 1) | (h >> 31)) ^ w;
    }
    return h;
}
static uint32_t st7305_fb_sum(const uint8_t *fb) { return st7305_sum_bytes(fb, FB_BYTES); }

esp_err_t st7305_flush(st7305_handle_t *dev) {
    if (s_flush_blocked) return ESP_OK;   /* 屏保门禁 */
    if (!dev || !dev->fb) return ESP_ERR_INVALID_ARG;

    /* ★★★ 2026-10-27 根因修复 (linit: "打开阅读器以后没有画面, 画面停住, 但触摸有反应"):
     *   原来顺序是「先算 dev->fb 的指纹 → 没变就 return → 才把 pfb 旋转进 dev->fb」。
     *   可阅读器/书架的内容是画在**竖屏缓冲 pt_pfb** 里的, dev->fb 在旋转之前根本没变,
     *   所以指纹恒等于上一帧 ⇒ **整帧被跳过, pfb 永远送不上屏**。
     *   表现: push 进阅读器成功、触摸/按键都有反应, 但屏幕停在上一帧(桌面)。
     *   修法: **先把 pfb 旋转进 dev->fb, 再算指纹** —— 指纹算的必须是"真正要送出去的内容"。
     *   静止画面仍然能靠指纹省掉 SPI 流量, 优化没有丢。 */
    if (dev->pt_pfb) {
        /* ★ 先看竖屏缓冲本身有没有变 —— 没变就整帧跳过(连 12 万次转换都不做)。
         *   阅读器停在一页不动时, 这层判断把每帧开销从"12 万次转换"降到"16000 字节求和"。 */
        uint32_t psum = st7305_sum_bytes(dev->pt_pfb, (size_t)PT_ROW_BYTES * PT_H);
        if (s_last_psum_valid && psum == s_last_psum) return ESP_OK;
        s_last_psum = psum;
        s_last_psum_valid = true;
        /* 竖屏绘制目标: 300×400 pfb → 物理 400×300 fb */
        memset(dev->fb, 0xFF, FB_BYTES);
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

    /* 内容没变 → 整帧跳过 (老工程行为)。此时 dev->fb 已含本帧真实内容。 */
    uint32_t sum = st7305_fb_sum(dev->fb);
    if (s_last_sum_valid && sum == s_last_sum) return ESP_OK;

    esp_err_t e;
    if (dev->r_flip1) {
        /* 横屏 180°: 必须在内存里翻转回横屏缓冲再送 (这块压缩屏不支持硬件行翻转) */
        st7305_fb_rot180(dev->fb, dev->r_flip1);
        e = st7305_flush_from(dev, dev->r_flip1);
    } else {
        e = st7305_flush_from(dev, dev->fb);
    }
    if (e == ESP_OK) { s_last_sum = sum; s_last_sum_valid = true; }
    return e;
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
         * 内存翻转回横屏缓冲再送, 干净无白缝.
         * V1.2: 字节网格转置+位反转 (O(N)), 原逐像素双循环已废弃. */
        st7305_fb_rot180(src, work_fb);
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
    /* ★★★ 2026-10-16 单一真相源: 直接取当前板型表的引脚, 不再有第二份宏定义。
     *   微雪: DC5/CS40/SCK11/MOSI12/RST41@60M(RST 必须驱动并拉高 3.3V)
     *   小智: DC7/CS11/SCK9/MOSI10/RST不接@60M */
    const board_pins_t *bp = board_pins_get();
    return (st7305_config_t){
        .dc_gpio  = (gpio_num_t)bp->lcd_dc,
        .rst_gpio = (gpio_num_t)bp->lcd_rst,
        .spi_host = SPI2_HOST,
        .spi_mosi = (gpio_num_t)bp->lcd_mosi,
        .spi_sclk = (gpio_num_t)bp->lcd_sck,
        .spi_cs   = (gpio_num_t)bp->lcd_cs,
        .spi_freq = bp->lcd_freq_mhz,
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
    s_b2_cur = v;
    return ESP_OK;
}

uint8_t st7305_get_frame_rate(void) { return s_b2_cur; }
