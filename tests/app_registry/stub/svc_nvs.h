#ifndef TEST_STUB_SVC_NVS_H
#define TEST_STUB_SVC_NVS_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef int nvs_handle_t;
#define NVS_READONLY  0
#define NVS_READWRITE 1

/* 宿主机上没有 NVS：全部当作"打开失败"。注册表同步本身不依赖 NVS，
 * 只影响"主菜单可见性"那部分（本套件不测它）。 */
static inline esp_err_t svc_nvs_open(const char *ns, int mode, nvs_handle_t *h)
{
    (void)ns; (void)mode;
    if (h) *h = 0;
    return ESP_FAIL;
}
static inline esp_err_t svc_nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v)
{
    (void)h; (void)k; (void)v;
    return ESP_FAIL;
}
static inline esp_err_t svc_nvs_set_i32(nvs_handle_t h, const char *k, int32_t v)
{
    (void)h; (void)k; (void)v;
    return ESP_FAIL;
}
static inline esp_err_t svc_nvs_get_str(nvs_handle_t h, const char *k, char *buf, size_t *len)
{
    (void)h; (void)k; (void)buf; (void)len;
    return ESP_FAIL;
}
static inline esp_err_t svc_nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{
    (void)h; (void)k; (void)v;
    return ESP_FAIL;
}
static inline esp_err_t svc_nvs_erase_key(nvs_handle_t h, const char *k)
{
    (void)h; (void)k;
    return ESP_FAIL;
}
static inline esp_err_t svc_nvs_commit(nvs_handle_t h) { (void)h; return ESP_FAIL; }
static inline void      svc_nvs_close(nvs_handle_t h)  { (void)h; }

#endif
