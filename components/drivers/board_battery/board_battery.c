#include "board_battery.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board_cfg.h"   /* ★ 引脚唯一真相源 */

static const char *TAG = "board_battery";

/*
 * 原理图连接（自制板 2M/1M ÷3）：
 *   VBAT -> R1 2M -> BAT_ADC(GPIO4) -> R2 1M -> GND
 *
 * ADC 测 BAT_ADC 该点电压，等于 VBAT 的 1/3，软件乘以 3 得电池电压。
 * ⚠️ 官方板（微雪 RLCD-4.2）亦为 200K/100K ÷3，系数一致。
 *
 * ★★ 2026-10-05 小智模块板（分支 board/xiaozhi-s3-core）—— **脚位改成 GPIO3**：
 *   `ESP32-S3-AI-2E` 模块**内部**自带电池分压并接到 **GPIO3**（该脚**没有引出到焊盘**，
 *   所以规格书上看不到 —— 焊盘 15 那个位置被 `VOP`(功放输出正) 占了，但**芯片内部照常可用**）。
 *   好处：**外部不用再画分压**，直接从模块读。
 *   ⚠ GPIO3 = **`ADC1_CH2`**（ESP32-S3 的 ADC1 覆盖 GPIO1..10）。选 ADC1 是对的 ——
 *     **只有 ADC2 会和 WiFi 抢**，ADC1 在 WiFi 跑着时也能读。旧脚 GPIO4 是 ADC1_CH3，同理。
 *   ⇒ 顺带 **GPIO4 空出来了**（旧板仍用它，别动 main）。
 *   ❓ **待校准**：模块内部分压比未知。现暂按 ÷3（= 旧板系数）。上机后拿万用表量一次真实
 *     VBAT，和固件打印的 mv 对比一下：若固件读数 ≈ 真实值×(3/k)，就把下面的 NUM/DEN 改成 k。
 */
#define BOARD_BATTERY_ADC_UNIT        ADC_UNIT_1
#define BOARD_BATTERY_ADC_CHANNEL     CFG_BAT_ADC_CHAN
#define BOARD_BATTERY_ADC_CH          2               /* 同上, 纯为日志打印 */
#define BOARD_BATTERY_ADC_GPIO_NAME   "GPIO3"         /* 同上, 纯为日志打印 */
#define BOARD_BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12
#define BOARD_BATTERY_SAMPLE_COUNT    8
#define BOARD_BATTERY_DIVIDER_NUM     3      /* ❓暂按 ÷3，待用万用表校准 */
#define BOARD_BATTERY_DIVIDER_DEN     1

/*
 * 单节锂电池的粗略电量映射。
 * 这个不是“电量计”级别的精确算法，只适合给用户一个大概剩余量。
 */
#define BOARD_BATTERY_FULL_MV         4200
#define BOARD_BATTERY_EMPTY_MV        3300

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_calibrated = false;
static bool s_initialized = false;

static uint8_t board_battery_voltage_to_percent(uint32_t mv)
{
    if (mv >= BOARD_BATTERY_FULL_MV) {
        return 100;
    }
    if (mv <= BOARD_BATTERY_EMPTY_MV) {
        return 0;
    }

    return (uint8_t)(((mv - BOARD_BATTERY_EMPTY_MV) * 100U) /
                     (BOARD_BATTERY_FULL_MV - BOARD_BATTERY_EMPTY_MV));
}

