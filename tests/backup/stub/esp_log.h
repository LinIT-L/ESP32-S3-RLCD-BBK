/* 测试专用 ESP 桩 —— svc_backup.c 的编译依赖 (非生产逻辑改写) */
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
