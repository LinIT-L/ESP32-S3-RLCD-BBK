/**
 * @file vibrator.c
 * @brief 苹果 Taptic Engine (LRA) 震动马达驱动 (V1.0.76)
 *
 * 驱动原理:
 *  - Taptic Engine 是线性谐振马达(LRA), 线圈电阻极低(≤20Ω), 峰值电流近 1A,
 *    无法由 ESP32-S3 GPIO 直接驱动 (GPIO 源电流仅 ~40mA), 必须经逻辑电平
 *    N-MOS 管切换 3.3V/5V 电源.
 *  - LEDC PWM 以线圈谐振频率(约 190Hz)输出方波控制 MOS 通断, 使线圈每半周期
 *    被励磁一次, 驱动内部振子在机械谐振点起振, 得到可感知的震动.
 *  - 单极性驱动手感偏"闷", 非 Taptic 原版双向线性回弹; 若需原版手感需改用
 *    H 桥或 DRV2605 触觉驱动 IC. 此处为无外部驱动板的最简方案.
 *
 * 接线 (仅裸马达):
 *   GPIO2 ---- N-MOS Gate
 *   3V3/5V -- N-MOS Drain
 *   Source -- 马达一端 + GND, 马达另一端 -- GND (即 MoS 切断负载地)
 *   马达线圈两端反向并联 1N5819 续流二极管 (负极接电源端).
 */
#include "vibrator.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdlib.h>

#define TAG "VIB"

#define VIB_GPIO        GPIO_NUM_6   /* 空闲 GPIO (GPIO2 是触摸复位脚, 不可用), 见 main/user_config.h */
#define VIB_TIMER       LEDC_TIMER_1 /* tone_player 已用 TIMER_0/CH_0 */
#define VIB_CHANNEL     LEDC_CHANNEL_1
#define VIB_RES         LEDC_TIMER_8_BIT
#define VIB_RES_MAX     255
#define VIB_RES_FREQ    190          /* Taptic Engine 谐振频率 (约 170~210Hz) */
#define VIB_DEFAULT_DUTY 200         /* 默认强度对应占空比 (~78%), 留裕量防过流 */

static bool s_ready = false;

void vibrator_init(void) {
    if (s_ready) return;
    ledc_timer_config_t tc = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = VIB_TIMER,
        .duty_resolution = VIB_RES,
        .freq_hz         = VIB_RES_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&tc) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC 定时器配置失败");
        return;
    }
    ledc_channel_config_t ch = {
        .gpio_num   = VIB_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = VIB_CHANNEL,
        .timer_sel  = VIB_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    if (ledc_channel_config(&ch) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC 通道配置失败");
        return;
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
    s_ready = true;
    ESP_LOGI(TAG, "震动马达就绪: GPIO%d @ %dHz", VIB_GPIO, VIB_RES_FREQ);
}

bool vibrator_ready(void) { return s_ready; }

void vibrator_set(uint8_t strength) {
    if (!s_ready) return;
    uint32_t duty = (strength > 100) ? 100 : strength;
    duty = (duty * VIB_RES_MAX) / 100;
    if (duty == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL, 0);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL, duty);
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
}

void vibrator_stop(void) {
    if (!s_ready) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
}

void vibrator_buzz(uint16_t ms) {
    if (!s_ready) return;
    if (ms == 0) { vibrator_stop(); return; }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL, VIB_DEFAULT_DUTY);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, VIB_CHANNEL);
}