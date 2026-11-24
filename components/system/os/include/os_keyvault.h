/**
 * os_keyvault.h — 系统密钥存储 API (包装 keyvault 加密存储模块).
 *
 * keyvault 是敏感密码存储模块. 按本 OS 的收敛范式, 应用不应直接
 * include keyvault.h, 统一经本系统 API 访问. 本文件在 include 层面透传
 * keyvault.h 的宏与类型 (KEYVAULT_KEY_LEN / KEYVAULT_MAX_*), 仅把函数名
 * 收敛为 os_keyvault_*, 与 os_hid / os_net / os_notify 一致. 页面只调 os_keyvault_*.
 *
 * 此收敛对"可放 TF 卡按需加载的模块化应用"尤为关键: 加载的外部模块
 * 只链接 os_* 系统 API, 不直接依赖 keyvault 内部结构, 便于 elf_loader 隔离.
 */
#ifndef OS_KEYVAULT_H
#define OS_KEYVAULT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "keyvault.h"   /* KEYVAULT_KEY_LEN / KEYVAULT_MAX_* 宏与类型 (透传) */

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化: 生成/加载 salt, 读失败计数, 初始化默认分类. 应用进入时调用一次. */
esp_err_t    os_keyvault_init(void);
/* 是否已设置 PIN */
bool         os_keyvault_has_pin(void);
/* ★ 空 PIN 等同「无密码」(含未设置) —— 门禁/网页直接放行 */
bool         os_keyvault_pin_is_empty(void);
/* 首次设置 PIN (>=4 位). 0=成功, -2=太短 */
int          os_keyvault_set_pin(const char *pin);
/* 验证 PIN: 0=正确, 1=错误, 2=锁定中, <0=未初始化 */
int          os_keyvault_verify_pin(const char *pin);
/* 统一 PIN 失败提示文案: 传 os_keyvault_verify_pin 的返回值, 写入 buf.
 * 1 → "PIN 错误，还可试 N 次"  2 → "已锁定，N 秒后重试"  其余 → "PIN 错误" */
const char  *os_keyvault_pin_msg(int r, char *buf, size_t n);
/* 距下次可重试的秒数 (0=可立即试) */
uint32_t     os_keyvault_lock_remaining_sec(void);
uint32_t     os_keyvault_fail_count(void);
/* 验证通过后, 用 PIN 派生解密钥 (UI 每次输入/增删前调用并保存)。
 * 返回 ESP_OK 才可用 key; 失败时 key 已清零。 */
esp_err_t    os_keyvault_unlock_secret(const char *pin, uint8_t key[KEYVAULT_KEY_LEN]);

/* 通用加密 (AES-256-GCM): out 容量 ≥ n+28; 解密返回明文长度, 0=失败/被篡改 */
size_t       os_keyvault_blob_encrypt(const uint8_t key[KEYVAULT_KEY_LEN], const uint8_t *in, size_t n, uint8_t *out);
size_t       os_keyvault_blob_decrypt(const uint8_t key[KEYVAULT_KEY_LEN], const uint8_t *in, size_t n, uint8_t *out);

/* ---- 分类管理 ---- */
int          os_keyvault_cat_count(void);              /* 当前分类数 (含内置) */
const char  *os_keyvault_cat_name(int i);              /* 分类名, i 越界返回 NULL */
int          os_keyvault_cat_add(const char *name);    /* 0=ok, -1=满, -2=重名 */
int          os_keyvault_cat_rename(int i, const char *name); /* 0=ok, -1=越界, -2=重名 */
int          os_keyvault_cat_remove(int i);            /* 0=ok, -1=越界, -2=仍有密钥, -3=内置不可删 */

/* ---- 密钥条目 ---- */
int          os_keyvault_add(const uint8_t key[KEYVAULT_KEY_LEN], int cat, const char *group, const char *account, const char *secret);
int          os_keyvault_count(void);
int          os_keyvault_list(char names[][64], int cats[], int n_cap);  /* 返回条目数, 每项分类填到 cats[i] */
int          os_keyvault_get_account(const uint8_t key[KEYVAULT_KEY_LEN], int idx, char *out, size_t out_cap);
int          os_keyvault_get_secret(const uint8_t key[KEYVAULT_KEY_LEN], int idx, char *out, size_t out_cap);
int          os_keyvault_remove(const uint8_t key[KEYVAULT_KEY_LEN], int idx);

/* 重置: 删除密钥文件、PIN 与全部分类 */
void         os_keyvault_reset(void);

/* 备份/恢复 (导出含 salt+pinh+分类+记录, 跨设备用同 PIN 可恢复). 0=成功, -1=失败. */
int          os_keyvault_export(const char *path);
int          os_keyvault_import(const char *path);

#ifdef __cplusplus
}
#endif
#endif /* OS_KEYVAULT_H */
