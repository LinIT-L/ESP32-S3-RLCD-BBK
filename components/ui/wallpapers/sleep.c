/* ============================================================================
 * 电源模块 — 关机 = ESP-IDF 官方 deep sleep
 *
 * 参考(ESP-IDF v5.5):
 *   examples/system/deep_sleep/main/deep_sleep_example_main.c  (关机流程 + RTC 内存存数据)
 *   examples/system/deep_sleep/main/ext_wakeup.c               (EXT1 唤醒源注册)
 *   docs: api-reference/system/sleep_modes.html
 *
 * 本芯片(ESP32-S3)的深睡 GPIO 唤醒只有 EXT1:
 *   S3 无 SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP → 不能用 esp_deep_sleep_enable_gpio_wakeup(),
 *   只能 esp_sleep_enable_ext1_wakeup_io()。
 *
 * 两条关机路径, 唤醒条件不同(唤醒掩码在关机时按来源设置):
 *   MANUAL 手动(长按关机键 → 弹窗确认): 只认 PWR 键, 且必须**保持按住 ≥0.5s** 才开机。
 *          EXT1 是电平触发(按下即唤醒), 0.5s 保持只能由软件层判定:
 *          唤醒后 wp_power_wake_gate() 检查 PWR 是否仍被按住满 0.5s, 不足 → 立即回深睡。
 *   AUTO   自动(休眠时间到点)        : 认任意键(PWR/KEY/BOOT), 短按即开机, 无需保持。
 *
 * 关机来源用 RTC 内存保存(RTC_NOINIT_ATTR, 掉电才清; 官方示例同样用 RTC 内存跨睡眠存数据)。
 * ==========================================================================*/
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"   /* rtc_gpio_pullup_en: RTC 域上拉(深睡期间数字域掉电, 数字上拉会失效) */
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "sleep.h"
#include "st7305.h"
#include "bt_manager.h"
#include "wifi_manager.h"

#define TAG "PWR"

/* 物理键: PWR(GPIO1) / KEY 左(GPIO18) / BOOT 右(GPIO0), 低电平按下。 */
#define PWR_KEY     GPIO_NUM_1
#define KEY_LEFT   GPIO_NUM_4
#define KEY_RIGHT   GPIO_NUM_0

#define PWR_MASK       (1ULL << PWR_KEY)
/* 唤醒掩码 = PWR(GPIO1) + KEY 左(GPIO18)。**故意不含 GPIO0(BOOT 键)**:
 * 实测(2026-09-19) GPIO0 作为 EXT1 唤醒源时, 深睡瞬间电平保不住(strapping 脚, 板上无外部上拉;
 * 关机前数字读=1, 深睡时变低), ext1_status=0x1 → 关机后立刻自我唤醒,
 * 现象正是"休眠自动关机后 1 秒又回桌面"。GPIO0 在正常按键扫描里仍照常作 BOOT 键使用。 */
#define ANY_KEY_MASK   (PWR_MASK | (1ULL << KEY_LEFT))
#define WAKE_LEVEL     ESP_EXT1_WAKEUP_ANY_LOW

#define MANUAL_HOLD_MS       500    /* 手动关机后, PWR 需保持按住多久才放行开机 */
#define KEY_RELEASE_WAIT_MS  3000   /* 等按键松开的最长时间(防卡死) */

/* 关机来源: 高 24 位 magic(掉电后 RTC 内存乱码 → 校验不通过), 低 8 位存来源。 */
#define ORIGIN_MAGIC   0x57000000u
#define ORIGIN_MASK    0xFFFFFF00u

RTC_NOINIT_ATTR static uint32_t s_off_origin;   /* 跨深睡保留的关机来源 */
static st7305_handle_t *s_lcd = NULL;

/* ---- 唤醒键(GPIO)配置: 普通数字输入 + 上拉 ---- */
static void keys_config(uint64_t mask)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    /* gpio_config 配的是**数字域**上拉: 深睡时数字域掉电 → 上拉失效 → 引脚浮空,
     * EXT1(ANY_LOW) 会把浮空误判成"按下" → 关机瞬间自我唤醒(现象: 关一下又回桌面)。
     * 官方 ext_wakeup.c 在 SOC_RTCIO_INPUT_OUTPUT_SUPPORTED 的芯片上正是补 RTC 域上拉。
     * 注: 只开 RTC 上拉(rtc_gpio_pullup_en)不动 IO_MUX, 数字读键不受影响;
     *     不要在这里调 rtc_gpio_init() —— 那会把 pad 路由到 RTC 功能, 数字读就断了。 */
    if (mask & PWR_MASK)             rtc_gpio_pullup_en(PWR_KEY);
    if (mask & (1ULL << KEY_LEFT))   rtc_gpio_pullup_en(KEY_LEFT);
    if (mask & (1ULL << KEY_RIGHT))  rtc_gpio_pullup_en(KEY_RIGHT);
}

/* ---- 唤醒源设置(官方 ext_wakeup.c 写法): 先清空, 再注册 EXT1 掩码 ---- */
static void wake_src_set(uint64_t mask)
{
    keys_config(mask);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(mask, WAKE_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "EXT1 唤醒源设置失败: %s", esp_err_to_name(err));
    }
    /* EXT1 不在 IDF 的"自动保 RTC_PERIPH 供电"名单里(RTC_EXT0/RTC_GPIO 才自动开,
     * 见 sleep_modes.c), 不显式打开则该域掉电 → RTC 上拉失效 → 引脚浮空被误唤醒。
     * 代价是深睡电流略高, 对按键唤醒设备是必要开销。 */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
}

