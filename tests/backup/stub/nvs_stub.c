/* nvs_stub.c — NVS 空桩
 *
 * svc_backup.c 里 bkp_save_nvs / bkp_load_nvs 是导出符号, 链接时必须解析
 * 它们引用的 nvs_*。本测试**不调用**这两个函数 (NVS 备份属真机项),
 * 所以桩只需能链接: 一律返回"未找到"。
 *
 * 这不是在"测假逻辑": 本测试全部断言走的都是 bkp_save/bkp_load/bkp_list/
 * bkp_remove 这条真实路径; NVS 那两个入口在这里只是占位。
 */
#include "nvs.h"

int  nvs_open(const char *ns, int mode, nvs_handle_t *out)
{ (void)ns; (void)mode; if (out) *out = 0; return -1; }
void nvs_close(nvs_handle_t h) { (void)h; }
int  nvs_commit(nvs_handle_t h) { (void)h; return 0; }

int  nvs_entry_find(const char *part, const char *ns, nvs_type_t type, nvs_iterator_t *out)
{ (void)part; (void)ns; (void)type; if (out) *out = 0; return -1; }
int  nvs_entry_next(nvs_iterator_t *it) { (void)it; return -1; }
int  nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *info) { (void)it; (void)info; return -1; }
void nvs_release_iterator(nvs_iterator_t it) { (void)it; }

int nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { (void)h; (void)k; (void)v; return -1; }
int nvs_get_i16(nvs_handle_t h, const char *k, int16_t *v) { (void)h; (void)k; (void)v; return -1; }
int nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v) { (void)h; (void)k; (void)v; return -1; }
int nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v) { (void)h; (void)k; (void)v; return -1; }
int nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v) { (void)h; (void)k; (void)v; return -1; }
int nvs_get_i64(nvs_handle_t h, const char *k, int64_t *v) { (void)h; (void)k; (void)v; return -1; }
int nvs_get_u64(nvs_handle_t h, const char *k, uint64_t *v) { (void)h; (void)k; (void)v; return -1; }
int nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *len) { (void)h; (void)k; (void)v; (void)len; return -1; }
int nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len) { (void)h; (void)k; (void)v; (void)len; return -1; }

int nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { (void)h; (void)k; (void)v; return -1; }
int nvs_set_i16(nvs_handle_t h, const char *k, int16_t v) { (void)h; (void)k; (void)v; return -1; }
int nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v) { (void)h; (void)k; (void)v; return -1; }
int nvs_set_i32(nvs_handle_t h, const char *k, int32_t v) { (void)h; (void)k; (void)v; return -1; }
int nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { (void)h; (void)k; (void)v; return -1; }
int nvs_set_i64(nvs_handle_t h, const char *k, int64_t v) { (void)h; (void)k; (void)v; return -1; }
int nvs_set_u64(nvs_handle_t h, const char *k, uint64_t v) { (void)h; (void)k; (void)v; return -1; }
int nvs_set_str(nvs_handle_t h, const char *k, const char *v) { (void)h; (void)k; (void)v; return -1; }
int nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{ (void)h; (void)k; (void)v; (void)len; return -1; }
