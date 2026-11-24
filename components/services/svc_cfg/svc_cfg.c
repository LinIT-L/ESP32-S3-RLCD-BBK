// svc_cfg — svc_nvs 便捷薄封装 (按 namespace+key 即开即读写, 带缺省值)
#include "svc_nvs.h"
#include "svc_cfg.h"

void svc_cfg_set_u32(const char *ns, const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (svc_nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_set_u32(h, key, v);
    svc_nvs_commit(h);
    svc_nvs_close(h);
}

uint32_t svc_cfg_get_u32(const char *ns, const char *key, uint32_t dflt)
{
    uint32_t v;
    nvs_handle_t h;
    if (svc_nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return dflt;
    if (svc_nvs_get_u32(h, key, &v) != ESP_OK) { svc_nvs_close(h); return dflt; }
    svc_nvs_close(h);
    return v;
}

void svc_cfg_set_i32(const char *ns, const char *key, int32_t v)
{
    nvs_handle_t h;
    if (svc_nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_set_i32(h, key, v);
    svc_nvs_commit(h);
    svc_nvs_close(h);
}

int32_t svc_cfg_get_i32(const char *ns, const char *key, int32_t dflt)
{
    int32_t v;
    nvs_handle_t h;
    if (svc_nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return dflt;
    if (svc_nvs_get_i32(h, key, &v) != ESP_OK) { svc_nvs_close(h); return dflt; }
    svc_nvs_close(h);
    return v;
}

void svc_cfg_set_u8(const char *ns, const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (svc_nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_set_u8(h, key, v);
    svc_nvs_commit(h);
    svc_nvs_close(h);
}

uint8_t svc_cfg_get_u8(const char *ns, const char *key, uint8_t dflt)
{
    uint8_t v;
    nvs_handle_t h;
    if (svc_nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return dflt;
    if (svc_nvs_get_u8(h, key, &v) != ESP_OK) { svc_nvs_close(h); return dflt; }
    svc_nvs_close(h);
    return v;
}
