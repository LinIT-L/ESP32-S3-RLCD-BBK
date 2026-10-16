/**
 * os_keyvault.c — 系统密钥存储 API 实现 (包装 keyvault).
 *
 * 全部函数直通 keyvault_*, 是密钥存储能力的统一收敛点. 未来若有生命周期/
 * 权限需求 (如进入应用前强制校验 PIN, 或对外加载模块做访问审计), 可在此
 * 统一拦截, 不让页面感知 keyvault 内部实现, 也不用改 11 个程序里的调用点.
 */
#include "os_keyvault.h"
#include "keyvault.h"

esp_err_t    os_keyvault_init(void)                         { return keyvault_init(); }
bool         os_keyvault_has_pin(void)                      { return keyvault_has_pin(); }
int          os_keyvault_set_pin(const char *pin)           { return keyvault_set_pin(pin); }
int          os_keyvault_verify_pin(const char *pin)        { return keyvault_verify_pin(pin); }
uint32_t     os_keyvault_lock_remaining_sec(void)           { return keyvault_lock_remaining_sec(); }
uint32_t     os_keyvault_fail_count(void)                   { return keyvault_fail_count(); }
void         os_keyvault_unlock_secret(const char *pin, uint8_t key[KEYVAULT_KEY_LEN]) { keyvault_unlock_secret(pin, key); }

int          os_keyvault_cat_count(void)                    { return keyvault_cat_count(); }
const char  *os_keyvault_cat_name(int i)                   { return keyvault_cat_name(i); }
int          os_keyvault_cat_add(const char *name)          { return keyvault_cat_add(name); }
int          os_keyvault_cat_rename(int i, const char *name){ return keyvault_cat_rename(i, name); }
int          os_keyvault_cat_remove(int i)                  { return keyvault_cat_remove(i); }

int          os_keyvault_add(const uint8_t key[KEYVAULT_KEY_LEN], int cat, const char *group, const char *account, const char *secret) {
    return keyvault_add(key, cat, group, account, secret);
}
int          os_keyvault_count(void)                        { return keyvault_count(); }
int          os_keyvault_list(char names[][64], int cats[], int n_cap) { return keyvault_list(names, cats, n_cap); }
int          os_keyvault_get_account(const uint8_t key[KEYVAULT_KEY_LEN], int idx, char *out, size_t out_cap) {
    return keyvault_get_account(key, idx, out, out_cap);
}
int          os_keyvault_get_secret(const uint8_t key[KEYVAULT_KEY_LEN], int idx, char *out, size_t out_cap) {
    return keyvault_get_secret(key, idx, out, out_cap);
}
int          os_keyvault_remove(const uint8_t key[KEYVAULT_KEY_LEN], int idx) { return keyvault_remove(key, idx); }

void         os_keyvault_reset(void)                       { keyvault_reset(); }
int          os_keyvault_export(const char *path)          { return keyvault_export(path); }
int          os_keyvault_import(const char *path)          { return keyvault_import(path); }
