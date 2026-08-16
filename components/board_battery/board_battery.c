#include "board_battery.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board_battery";

/*
 * 原理图连接：
 *   VBAT -> R21 200K -> BAT_ADC(GPIO4) -> R23 100K -> GND
 *
 * ADC 只能测 BAT_ADC 这个点的电压。因为下面电阻是 100K，上面电阻是 200K，
 * BAT_ADC 电压约等于 VBAT 的 1/3，所以软件里要乘以 3 才是电池电压。
 */
#define BOARD_BATTERY_ADC_UNIT        ADC_UNIT_1
#define BOARD_BATTERY_ADC_CHANNEL     ADC_CHANNEL_3
#define BOARD_BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12
#define BOARD_BATTERY_SAMPLE_COUNT    8
#define BOARD_BATTERY_DIVIDER_NUM     3
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
    ESP_LOGI(TAG, "Battery ADC initialized: GPIO4 ADC1_CH3, divider 200K/100K");
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
