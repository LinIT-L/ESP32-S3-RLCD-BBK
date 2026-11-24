/* os_temp.c — 片内温度传感器统一入口
 *
 * 为什么放在 os 层: main(最小系统/remcon) 和 apps(性能监视器) 都要读温度，
 * 而 apps 不能反向依赖 main。传感器全局只 install 一次，这里惰性初始化。
 *
 * 精度: ESP32-S3 片内传感器 ±5°C 绝对值，但**相对变化**可靠 ——
 *       用来判"某个模块让芯片热了几度"足够，不要当环境温度用。
 */
#include "os.h"
#include "esp_log.h"
#include "driver/temperature_sensor.h"

static const char *TAG = "OS_TEMP";

static temperature_sensor_handle_t s_ts;
static int s_max10 = -1000;

int os_temp_c10(void)
{
    if (!s_ts) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_ts) != ESP_OK ||
            temperature_sensor_enable(s_ts) != ESP_OK) {
            s_ts = NULL;
            return -1000;
        }
        ESP_LOGI(TAG, "片内温度传感器已启用 (±5°C 绝对值, 看相对变化)");
    }
    float c = 0;
    if (temperature_sensor_get_celsius(s_ts, &c) != ESP_OK) return -1000;
    int t10 = (int)(c * 10.0f + (c >= 0 ? 0.5f : -0.5f));
    if (t10 > s_max10) s_max10 = t10;
    return t10;
}

int os_temp_max_c10(void) { return s_max10; }