static void board_battery_init_calibration(void)
{
    esp_err_t ret = ESP_FAIL;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_config = {
        .unit_id = BOARD_BATTERY_ADC_UNIT,
        .chan = BOARD_BATTERY_ADC_CHANNEL,
        .atten = BOARD_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&curve_config, &s_cali_handle);
    if (ret == ESP_OK) {
        s_calibrated = true;
        ESP_LOGI(TAG, "ADC calibration: curve fitting");
        return;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line_config = {
        .unit_id = BOARD_BATTERY_ADC_UNIT,
        .atten = BOARD_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&line_config, &s_cali_handle);
    if (ret == ESP_OK) {
        s_calibrated = true;
        ESP_LOGI(TAG, "ADC calibration: line fitting");
        return;
    }
#endif

    ESP_LOGW(TAG, "ADC calibration unavailable, battery voltage will use raw estimate");
}

esp_err_t board_battery_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = BOARD_BATTERY_ADC_UNIT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &s_adc_handle), TAG, "create adc unit failed");

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = BOARD_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_handle, BOARD_BATTERY_ADC_CHANNEL, &channel_config),
                        TAG,
                        "config battery adc channel failed");

    board_battery_init_calibration();

    s_initialized = true;
    ESP_LOGI(TAG, "Battery ADC initialized: %s ADC1_CH%d, divider x%d/%d",
             BOARD_BATTERY_ADC_GPIO_NAME, BOARD_BATTERY_ADC_CH, 
             BOARD_BATTERY_DIVIDER_NUM, BOARD_BATTERY_DIVIDER_DEN);

    /* ★ 2026-10-05 一次性自检打印 —— 用来**校准分压比**。
     * 小智模块的电池分压做在**模块内部**(接 GPIO3)，阻值未知；拿万用表量一下真实 VBAT，
     * 和下面 "脚上" 那行对比即可算出真实分压比:
     *     真实分压比 k = 真实 VBAT(mV) / 脚上电压(mV)
     * 然后把上面的 DIVIDER_NUM/DEN 改成 k 即可 (例: k≈2 → NUM=2, DEN=1)。 */
    {
        int raw0 = 0;
        if (adc_oneshot_read(s_adc_handle, BOARD_BATTERY_ADC_CHANNEL, &raw0) == ESP_OK) {
            int pin_mv = -1;
            if (s_calibrated && s_cali_handle) {
                adc_cali_raw_to_voltage(s_cali_handle, raw0, &pin_mv);
            }
            ESP_LOGI(TAG, "电池自检: raw=%d 脚上=%d mV  ⇒ 按现系数(%d/%d)算出 VBAT≈%d mV"
                          "   [拿万用表量真实 VBAT 对比即可校准]",
                     raw0, pin_mv,
                     BOARD_BATTERY_DIVIDER_NUM, BOARD_BATTERY_DIVIDER_DEN,
                     pin_mv > 0 ? pin_mv * BOARD_BATTERY_DIVIDER_NUM / BOARD_BATTERY_DIVIDER_DEN : -1);
        }
    }
    return ESP_OK;
}

esp_err_t board_battery_read(board_battery_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is null");
    ESP_RETURN_ON_ERROR(board_battery_init(), TAG, "init battery adc failed");

    int raw_sum = 0;
    for (int i = 0; i < BOARD_BATTERY_SAMPLE_COUNT; i++) {
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_handle, BOARD_BATTERY_ADC_CHANNEL, &raw),
                            TAG,
                            "read battery adc failed");
        raw_sum += raw;
    }

    int raw_avg = raw_sum / BOARD_BATTERY_SAMPLE_COUNT;
    int adc_mv = 0;

    if (s_calibrated) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &adc_mv),
                            TAG,
                            "convert battery adc voltage failed");
    } else {
        adc_mv = (raw_avg * 3300) / 4095;
    }

    uint32_t battery_mv = ((uint32_t)adc_mv * BOARD_BATTERY_DIVIDER_NUM) / BOARD_BATTERY_DIVIDER_DEN;
    status->voltage_mv = battery_mv;
    status->percent = board_battery_voltage_to_percent(battery_mv);

    return ESP_OK;
}

/* ---- 统一 60s 缓存采样 (linit 2026-09-20: 全系统只打这一个节拍器) ----
 * 之前状态栏(60s)和低电量告警(30s)各自持定时器重复打 ADC, 现合并:
 * 所有消费方都走 board_battery_read_cached(), ADC 至多 60s 采样一次. */
#define BOARD_BATTERY_CACHE_MS 60000
static board_battery_status_t s_cache;
static bool     s_cache_valid = false;
static uint32_t s_cache_ms    = 0;

esp_err_t board_battery_read_cached(board_battery_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_cache_valid || (uint32_t)(now - s_cache_ms) >= BOARD_BATTERY_CACHE_MS) {
        esp_err_t err = board_battery_read(&s_cache);
        if (err != ESP_OK) return err;   /* 采样失败不污染旧缓存 */
        s_cache_ms = now;
        s_cache_valid = true;
    }
    *status = s_cache;
    return ESP_OK;
}
