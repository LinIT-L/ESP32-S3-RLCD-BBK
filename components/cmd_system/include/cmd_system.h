// 本地 stub — 替代 espressif/cmd_system 组件 (避免联网下拉).
// bluepad32 的 arch/uni_console_esp32.c 无条件 #include <cmd_system.h>
// 但实际调用在 CONFIG_BLUEPAD32_USB_CONSOLE_ENABLE 关闭时被编译排除.
// ESP32-S3 不编译 uni_platform_unijoysticle.c (其中调用 cmd_system_version).
// 故此处 header 保底声明, 函数返回 ESP_ERR_NOT_SUPPORTED 即可.

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cmd_system_register(void);
esp_err_t cmd_system_version(void);

#ifdef __cplusplus
}
#endif