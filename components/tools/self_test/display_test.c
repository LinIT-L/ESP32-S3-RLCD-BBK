/* 显示刷新自检: 排查整屏刷新率闪烁.
 *
 * 图案:
 *   P0 全黑 / P1 全白 / P2 横条纹(4px) / P3 竖条纹(4px)
 *   P4 棋盘2x2 / P5 棋盘8x8 / P6 稀疏单点(8x8 一个点)
 *   P7 灰度2x2(1黑点) / P8 灰度4x4
 * 刷新率: 10 / 15 / 20 / 30 / 45 / 60 Hz
 * 每档 2 秒, 串口打印 "DISPLAY_TEST pattern=.. rate=..Hz", 用于对照观察.
 */
#include "display_test.h"

#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "st7305.h"

#define TAG "DISP_TEST"

static const int s_rates_hz[] = { 10, 15, 20, 30, 45, 60 };
static const int s_rate_count = (int)(sizeof(s_rates_hz) / sizeof(s_rates_hz[0]));

static void draw_pattern(st7305_handle_t *lcd, int p)
{
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    switch (p) {
    case 0: /* 全黑 */
        st7305_clear(lcd, ST7305_COLOR_BLACK);
        break;
    case 1: /* 全白 */
        break;
    case 2: /* 横条纹 4px */
        for (int y = 0; y < ST7305_HEIGHT; y += 8)
            for (int yy = 0; yy < 4 && y + yy < ST7305_HEIGHT; yy++)
                for (int x = 0; x < ST7305_WIDTH; x++)
                    st7305_draw_pixel(lcd, x, y + yy, ST7305_COLOR_BLACK);
        break;
    case 3: /* 竖条纹 4px */
        for (int x = 0; x < ST7305_WIDTH; x += 8)
            for (int xx = 0; xx < 4 && x + xx < ST7305_WIDTH; xx++)
                for (int y = 0; y < ST7305_HEIGHT; y++)
                    st7305_draw_pixel(lcd, x + xx, y, ST7305_COLOR_BLACK);
        break;
    case 4: /* 棋盘 2x2 */
        for (int y = 0; y < ST7305_HEIGHT; y += 2)
            for (int x = 0; x < ST7305_WIDTH; x += 2)
                if (((x / 2 + y / 2) & 1) == 0)
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++)
                            st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_BLACK);
        break;
    case 5: /* 棋盘 8x8 */
        for (int y = 0; y < ST7305_HEIGHT; y += 8)
            for (int x = 0; x < ST7305_WIDTH; x += 8)
                if (((x / 8 + y / 8) & 1) == 0)
                    for (int dy = 0; dy < 8; dy++)
                        for (int dx = 0; dx < 8; dx++)
                            st7305_draw_pixel(lcd, x + dx, y + dy, ST7305_COLOR_BLACK);
        break;
    case 6: /* 稀疏单点: 每 8x8 一个黑点 */
        for (int y = 0; y < ST7305_HEIGHT; y += 8)
            for (int x = 0; x < ST7305_WIDTH; x += 8)
                st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
        break;
    case 7: /* 灰度2x2: 每 4x4 块左上 2x2 一个黑点 */
        for (int y = 0; y < ST7305_HEIGHT; y += 4)
            for (int x = 0; x < ST7305_WIDTH; x += 4)
                st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
        break;
    case 8: /* 灰度4x4 渐变块 */
        for (int y = 0; y < ST7305_HEIGHT; y += 4)
            for (int x = 0; x < ST7305_WIDTH; x += 4) {
                int d = ((x / 4 + y / 4) & 3);
                if (d == 0) continue;
                for (int k = 0; k < d && k < 4; k++)
                    st7305_draw_pixel(lcd, x + k, y, ST7305_COLOR_BLACK);
            }
        break;
    default:
        break;
    }
}

/* 动态测试: 模拟游戏的内容变化.
 *  M0 灰底(2x2 单点) + 滚动黑条        -> 灰度 + 运动
 *  M1 纯白底 + 滚动黑条                 -> 无灰度 + 运动 (对照)
 *  M2 灰度帧交替 (1黑点 <-> 对角2黑点)  -> 灰度内容每帧切换 (无运动)
 */
