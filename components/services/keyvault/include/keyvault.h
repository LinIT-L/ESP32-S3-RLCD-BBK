/**
 * keyvault.h — 密钥管理器 加密存储模块公开接口.
 */
#ifndef KEYVAULT_H
#define KEYVAULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KEYVAULT_KEY_LEN 16
#define KEYVAULT_MAX_NAME 60
#define KEYVAULT_MAX_SECRET 128
#define KEYVAULT_MAX_ENTRIES 64
#define KEYVAULT_MAX_CATS  8      /* 分类上限 (含内置) */
#define KEYVAULT_DEF_CATS  3      /* 内置分类数: 网站/程序/游戏 */

/* 初始化 (生成/加载 salt, 读失败计数, 初始化默认分类). 应用进入时调用一次. */
esp_err_t keyvault_init(void);

/* 是否已设置 PIN */
bool keyvault_has_pin(void);

/* 首次设置 PIN (>=4 位). 0=成功, -2=太短 */
int keyvault_set_pin(const char *pin);

/* 验证 PIN: 0=正确, 1=错误, 2=锁定中, <0=未初始化 */
int keyvault_verify_pin(const char *pin);

/* 距下次可重试的秒数 (0=可立即试) */
uint32_t keyvault_lock_remaining_sec(void);
uint32_t keyvault_fail_count(void);

/* 验证通过后, 用 PIN 派生解密钥 (UI 每次输入/增删前调用并保存) */
void keyvault_unlock_secret(const char *pin, uint8_t key[KEYVAULT_KEY_LEN]);

/* ---- 分类管理 ---- */
int         keyvault_cat_count(void);              /* 当前分类数 (含内置) */
const char *keyvault_cat_name(int i);               /* 分类名, i 越界返回 NULL */
int         keyvault_cat_add(const char *name);     /* 0=ok, -1=满, -2=重名 */
int         keyvault_cat_rename(int i, const char *name); /* 0=ok, -1=越界, -2=重名 */
int         keyvault_cat_remove(int i);             /* 0=ok, -1=越界, -2=仍有密钥, -3=内置不可删 */

/* ---- 密钥条目 ----
 * cat: 分类索引 (0..keyvault_cat_count()-1).
 * 条目 = 网站(含账号): 每条 {group 网站名, account 账号, secret 密码}.
 * 同一网站可加多条记录实现「多账号」; 列表/取用/删除均按"全局条目序号" idx.
 * V1→V2 自动迁移: 旧记录变 group=原网站名, account="" (见 keyvault_init). */
int keyvault_add(const uint8_t key[KEYVAULT_KEY_LEN], int cat, const char *group, const char *account, const char *secret);
int keyvault_count(void);
int keyvault_list(char names[][64], int cats[], int n_cap);  /* 返回条目数, 每项分类填到 cats[i] */
int keyvault_get_account(const uint8_t key[KEYVAULT_KEY_LEN], int idx, char *out, size_t out_cap); /* 取账号(网站名用 list) */
int keyvault_get_secret(const uint8_t key[KEYVAULT_KEY_LEN], int idx, char *out, size_t out_cap);
int keyvault_remove(const uint8_t key[KEYVAULT_KEY_LEN], int idx);

/* 重置: 删除密钥文件、PIN 与全部分类 */
void keyvault_reset(void);

/* 备份/恢复 (可选, 手动到 TF 卡): 导出含 salt+pinh+分类+记录, 跨设备用同 PIN 可恢复.
 * 0=成功; -1=失败. path 为完整文件路径 (如 "/sdcard/keyvault.bak"). */
int keyvault_export(const char *path);
int keyvault_import(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* KEYVAULT_H */