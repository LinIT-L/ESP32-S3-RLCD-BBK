/* host 测试用 shim: esp_err.h —— 只为让 gps_nmea.c 脱离 ESP-IDF 编译.
 * 与真实 esp_err.h 的语义一致 (ESP_OK=0), 不做错误名映射. */
#pragma once
typedef int esp_err_t;
#define ESP_OK                 0
#define ESP_FAIL               (-1)
#define ESP_ERR_INVALID_STATE  0x103
#define ESP_ERR_INVALID_SIZE   0x104
