// svc_nvs — NVS 落盘统一封装服务
// 全库所有 nvs_flash / nvs_* 读写一律经 svc_nvs_* 调用, 收拢为单一入口。
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "svc_nvs.h"

esp_err_t svc_nvs_flash_init(void)
{
    return nvs_flash_init();
}

esp_err_t svc_nvs_flash_erase(void)
{
    return nvs_flash_erase();
}

esp_err_t svc_nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    return nvs_open(name, open_mode, out_handle);
}

void svc_nvs_close(nvs_handle_t handle)
{
    nvs_close(handle);
}

esp_err_t svc_nvs_commit(nvs_handle_t handle)
{
    return nvs_commit(handle);
}

esp_err_t svc_nvs_erase_key(nvs_handle_t handle, const char *key)
{
    return nvs_erase_key(handle, key);
}

esp_err_t svc_nvs_erase_all(nvs_handle_t handle)
{
    return nvs_erase_all(handle);
}

esp_err_t svc_nvs_get_i8(nvs_handle_t handle, const char *key, int8_t *out)
{
    return nvs_get_i8(handle, key, out);
}

esp_err_t svc_nvs_set_i8(nvs_handle_t handle, const char *key, int8_t value)
{
    return nvs_set_i8(handle, key, value);
}

esp_err_t svc_nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    return nvs_get_u8(handle, key, out);
}

esp_err_t svc_nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    return nvs_set_u8(handle, key, value);
}

esp_err_t svc_nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *out)
{
    return nvs_get_i32(handle, key, out);
}

esp_err_t svc_nvs_set_i32(nvs_handle_t handle, const char *key, int32_t value)
{
    return nvs_set_i32(handle, key, value);
}

esp_err_t svc_nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    return nvs_get_u32(handle, key, out);
}

esp_err_t svc_nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    return nvs_set_u32(handle, key, value);
}

esp_err_t svc_nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *length)
{
    return nvs_get_str(handle, key, out, length);
}

esp_err_t svc_nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    return nvs_set_str(handle, key, value);
}

esp_err_t svc_nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    return nvs_get_blob(handle, key, out, length);
}

esp_err_t svc_nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    return nvs_set_blob(handle, key, value, length);
}
