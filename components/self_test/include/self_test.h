#ifndef SELF_TEST_H
#define SELF_TEST_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 运行全部自测场景, 逐条输出 SELF-TEST [PASS|FAIL], 返回 ESP_OK=全部通过 */
esp_err_t self_test_run_all(void);

#ifdef __cplusplus
}
#endif

#endif
