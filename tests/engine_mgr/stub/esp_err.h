/* 测试专用 ESP-IDF 最小桩 —— 只满足 engine_manager.c / engine_manager.h 的编译需求。
 * 不是"重写生产逻辑": 生产 engine_manager.c 原样编译, 只是它依赖的 ESP 头在这里给出最小定义。 */
#pragma once

typedef int esp_err_t;

#define ESP_OK                 0
#define ESP_FAIL               -1
#define ESP_ERR_NO_MEM         0x101
#define ESP_ERR_INVALID_ARG    0x102
#define ESP_ERR_INVALID_STATE  0x103

const char *esp_err_to_name(esp_err_t code);
