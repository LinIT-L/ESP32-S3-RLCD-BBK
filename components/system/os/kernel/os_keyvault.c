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
bool         os_keyvault_pin_is_empty(void)                 { return keyvault_pin_is_empty(); }
int          os_keyvault_set_pin(const char *pin)           { return keyvault_set_pin(pin); }
int          os_keyvault_verify_pin(const char *pin)        { return keyvault_verify_pin(pin); }
uint32_t     os_keyvault_lock_remaining_sec(void)           { return keyvault_lock_remaining_sec(); }
uint32_t     os_keyvault_fail_count(void)                   { return keyvault_fail_count(); }
esp_err_t    os_keyvault_unlock_secret(const char *pin, uint8_t key[KEYVAULT_KEY_LEN]) { return keyvault_unlock_secret(pin, key); }

/* ★★ 2026-10-18: **统一 PIN 失败文案** —— 所有需要 PIN 的页面都调这一个,
 *   保证"错了还剩几次 / 锁到什么时候"说法一致, 不再各自写死一句"PIN 错误"。 */
const char *os_keyvault_pin_msg(int r, char *buf, size_t n) {
    if (!buf || n == 0) return "";
    if (r == 2) {
        uint32_t s = keyvault_lock_remaining_sec();
        snprintf(buf, n, "\xe5\xb7\xb2\xe9\x94\x81\xe5\xae\x9a\xef\xbc\x8c%u \xe7\xa7\x92\xe5\x90\x8e\xe9\x87\x8d\xe8\xaf\x95", (unsigned)s); /* 已锁定，N 秒后重试 */
    } else if (r == 1) {
        uint32_t f = keyvault_fail_count();
        uint32_t left = f >= 3 ? 0 : (3 - f);
        if (left) snprintf(buf, n, "PIN \xe9\x94\x99\xe8\xaf\xaf\xef\xbc\x8c\xe8\xbf\x98\xe5\x8f\xaf\xe8\xaf\x95 %u \xe6\xac\xa1", (unsigned)left); /* PIN 错误，还可试 N 次 */
        else      snprintf(buf, n, "PIN \xe9\x94\x99\xe8\xaf\xaf\xef\xbc\x8c\xe5\x86\x8d\xe9\x94\x99\xe5\xb0\xb1\xe9\x94\x81\xe5\xae\x9a"); /* PIN 错误，再错就锁定 */
    } else {
        snprintf(buf, n, "PIN \xe9\x94\x99\xe8\xaf\xaf");
    }
    return buf;
}

size_t os_keyvault_blob_encrypt(const uint8_t key[KEYVAULT_KEY_LEN], const uint8_t *in, size_t n, uint8_t *out) { return keyvault_blob_encrypt(key, in, n, out); }
size_t os_keyvault_blob_decrypt(const uint8_t key[KEYVAULT_KEY_LEN], const uint8_t *in, size_t n, uint8_t *out) { return keyvault_blob_decrypt(key, in, n, out); }

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
