/* esp_log.h 桩 —— 只为在主机上编译生产 lua_sandbox.c 用, 不参与固件构建。 */
#ifndef STUB_ESP_LOG_H
#define STUB_ESP_LOG_H

#define ESP_LOGE(tag, fmt, ...) ((void)(tag), (void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)(tag), (void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)(tag), (void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)(tag), (void)0)

#endif
