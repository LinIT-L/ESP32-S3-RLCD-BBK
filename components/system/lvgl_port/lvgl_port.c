/**
 * lvgl_port.c — LVGL 9.x → ST7305 (400×300 1bpp 反射屏) 移植层.
 *
 * 设计要点
 * --------
 * ① 只用 partial 模式 + **整行宽**缓冲: 缓冲宽 = 屏宽 400 → 每行 50 字节,
 *    与 st7305_blit_1bit() 的 bytes_per_row = (w+7)/8 完全一致, 不需要处理 stride 差异。
 *    带高 40 行 → 单缓冲 2000 B, 双缓冲共 4 KB, 全部放 PSRAM。
 *
 * ② 刷屏时机: 等 lv_display_flush_is_last() 为真(所有带都画完)才 st7305_flush(),
 *    否则每个带都推一次 SPI, 撕屏且慢。
 *
 * ③ 位极性(唯一需要上机确认的常量): LVGL 单色缓冲里 位=1 表示"亮"。ST7305 是黑白
 *    反射屏, "亮" = 白。驱动提供两个只写"命中像素"的批量接口:
 *      st7305_blit_1bit()       → 位=1 写**黑**
 *      st7305_blit_1bit_white() → 位=1 写**白**
 *    但两者都不碰未命中的像素, 所以一个区域要先整块铺底色再盖上亮色, 共两次 blit:
 *      LP_BIT1_WHITE=1 → 先 blit_1bit(全 0xFF 铺黑) 再 blit_1bit_white(LVGL 缓冲)
 *      LP_BIT1_WHITE=0 → 先 blit_1bit_white(全 0xFF 铺白) 再 blit_1bit(LVGL 缓冲)
 *    若上机发现整屏反色, 把 LP_BIT1_WHITE 取反即可。
 *
 * ④ 内存: LVGL 自身走 libc malloc (sdkconfig 里 LV_USE_CLIB_MALLOC), 屏幕缓冲显式
 *    分配在 PSRAM; 本层不占内部 RAM (除 2KB 静态铺底色缓冲)。
 */
#include <string.h>

#include "lvgl.h"
#include "st7305.h"
#include "input.h"
#include "lvgl_port.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "LVGLP"

#define LP_W        400
#define LP_H        300
#define LP_BAND     40                                  /* 每条带行数 */
#define LP_BUF_BYTES ((LP_W * LP_BAND) / 8)             /* = 2000 B */

/* 位极性: 1 = LVGL 的"亮"对应屏幕白 (默认) */
#define LP_BIT1_WHITE 1

static st7305_handle_t *s_lcd;
static lv_display_t    *s_disp;
static uint8_t         *s_buf_a;      /* PSRAM */
static uint8_t         *s_buf_b;      /* PSRAM */
static uint8_t          s_fill[LP_BUF_BYTES];   /* 全 1 → 用于铺底色 */
static esp_timer_handle_t s_tick;
static volatile bool    s_inited;
static volatile bool    s_paused;
static TaskHandle_t     s_task;

/* ---------------- ② flush: 1bpp 缓冲 → ST7305 ---------------- */
static void lp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int w = lv_area_get_width(area);
    const int h = lv_area_get_height(area);
    const int x = area->x1;
    const int y = area->y1;

#if LP_BIT1_WHITE
    st7305_blit_1bit(s_lcd, x, y, w, h, s_fill);            /* 铺黑底 */
    st7305_blit_1bit_white(s_lcd, x, y, w, h, px_map);      /* 亮像素写白 */
#else
    st7305_blit_1bit_white(s_lcd, x, y, w, h, s_fill);      /* 铺白底 */
    st7305_blit_1bit(s_lcd, x, y, w, h, px_map);            /* 亮像素写黑 */
#endif

    if (lv_display_flush_is_last(disp)) {
        st7305_flush(s_lcd);                                /* 全部带画完才真推 SPI */
    }
    lv_display_flush_ready(disp);
}

/* ---------------- ④ 输入: 触摸 pointer ---------------- */
static void lp_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = 0, y = 0;
    if (input_get_touch_pos(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state   = LV_INDEV_STATE_RELEASED;
    }
}

/* ---------------- ③ 心跳 + 主循环 ---------------- */
static void lp_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

static void lp_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms < 5)  delay_ms = 5;
        if (delay_ms > 30) delay_ms = 30;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* ---------------- 初始化 ---------------- */
esp_err_t lvgl_port_init(st7305_handle_t *lcd)
{
    if (s_inited) return ESP_OK;
    if (!lcd)     return ESP_ERR_INVALID_ARG;
    s_lcd = lcd;

    lv_init();

    s_buf_a = heap_caps_malloc(LP_BUF_BYTES, MALLOC_CAP_SPIRAM);
    s_buf_b = heap_caps_malloc(LP_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_buf_a || !s_buf_b) {
        ESP_LOGE(TAG, "绘制缓冲分配失败 (各 %d B PSRAM)", LP_BUF_BYTES);
        if (s_buf_a) { heap_caps_free(s_buf_a); s_buf_a = NULL; }
        if (s_buf_b) { heap_caps_free(s_buf_b); s_buf_b = NULL; }
        return ESP_ERR_NO_MEM;
    }
    memset(s_fill, 0xFF, sizeof(s_fill));

    s_disp = lv_display_create(LP_W, LP_H);
    if (!s_disp) { ESP_LOGE(TAG, "lv_display_create 失败"); return ESP_FAIL; }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(s_disp, s_buf_a, s_buf_b, LP_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, lp_flush);

    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, lp_touch_read);
    lv_indev_set_display(touch, s_disp);

    const esp_timer_create_args_t ta = { .callback = lp_tick_cb, .name = "lvgl_tick" };
    if (esp_timer_create(&ta, &s_tick) == ESP_OK) {
        esp_timer_start_periodic(s_tick, 2000);
    } else {
        ESP_LOGW(TAG, "esp_timer 创建失败, LVGL 动画/超时将不推进");
    }

    if (xTaskCreatePinnedToCore(lp_task, "lvgl", 6144, NULL, 4, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "LVGL 任务创建失败");
        return ESP_FAIL;
    }
    s_task = NULL;   /* 不需要句柄, 靠 s_paused 控制 */

    s_inited = true;
    ESP_LOGI(TAG, "LVGL %d.%d.%d 就绪: %dx%d %dbpp, 带 %d 行, 缓冲 %u B×2 (PSRAM)",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             LP_W, LP_H, LV_COLOR_DEPTH, LP_BAND, (unsigned)LP_BUF_BYTES);
    return ESP_OK;
}

bool lvgl_port_ready(void) { return s_inited; }

lv_display_t *lvgl_port_display(void) { return s_disp; }

void lvgl_port_pause(bool paused) { s_paused = paused; }
