/* 测试专用 esp_log.h —— 日志默认静默, LINUX_VERBOSE=1 时打到 stderr。 */
#pragma once

#include <stdio.h>
#include <stdbool.h>

extern bool g_test_log_verbose;

#define ESP_LOG_AT(level, tag, fmt, ...) \
    do { if (g_test_log_verbose) \
        fprintf(stderr, "[" level "] %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#define ESP_LOGE(tag, fmt, ...) ESP_LOG_AT("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ESP_LOG_AT("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOG_AT("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ESP_LOG_AT("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ESP_LOG_AT("V", tag, fmt, ##__VA_ARGS__)
