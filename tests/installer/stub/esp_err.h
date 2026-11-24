/* 宿主机测试用的 esp_err.h 替身 —— 只提供 am_lpkg.c 用到的类型/常量。
 * 生产编译永远用 IDF 的真头文件（这个目录只在 tests/installer/run_all.sh 的 -I 里出现）。 */
#ifndef TEST_STUB_ESP_ERR_H
#define TEST_STUB_ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106

static inline const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK:                return "ESP_OK";
    case ESP_FAIL:              return "ESP_FAIL";
    case ESP_ERR_NO_MEM:        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:   return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:  return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:     return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    default:                    return "ESP_ERR_?";
    }
}

#endif
