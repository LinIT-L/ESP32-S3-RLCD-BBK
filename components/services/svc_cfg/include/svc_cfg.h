#ifndef LIN_TOS_SVC_CFG_H
#define LIN_TOS_SVC_CFG_H

// svc_cfg — svc_nvs 便捷薄封装 (按 namespace+key 即开即读写, 带缺省值返回)
// set = 打开 NVS_READWRITE → set → commit → close (失败静默)
// get = 打开 NVS_READONLY → get, 成功返回读到值; 未找到/失败返回 dflt

#include <stdint.h>
#include "svc_nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

void     svc_cfg_set_u32(const char *ns, const char *key, uint32_t v);
uint32_t svc_cfg_get_u32(const char *ns, const char *key, uint32_t dflt);
void     svc_cfg_set_i32(const char *ns, const char *key, int32_t v);
int32_t  svc_cfg_get_i32(const char *ns, const char *key, int32_t dflt);
void     svc_cfg_set_u8(const char *ns, const char *key, uint8_t v);
uint8_t  svc_cfg_get_u8(const char *ns, const char *key, uint8_t dflt);

#ifdef __cplusplus
}
#endif

#endif // LIN_TOS_SVC_CFG_H
