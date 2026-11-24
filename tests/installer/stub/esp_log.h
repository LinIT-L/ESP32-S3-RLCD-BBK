/* 宿主机测试用的 esp_log.h 替身：默认静默（测试只看断言，不想被日志淹没）。
 * 需要看安装器日志时: CCFLAGS=-DTEST_LOG_VERBOSE=1 sh tests/installer/run_all.sh */
#ifndef TEST_STUB_ESP_LOG_H
#define TEST_STUB_ESP_LOG_H

#include <stdio.h>

#ifdef TEST_LOG_VERBOSE
#define TEST_LOG(level, tag, fmt, ...)  printf("  [" level " %s] " fmt "\n", tag, ##__VA_ARGS__)
#else
#define TEST_LOG(level, tag, fmt, ...)  do { (void)(tag); } while (0)
#endif

#define ESP_LOGE(tag, fmt, ...) TEST_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) TEST_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) TEST_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) TEST_LOG("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) TEST_LOG("V", tag, fmt, ##__VA_ARGS__)

#endif
