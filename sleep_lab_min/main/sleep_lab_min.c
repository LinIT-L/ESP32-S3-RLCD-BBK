/* ============================================================================
 * sleep_lab_min — L0 裸机浅睡最小测试
 *
 * ⚠ 上一轮的教训: 一旦"真的睡进去", USB-Serial-JTAG 断电, 醒后任何 ESP_LOG 都会把
 *    系统搞挂(abort/静默) —— 所以本版**测试过程中全程不打日志**, 结果只写 RTC 内存,
 *    跑完全部变体后 esp_restart(), 在**全新的这次开机**里把结果打印出来。
 *
 * 变体: V1 = EXT1 + TIMER ; V2 = 仅 TIMER
 * ==========================================================================*/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "SLAB";

#define WAKE_GPIO_MASK  ((1ULL << 0) | (1ULL << 1) | (1ULL << 18))
#define SLEEP_TIMER_US  (2ULL * 1000ULL * 1000ULL)
#define CYCLES          3
#define NVARIANT        2

RTC_DATA_ATTR static uint32_t s_boot   = 0;
RTC_DATA_ATTR static int      s_phase  = 0;      /* 0=跑测试 1=结果待打印 */
RTC_DATA_ATTR static int      s_var    = 0;
RTC_DATA_ATTR static int      s_cycle  = 0;
RTC_DATA_ATTR static uint32_t s_slept[NVARIANT][CYCLES];
RTC_DATA_ATTR static int      s_cause[NVARIANT][CYCLES];

static const char *cause_str(int c)
{
    switch (c) {
        case ESP_SLEEP_WAKEUP_EXT1:      return "EXT1";
        case ESP_SLEEP_WAKEUP_TIMER:     return "TIMER";
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "UNDEF(没睡)";
        default:                          return "OTHER";
    }
}

static void wake_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = WAKE_GPIO_MASK,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

void app_main(void)
{
    s_boot++;
    ESP_LOGW(TAG, "==== BOOT #%u reset_reason=%d phase=%d ====",
             (unsigned)s_boot, (int)esp_reset_reason(), s_phase);

    /* 上一轮结果已就绪 -> 打印(此时是全新开机, USB 健康) */
    if (s_phase == 1) {
        ESP_LOGW(TAG, "============== 上一轮结果 ==============");
        for (int v = 0; v < NVARIANT; v++) {
            for (int i = 0; i < CYCLES; i++) {
                ESP_LOGW(TAG, "V%d #%d slept_ms=%u cause=%s",
                         v + 1, i, (unsigned)s_slept[v][i], cause_str(s_cause[v][i]));
            }
        }
        ESP_LOGW(TAG, "判读: slept≈2000&TIMER=真睡着; slept≈1&UNDEF=没睡进去(被拒绝)");
        ESP_LOGW(TAG, "========================================");
        s_phase = 0; s_var = 0; s_cycle = 0;
    }

    wake_gpio_init();
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* ==== 以下全程不打日志 ==== */
    for (; s_var < NVARIANT; s_var++) {
        for (; s_cycle < CYCLES; s_cycle++) {
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
            if (s_var == 0) esp_sleep_enable_ext1_wakeup_io(WAKE_GPIO_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
            esp_sleep_enable_timer_wakeup(SLEEP_TIMER_US);

            int64_t t0 = esp_timer_get_time();
            esp_light_sleep_start();
            int64_t t1 = esp_timer_get_time();

            s_slept[s_var][s_cycle] = (uint32_t)((t1 - t0) / 1000);
            s_cause[s_var][s_cycle] = (int)esp_sleep_get_wakeup_cause();
        }
        s_cycle = 0;
    }
    s_phase = 1;          /* 标记结果就绪 */
    esp_restart();        /* 重启后在新的一次开机里打印 */
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
