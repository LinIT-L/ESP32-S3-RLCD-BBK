/* 测试专用 nvs.h —— 只给 svc_backup.c 的 bkp_save_nvs/bkp_load_nvs 提供类型。
 * 本测试不调用那两个函数 (dead_strip 会丢掉它们), 因此这里不必实现。 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint32_t nvs_handle_t;
typedef struct nvs_opaque_iterator_t *nvs_iterator_t;

typedef enum {
    NVS_TYPE_U8 = 0x01, NVS_TYPE_I8 = 0x11,
    NVS_TYPE_U16 = 0x02, NVS_TYPE_I16 = 0x12,
    NVS_TYPE_U32 = 0x04, NVS_TYPE_I32 = 0x14,
    NVS_TYPE_U64 = 0x08, NVS_TYPE_I64 = 0x18,
    NVS_TYPE_STR = 0x21, NVS_TYPE_BLOB = 0x42,
    NVS_TYPE_ANY = 0xff,
} nvs_type_t;

typedef struct {
    char namespace_name[16];
    char key[16];
    nvs_type_t type;
} nvs_entry_info_t;

#define NVS_READONLY  0
#define NVS_READWRITE 1
#define NVS_NOT_FOUND 0x1102

int  nvs_open(const char *ns, int mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
int  nvs_commit(nvs_handle_t h);
int  nvs_entry_find(const char *part, const char *ns, nvs_type_t type, nvs_iterator_t *out);
int  nvs_entry_next(nvs_iterator_t *it);
int  nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *info);
void nvs_release_iterator(nvs_iterator_t it);
int  nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v);
int  nvs_get_i16(nvs_handle_t h, const char *k, int16_t *v);
int  nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v);
int  nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v);
int  nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v);
int  nvs_get_i64(nvs_handle_t h, const char *k, int64_t *v);
int  nvs_get_u64(nvs_handle_t h, const char *k, uint64_t *v);
int  nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *len);
int  nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len);
int  nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v);
int  nvs_set_i16(nvs_handle_t h, const char *k, int16_t v);
int  nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v);
int  nvs_set_i32(nvs_handle_t h, const char *k, int32_t v);
int  nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v);
int  nvs_set_i64(nvs_handle_t h, const char *k, int64_t v);
int  nvs_set_u64(nvs_handle_t h, const char *k, uint64_t v);
int  nvs_set_str(nvs_handle_t h, const char *k, const char *v);
int  nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len);