/* 掩码内是否有键正被按住 */
static bool any_key_down(uint64_t mask)
{
    if ((mask & PWR_MASK)                && gpio_get_level(PWR_KEY)   == 0) return true;
    if ((mask & (1ULL << KEY_LEFT))      && gpio_get_level(KEY_LEFT)  == 0) return true;
    if ((mask & (1ULL << KEY_RIGHT))     && gpio_get_level(KEY_RIGHT) == 0) return true;
    return false;
}

/* 等唤醒键全部松开。EXT1 是电平触发: 键还按着就睡会立刻自醒 → 看起来"关不掉"。
 * 只在深睡**前**调用(此时 flash 缓存还在, 可以正常 vTaskDelay)。 */
static void wait_keys_released(uint64_t mask)
{
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while (any_key_down(mask)) {
        if ((uint32_t)(esp_timer_get_time() / 1000) - t0 > KEY_RELEASE_WAIT_MS) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void wp_power_pin_init(void)
{
    keys_config(ANY_KEY_MASK);
    wake_src_set(PWR_MASK);   /* 默认保守: 只有 PWR 能唤醒 */
}

void wp_power_init(st7305_handle_t *lcd)
{
    s_lcd = lcd;
    keys_config(ANY_KEY_MASK);
    wake_src_set(PWR_MASK);
}

/* ---- 关机(不返回) ----
 * 顺序: 停射频 → 清屏(polling 同步刷出) → 记录来源 → 等键松开 → 设唤醒掩码 → 深睡。
 * 深睡瞬间禁用 flash/PSRAM 缓存, 屏保监督任务(PSRAM 栈刷屏)由调用方先挂起。 */
void wp_power_off(int origin)
{
    if (bt_manager_is_enabled())   bt_manager_disable();
    if (wifi_manager_is_enabled()) wifi_manager_disable();

    if (s_lcd) {
        st7305_clear(s_lcd, ST7305_COLOR_WHITE);
        st7305_flush_override(s_lcd);
    }

    const uint64_t mask = (origin == WP_OFF_AUTO) ? ANY_KEY_MASK : PWR_MASK;
    s_off_origin = ORIGIN_MAGIC | (uint32_t)((unsigned)origin & 0xFFu);

    /* 诊断: 关机前的三键电平(1=松开 0=按下)。某键恒 0 = 浮空/被外部拉低 → 会被 EXT1 误唤醒。 */
    ESP_LOGW(TAG, "关机前键电平: PWR=%d KEY18=%d BOOT0=%d",
             gpio_get_level(PWR_KEY), gpio_get_level(KEY_LEFT), gpio_get_level(KEY_RIGHT));

    wait_keys_released(mask);
    wake_src_set(mask);

    ESP_LOGI(TAG, "关机: 进入深睡 (来源=%s, 唤醒=%s, 掩码=0x%llX)",
             (origin == WP_OFF_AUTO) ? "休眠到点" : "手动",
             (origin == WP_OFF_AUTO) ? "任意键" : "PWR 键按住 0.5s",
             (unsigned long long)mask);
    esp_deep_sleep_start();
    /* 不返回 */
}

/* ---- 开机最早期门控 ----
 * MANUAL 关机唤醒: 要求 PWR 键保持按住 ≥0.5s, 不足(碰一下) → 判为误触, 立即回深睡。
 * AUTO 关机唤醒 / 非关机唤醒(上电等): 直接放行。 */
void wp_power_wake_gate(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return;
    if ((s_off_origin & ORIGIN_MASK) != ORIGIN_MAGIC) return;      /* 非本模块记录 → 放行 */
    if ((s_off_origin & 0xFFu) == (uint32_t)WP_OFF_AUTO) return;   /* 自动关机: 任意键直接开机 */

    keys_config(PWR_MASK);
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < MANUAL_HOLD_MS) {
        if (gpio_get_level(PWR_KEY) != 0) {
            /* 松手了 / 按住不足 0.5s → 误触: 直接回深睡(保持手动语义) */
            wait_keys_released(PWR_MASK);
            s_off_origin = ORIGIN_MAGIC | (uint32_t)WP_OFF_MANUAL;
            wake_src_set(PWR_MASK);
            esp_deep_sleep_start();   /* 不返回 */
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* 按住满 0.5s → 放行开机; 等松手, 避免残余按键事件连发 */
    wait_keys_released(PWR_MASK);
}

bool wp_power_woke_from_off(void)
{
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
}

int wp_power_wake_origin(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return -1;
    if ((s_off_origin & ORIGIN_MASK) != ORIGIN_MAGIC) return -1;
    return (int)(s_off_origin & 0xFFu);
}

void wp_sleep_boot_diag(void)
{
    esp_reset_reason_t r        = esp_reset_reason();
    esp_sleep_wakeup_cause_t w  = esp_sleep_get_wakeup_cause();
    int o = wp_power_wake_origin();
    /* ext1_status: 触发本次唤醒的键掩码(按 GPIO 号; 0 = 非 EXT1 唤醒)。 */
    uint64_t ws = (w == ESP_SLEEP_WAKEUP_EXT1) ? esp_sleep_get_ext1_wakeup_status() : 0;
    ESP_LOGI(TAG, "开机: reset_reason=%d wakeup_cause=%d 关机来源=%s ext1_status=0x%llX (PWR1=%d KEY18=%d BOOT0=%d)",
             (int)r, (int)w,
             (o == WP_OFF_MANUAL) ? "手动" : (o == WP_OFF_AUTO) ? "休眠到点" : "非关机唤醒",
             (unsigned long long)ws,
             (ws & PWR_MASK) ? 1 : 0,
             (ws & (1ULL << KEY_LEFT)) ? 1 : 0,
             (ws & (1ULL << KEY_RIGHT)) ? 1 : 0);
}