static void draw_motion(st7305_handle_t *lcd, int m, int frame)
{
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    if (m == 0) {
        /* 灰底: 每 4x4 左上一个黑点 */
        for (int y = 0; y < ST7305_HEIGHT; y += 4)
            for (int x = 0; x < ST7305_WIDTH; x += 4)
                st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
        /* 20px 黑条滚动 */
        int bx = (frame * 8) % (ST7305_WIDTH + 40) - 20;
        for (int x = bx; x < bx + 20; x++)
            if (x >= 0 && x < ST7305_WIDTH)
                for (int y = 0; y < ST7305_HEIGHT; y++)
                    st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
    } else if (m == 1) {
        int bx = (frame * 8) % (ST7305_WIDTH + 40) - 20;
        for (int x = bx; x < bx + 20; x++)
            if (x >= 0 && x < ST7305_WIDTH)
                for (int y = 0; y < ST7305_HEIGHT; y++)
                    st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
    } else {
        /* 灰度帧交替: 偶帧=单点, 奇帧=对角双点 */
        for (int y = 0; y < ST7305_HEIGHT; y += 4) {
            for (int x = 0; x < ST7305_WIDTH; x += 4) {
                if ((frame & 1) == 0) {
                    st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
                } else {
                    st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
                    st7305_draw_pixel(lcd, x + 2, y + 2, ST7305_COLOR_BLACK);
                }
            }
        }
    }
}

static void run_motion(st7305_handle_t *lcd)
{
    const int rates[] = { 30, 60 };
    for (int m = 0; m < 3; m++) {
        for (int ri = 0; ri < 2; ri++) {
            int hz = rates[ri];
            ESP_LOGI(TAG, "MOTION_TEST motion=%d rate=%dHz", m, hz);
            uint32_t period_ms = 1000 / hz;
            uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
            int frame = 0;
            while ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) < 2500) {
                draw_motion(lcd, m, frame++);
                st7305_flush(lcd);
                vTaskDelay(pdMS_TO_TICKS(period_ms > 13 ? period_ms - 13 : 1));
            }
        }
    }
}

void display_test_run(st7305_handle_t *lcd)
{
    ESP_LOGI(TAG, "==== 显示刷新自检开始 (观察屏幕, 每档 2 秒) ====");
    for (int p = 0; p < 9; p++) {
        draw_pattern(lcd, p);
        for (int r = 0; r < s_rate_count; r++) {
            int hz = s_rates_hz[r];
            ESP_LOGI(TAG, "DISPLAY_TEST pattern=%d rate=%dHz", p, hz);
            uint32_t period_ms = 1000 / hz;
            uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
            while ((xTaskGetTickCount() * portTICK_PERIOD_MS - start) < 2000) {
                st7305_flush(lcd);
                vTaskDelay(pdMS_TO_TICKS(period_ms > 13 ? period_ms - 13 : 1));
            }
        }
    }
    run_motion(lcd);
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    st7305_flush(lcd);
    ESP_LOGI(TAG, "==== 显示刷新自检结束 ====");
}

/* 快速诊断: 只测"密集 2x2 灰度点阵" (与游戏灰阶最接近的图案).
 *  Q0 密集单点静止 / Q1 密集对角双点静止 / Q2 单点<->双点交替
 *  各在 30/60Hz 显示 2 秒. 用于判断: 静止密集灰点是否闪烁(面板串扰). */
void display_test_quick(st7305_handle_t *lcd)
{
    ESP_LOGI(TAG, "==== 密集灰度快速自检 (每档 2 秒) ====");
    const int rates[] = { 30, 60 };
    for (int q = 0; q < 3; q++) {
        for (int ri = 0; ri < 2; ri++) {
            int hz = rates[ri];
            ESP_LOGI(TAG, "QUICK_TEST q=%d rate=%dHz", q, hz);
            uint32_t period_ms = 1000 / hz;
            uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
            int frame = 0;
            while ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) < 2000) {
                st7305_clear(lcd, ST7305_COLOR_WHITE);
                for (int y = 0; y < ST7305_HEIGHT; y += 2) {
                    for (int x = 0; x < ST7305_WIDTH; x += 2) {
                        if (q == 0) {
                            st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
                        } else if (q == 1) {
                            st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
                            st7305_draw_pixel(lcd, x + 1, y + 1, ST7305_COLOR_BLACK);
                        } else {
                            if ((frame & 1) == 0)
                                st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
                            else {
                                st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
                                st7305_draw_pixel(lcd, x + 1, y + 1, ST7305_COLOR_BLACK);
                            }
                        }
                    }
                }
                st7305_flush(lcd);
                frame++;
                vTaskDelay(pdMS_TO_TICKS(period_ms > 13 ? period_ms - 13 : 1));
            }
        }
    }
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    st7305_flush(lcd);
    ESP_LOGI(TAG, "==== 密集灰度快速自检结束 ====");
}

/* 只跑动态测试: M0 灰底滚动条 / M1 白底滚动条 / M2 灰度帧交替 @30/60Hz */
void display_test_motion_only(st7305_handle_t *lcd)
{
    ESP_LOGI(TAG, "==== 动态画面自检开始 (每档 2.5 秒) ====");
    run_motion(lcd);
    st7305_clear(lcd, ST7305_COLOR_WHITE);
    st7305_flush(lcd);
    ESP_LOGI(TAG, "==== 动态画面自检结束 ====");
}
