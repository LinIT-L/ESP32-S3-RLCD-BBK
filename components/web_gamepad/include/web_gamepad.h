#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 WiFi 热点 + 网页模拟手柄 (AP: ESP32-BBK / 12345678, 浏览器打开 http://192.168.4.1) */
esp_err_t web_gamepad_start(void);

/* 停止网页手柄 (关闭 HTTP 服务器 + WiFi 射频) */
void web_gamepad_stop(void);

bool web_gamepad_is_running(void);

/* 网页手柄 joypad 掩码 (低电平有效, 0xFF=无按键), 与 GB joypad 一致:
 * bit0=A bit1=B bit2=Select bit3=Start bit4=右 bit5=左 bit6=上 bit7=下 */
uint8_t web_gamepad_get_joypad_state(void);

#ifdef __cplusplus
}
#endif
