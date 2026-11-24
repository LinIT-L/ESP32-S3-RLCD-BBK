/* esp_log.h — 主机测试桩（只为编译生产 wiki_db.c / lz4_dec.c） */
#ifndef STUB_ESP_LOG_H
#define STUB_ESP_LOG_H
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "[E %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "[W %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#define ESP_LOGD(tag, fmt, ...) ((void)(tag))
#endif
