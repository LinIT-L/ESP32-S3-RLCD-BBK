/**
 * keyvault.c — 密钥管理器 加密存储模块.
 *
 * 职责:
 *   - PIN 锁 + 暴力破解指数退避 (30s/60s/2m/5m), 跨重启持久化失败次数
 *   - 密钥(密码) AES-CBC 加密后按条目存**内部 flash NVS** (不依赖 TF 卡)
 *   - PIN 经 SHA-256 派生密钥; 密钥可手动导入/导出到 TF 卡 (备份/迁移)
 *   - 分类 (网站/程序/游戏...) 存 NVS
 *
 * 记录存 NVS "keyvault" ns, 每条一个键 "r0".."r{rcnt-1}", 值 blob 格式 (V2, 支持账号):
 *   [ver 演变见下][cat u8][glen u8][group glen][alen u8][account alen][clen u16 LE][cipher(clen)]
 * group=网站名, account=账号, cipher=该账号密码(AES-128-CBC). 旧 V1 格式
 * [cat][nl][name][clen][cipher] 首次启动自动迁移为 group=name, account="" (见 keyvault_init).
 */
#include "keyvault.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

#include "svc_nvs.h"
/* ★ ESP-IDF 6.0 / Mbed TLS 4.0: 旧密码学原语头 (mbedtls/aes.h gcm.h pkcs5.h md.h
 *   sha256.h entropy.h ctr_drbg.h) 已全部移除, 统一改用 PSA Crypto。
 *   算法与参数保持完全一致 (PBKDF2-HMAC-SHA256 / AES-256-GCM / AES-256-CBC),
 *   已存 NVS 的密文无需重新加密。 */
#include "psa/crypto.h"
#include "esp_random.h"
#include "esp_timer.h"

#define TAG "KEYV"

#define KV_NVS_NS      "keyvault"
#define KV_NVS_PINHASH "pinh"
#define KV_NVS_SALT    "salt"
#define KV_NVS_FAILS   "fails"
#define KV_NVS_VALID   "valid"
#define KV_NVS_CATN    "catnum"
#define KV_NVS_CATPFX  "cat"     /* 分类名键前缀 */
#define KV_NVS_RECPFX  "r"       /* 记录键前缀 */
#define KV_NVS_RCNT    "rcnt"
#define KV_NVS_VER     "fver"    /* 记录格式版本 (2=带账号) */

#define KV_SALT_LEN    16
#define KV_KEY_LEN     32   /* AES-256 */
#define KV_ITER        10000    /* PBKDF2-HMAC-SHA256 迭代数 (解锁实测目标 <0.2s; 仍是单轮 SHA256 的 1e4 倍成本) */
#define KV_NONCE_LEN   12
#define KV_TAG_LEN     16
#define KV_FMT_NVS     "fmt"
#define KV_REC_MAXB    (1 + 1 + 60 + 1 + 60 + 2 + 200)   /* 单条记录 blob 上限 (带 group+account) */

static const char *s_def_cats[KEYVAULT_DEF_CATS] = {
    "\xe7\xbd\x91\xe7\xab\x99\xe8\xae\xba\xe5\x9d\x9b",   /* 网站论坛 */
    "\xe8\xbd\xaf\xe4\xbb\xb6\xe7\xa8\x8b\xe5\xba\x8f",   /* 软件程序 */
    "\xe6\xb8\xb8\xe6\x88\x8f\xe5\xa8\xb1\xe4\xb9\x90",   /* 游戏娱乐 */
};
static const uint32_t s_lock_seconds[4] = { 30, 60, 120, 300 };

static bool     s_ready = false;
static uint8_t  s_salt[KV_SALT_LEN];
static uint32_t s_fails = 0;
static uint32_t s_lock_until_ms = 0;
static char     s_cat_names[KEYVAULT_MAX_CATS][32];
static int      s_cat_count = 0;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---------- NVS 基础 ---------- */
static void kv_nvs_set_blob(const char *key, const void *d, size_t n) {
    nvs_handle_t h;
    if (svc_nvs_open(KV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_set_blob(h, key, d, n);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
}
static void kv_nvs_set_u32(const char *key, uint32_t v) {
    nvs_handle_t h;
    if (svc_nvs_open(KV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_set_u32(h, key, v);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
}
static uint32_t kv_nvs_get_u32(const char *key, uint32_t def) {
    uint32_t v = def;
    nvs_handle_t h;
    if (svc_nvs_open(KV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        svc_nvs_get_u32(h, key, &v);
        svc_nvs_close(h);
    }
    return v;
}
static int kv_nvs_get_blob(const char *key, void *d, size_t n) {
    nvs_handle_t h;
    if (svc_nvs_open(KV_NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t l = n;
    int r = svc_nvs_get_blob(h, key, d, &l);
    svc_nvs_close(h);
    /* 变长记录(单条)长度 l 往往 != 传入缓冲 n, 不能要求 l==n 否则记录读不出.
     * svc_nvs_get_blob 自带边界保护, 由调用方按内嵌 gl/al/cl 解析即可. */
    return (r == ESP_OK) ? 0 : -1;
}
static void kv_nvs_erase_key(const char *key) {
    nvs_handle_t h;
    if (svc_nvs_open(KV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_erase_key(h, key);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
}

/* ---------- 分类 (NVS) ---------- */
static void kv_cats_save(void) {
    kv_nvs_set_u32(KV_NVS_CATN, (uint32_t)s_cat_count);
    for (int i = 0; i < s_cat_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", KV_NVS_CATPFX, i);
        kv_nvs_set_blob(key, s_cat_names[i], 32);
    }
}
static void kv_cats_load(void) {
    s_cat_count = 0;
    uint32_t n = kv_nvs_get_u32(KV_NVS_CATN, 0);
    if (n > 0 && n <= KEYVAULT_MAX_CATS) {
        char key[16];
        snprintf(key, sizeof(key), "%s0", KV_NVS_CATPFX);
        uint8_t probe[32];
        if (kv_nvs_get_blob(key, probe, 32) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                char k2[16];
                snprintf(k2, sizeof(k2), "%s%d", KV_NVS_CATPFX, (int)i);
                if (kv_nvs_get_blob(k2, s_cat_names[i], 32) != 0) memset(s_cat_names[i], 0, 32);
                s_cat_count++;
            }
            return;
        }
    }
    for (int i = 0; i < KEYVAULT_DEF_CATS; i++) {
        memset(s_cat_names[i], 0, 32);
        strncpy(s_cat_names[i], s_def_cats[i], 31);
    }
    s_cat_count = KEYVAULT_DEF_CATS;
    kv_cats_save();
}

/* ---------- 记录 (NVS, 内部 flash) ---------- */
static int kv_rcnt(void) { return (int)kv_nvs_get_u32(KV_NVS_RCNT, 0); }
static void kv_rcnt_set(int n) { kv_nvs_set_u32(KV_NVS_RCNT, (uint32_t)n); }

/* 读第 idx 条记录 (V2): 解析 cat/group/account/cipher. 若干字段传 NULL 可跳过. */
static int kv_rec_get(int idx, uint8_t *cat, char *group, size_t gn,
                      char *account, size_t an,
                      uint8_t *cipher, uint16_t *clen) {
    char key[16];
    snprintf(key, sizeof(key), "%s%d", KV_NVS_RECPFX, idx);
    uint8_t buf[KV_REC_MAXB];
    memset(buf, 0, sizeof(buf));   /* 记录为变长: 读回短于缓冲时尾部清零, 防解析未初始化数据 */
    if (kv_nvs_get_blob(key, buf, sizeof(buf)) != 0) return -1;
    size_t o = 0;
    if (cat) *cat = buf[o];
    uint8_t gl = buf[o + 1];
    o += 2;
    if (o + gl + 1 + 60 + 2 > sizeof(buf)) return -1;
    if (group && gn > 0) {
        size_t cp = (gl < gn - 1) ? gl : (gn - 1);
        memcpy(group, buf + o, cp); group[cp] = 0;
    }
    o += gl;
    uint8_t al = buf[o++];
    if (account && an > 0) {
        size_t cp = (al < an - 1) ? al : (an - 1);
        memcpy(account, buf + o, cp); account[cp] = 0;
    }
    o += al;
    uint16_t cl = (uint16_t)((uint16_t)buf[o] | ((uint16_t)buf[o + 1] << 8));
    o += 2;
    if ((size_t)(o + cl) > sizeof(buf)) return -1;
    if (cipher && clen) { memcpy(cipher, buf + o, cl); *clen = cl; }
    return 0;
}

/* 写第 idx 条记录 (覆盖), V2 编码, clen=密文字节数 */
static int kv_rec_set(int idx, uint8_t cat, const char *group, const char *account,
                      const uint8_t *cipher, uint16_t clen) {
    uint8_t buf[KV_REC_MAXB];
    size_t o = 0;
    size_t gl = strlen(group), al = account ? strlen(account) : 0;
    if (gl > 60 || al > 60 || clen > 200) return -1;
    buf[o++] = cat;
    buf[o++] = (uint8_t)gl;
    memcpy(buf + o, group, gl); o += gl;
    buf[o++] = (uint8_t)al;
    memcpy(buf + o, account, al); o += al;
    buf[o++] = (uint8_t)(clen & 0xFF);
    buf[o++] = (uint8_t)((clen >> 8) & 0xFF);
    memcpy(buf + o, cipher, clen); o += clen;
    char key[16];
    snprintf(key, sizeof(key), "%s%d", KV_NVS_RECPFX, idx);
    kv_nvs_set_blob(key, buf, o);
    return 0;
}

/* ---------- PSA 封装 (v6.0) ---------- */

static bool s_psa_ready = false;
/* psa_crypto_init() 幂等; 若 IDF 启动流程已初始化则直接返回成功。 */
static void kv_psa_ensure(void) {
    if (!s_psa_ready && psa_crypto_init() == PSA_SUCCESS) s_psa_ready = true;
}

/* 返回 true = 成功。失败时 out 已清零, 调用方必须放弃本次认证/写入 ——
 * 绝不能把全零摘要当成"正常摘要"继续比对 (两边都失败会误判为 PIN 匹配)。 */
static bool kv_sha256(const uint8_t *in, size_t n, uint8_t out[32]) {
    size_t olen = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, in, n, out, 32, &olen) != PSA_SUCCESS || olen != 32) {
        memset(out, 0, 32);
        return false;
    }
    return true;
}

/* 导入 AES-256 密钥 (仅本次操作有效, 用完即销毁, 不落 PSA 持久存储) */
static psa_status_t kv_import_aes(const uint8_t *key, psa_algorithm_t alg, psa_key_id_t *out) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, KV_KEY_LEN * 8);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, alg);
    psa_status_t st = psa_import_key(&attr, key, KV_KEY_LEN, out);
    psa_reset_key_attributes(&attr);
    return st;
}

/* ★★ 2026-10-18: 密钥派生 SH256(pin||salt) → **PBKDF2-HMAC-SHA256 (60k 迭代)**。
 *   单轮 SHA256 对 8 位 PIN 只需 1e8 次就能爆破；PBKDF2 把每次猜测成本抬了 6 万倍。
 * 返回 true = 成功。失败时 key 已清零, 调用方必须丢弃它 —— 不得用全零密钥加解密。 */
static bool kv_derive_key(const char *pin, uint8_t key[KV_KEY_LEN]) {
    kv_psa_ensure();
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, KV_ITER);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, s_salt, KV_SALT_LEN);
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                            (const uint8_t *)pin, strlen(pin));
    if (st == PSA_SUCCESS)
        st = psa_key_derivation_output_bytes(&op, key, KV_KEY_LEN);
    psa_key_derivation_abort(&op);   /* 无论成败都释放派生上下文 */
    if (st != PSA_SUCCESS) {
        memset(key, 0, KV_KEY_LEN);  /* 派生失败绝不留弱密钥 */
        ESP_LOGE(TAG, "PBKDF2 派生失败 (psa_status=%d)", (int)st);
        return false;
    }
    return true;
}

/* 新格式单条: [nonce12][cipher][tag16]，AES-256-GCM；返回总长度 */
static size_t kv_gcm_encrypt(const uint8_t *key, const uint8_t *in, size_t n, uint8_t *out) {
    kv_psa_ensure();
    psa_key_id_t k = 0;
    if (kv_import_aes(key, PSA_ALG_GCM, &k) != PSA_SUCCESS) return 0;
    for (int i = 0; i < KV_NONCE_LEN; i += 4) {
        uint32_t r = esp_random();
        memcpy(out + i, &r, 4);
    }
    size_t olen = 0;
    /* PSA 输出 = cipher||tag, 与原 [nonce][cipher][tag] 布局一致 */
    psa_status_t st = psa_aead_encrypt(k, PSA_ALG_GCM, out, KV_NONCE_LEN, NULL, 0,
                                       in, n, out + KV_NONCE_LEN, n + KV_TAG_LEN, &olen);
    psa_destroy_key(k);
    if (st != PSA_SUCCESS) return 0;
    return KV_NONCE_LEN + n + KV_TAG_LEN;
}

/* 解密: 成功返回明文长度, 失败返回 0 (篡改/密钥不对都会失败) */
static size_t kv_gcm_decrypt(const uint8_t *key, const uint8_t *in, size_t n, uint8_t *out) {
    if (n < KV_NONCE_LEN + KV_TAG_LEN) return 0;
    kv_psa_ensure();
    size_t ct = n - KV_NONCE_LEN - KV_TAG_LEN;
    psa_key_id_t k = 0;
    if (kv_import_aes(key, PSA_ALG_GCM, &k) != PSA_SUCCESS) return 0;
    size_t olen = 0;
    psa_status_t st = psa_aead_decrypt(k, PSA_ALG_GCM, in, KV_NONCE_LEN, NULL, 0,
                                       in + KV_NONCE_LEN, ct + KV_TAG_LEN, out, ct, &olen);
    psa_destroy_key(k);
    return (st == PSA_SUCCESS) ? ct : 0;
}

/* ---------- API ---------- */
bool keyvault_has_pin(void) { return kv_nvs_get_u32(KV_NVS_VALID, 0) != 0; }

/* V1→V2 记录格式迁移: 旧[cat][nl][name][clen][cipher] → 新[cat][glen][group][alen=""]...[cipher]
 * 仅重排头部, 密文原样搬移, 无需解密. 幂等, 已完成则跳过. */
static void kv_migrate_v1(void) {
    if (kv_nvs_get_u32(KV_NVS_VER, 1) >= 2) return;
    int n = kv_rcnt();
    for (int i = 0; i < n; i++) {
        char key[16]; snprintf(key, sizeof(key), "%s%d", KV_NVS_RECPFX, i);
        uint8_t buf[KV_REC_MAXB];
        if (kv_nvs_get_blob(key, buf, sizeof(buf)) != 0) continue;
        size_t o = 0;
        uint8_t cat = buf[o], nl = buf[o + 1];
        o += 2;
        if ((size_t)(o + nl + 2) > sizeof(buf)) continue;
        uint16_t cl = (uint16_t)((uint16_t)buf[o + nl] | ((uint16_t)buf[o + nl + 1] << 8));
        if ((size_t)(o + nl + 2 + cl) > sizeof(buf)) continue;
        uint8_t out[KV_REC_MAXB]; size_t w = 0;
        out[w++] = cat; out[w++] = nl;                       /* cat + glen(=nl, name 当 group) */
        memcpy(out + w, buf + o, nl); w += nl;               /* group */
        out[w++] = 0;                                        /* alen=0 (账号空) */
        out[w++] = (uint8_t)(cl & 0xFF); out[w++] = (uint8_t)((cl >> 8) & 0xFF);
        memcpy(out + w, buf + o + nl + 2, cl); w += cl;      /* cipher */
        kv_nvs_set_blob(key, out, w);
    }
    kv_nvs_set_u32(KV_NVS_VER, 2);
}

esp_err_t keyvault_init(void) {
    if (s_ready) return ESP_OK;   /* ★ 幂等: 每次登录/请求都调也不重复读 NVS + 重试迁移 */
    if (kv_nvs_get_blob(KV_NVS_SALT, s_salt, sizeof(s_salt)) != 0) {
        /* v6.0: mbedtls_ctr_drbg 已移除, 硬件真随机源 esp_fill_random 等价且更省内存 */
        esp_fill_random(s_salt, sizeof(s_salt));
        kv_nvs_set_blob(KV_NVS_SALT, s_salt, sizeof(s_salt));
    }
    s_fails = kv_nvs_get_u32(KV_NVS_FAILS, 0);
    s_ready = true;
    if (s_cat_count == 0) kv_cats_load();
    kv_migrate_v1();
    /* 存量默认分类改名: 旧名(网站/程序/游戏) → (网站论坛/软件程序/游戏娱乐), 仅当仍是旧名时改 */
    {
        static const char *oldn[KEYVAULT_DEF_CATS] = {
            "\xe7\xbd\x91\xe7\xab\x99", "\xe7\xa8\x8b\xe5\xba\x8f", "\xe6\xb8\xb8\xe6\x88\x8f" };
        int changed = 0;
        for (int i = 0; i < KEYVAULT_DEF_CATS && i < s_cat_count; i++) {
            if (strcmp(s_cat_names[i], oldn[i]) == 0) {
                memset(s_cat_names[i], 0, 32);
                strncpy(s_cat_names[i], s_def_cats[i], 31);
                changed = 1;
            }
        }
        if (changed) kv_cats_save();
    }
    ESP_LOGI(TAG, "keyvault 就绪 (内部NVS, PIN%s, 失败=%u, 分类=%d, 记录=%d)",
             keyvault_has_pin() ? "已设置" : "未设置", (unsigned)s_fails,
             s_cat_count, kv_rcnt());
    return ESP_OK;
}

int keyvault_set_pin(const char *pin) {
    if (!s_ready) return -1;
    if (!pin || strlen(pin) > 47) return -2;   /* ★ 任意位数, 允许空密码 (linit 2026-10-19) */
    uint8_t ph[32];
    if (!kv_sha256((const uint8_t *)pin, strlen(pin), ph)) return -3;  /* 哈希失败: 不写 PIN, 不置 valid */
    kv_nvs_set_blob(KV_NVS_PINHASH, ph, sizeof(ph));
    kv_nvs_set_u32(KV_NVS_VALID, 1);
    kv_nvs_set_u32(KV_NVS_FAILS, 0);
    kv_nvs_set_u32(KV_FMT_NVS, 2);   /* 新装即新格式 */
    s_fails = 0;
    return 0;
}
uint32_t keyvault_lock_remaining_sec(void) {
    uint32_t now = now_ms();
    if (s_lock_until_ms <= now) return 0;
    return (s_lock_until_ms - now + 999) / 1000;
}
uint32_t keyvault_fail_count(void) { return s_fails; }

/* ★ 2026-10-19 linit: 空 PIN = 无密码 —— 用户不设密码时不该被任何门禁挡住.
 *   判定方式: 未设置 或 存储的 PIN 哈希 == sha256("")。不消耗失败次数。 */
bool keyvault_pin_is_empty(void) {
    if (!s_ready || !keyvault_has_pin()) return true;
    uint8_t ph[32], stored[32];
    if (!kv_sha256((const uint8_t *)"", 0, ph)) return false;  /* 哈希失败: 不敢判定为"空 PIN" */
    if (kv_nvs_get_blob(KV_NVS_PINHASH, stored, sizeof(stored)) != 0) return true;
    return memcmp(ph, stored, sizeof(ph)) == 0;
}

int keyvault_verify_pin(const char *pin) {
    if (!s_ready || !keyvault_has_pin()) return -1;
    if (keyvault_lock_remaining_sec() > 0) return 2;
    uint8_t ph[32];
    /* 哈希失败属于系统错误: 既不能当成"PIN 匹配", 也不应计入用户失败次数(避免误锁) */
    if (!kv_sha256((const uint8_t *)pin, strlen(pin), ph)) {
        ESP_LOGE(TAG, "PIN 校验中止: SHA-256 计算失败");
        return -1;
    }
    uint8_t stored[32];
    if (kv_nvs_get_blob(KV_NVS_PINHASH, stored, sizeof(stored)) != 0) return -1;
    if (memcmp(ph, stored, sizeof(ph)) == 0) {
        s_fails = 0; kv_nvs_set_u32(KV_NVS_FAILS, 0); return 0;
    }
    s_fails++;
    kv_nvs_set_u32(KV_NVS_FAILS, s_fails);
    /* ★ 2026-10-18 linit: **前 3 次不锁** —— 原来第 1 次错就锁 30 秒，
     *   手滑一下要干等半分钟，很恼人。第 4 次起才逐级加长。 */
    size_t idx = 0;
    if (s_fails >= 4) {
        idx = (s_fails - 4) < 4 ? (size_t)(s_fails - 4) : 3;
        s_lock_until_ms = now_ms() + s_lock_seconds[idx] * 1000UL;
    } else {
        s_lock_until_ms = 0;
    }
    ESP_LOGD(TAG, "PIN 错误 %u 次, 锁定 %u 秒", (unsigned)s_fails, (unsigned)(s_fails >= 4 ? s_lock_seconds[idx] : 0));
    return 1;
}
/* 旧格式(AES-CBC/零IV) → 新格式(GCM) 的一次性迁移。
 *
 * ⚠ 2026-10-29 (ESP-IDF 6.0 迁移): 本路径改为"安全失败", 不再自动转换数据。
 *   阻断原因(源码证据见迁移前实现):
 *     · 旧代码 kv_derive_key_v1() 只产出 16 字节密钥, 却被 kv_aes_crypt() 按
 *       KV_KEY_LEN(32)×8 位送进 AES —— 密钥后半段是栈上越界数据, 历史密文
 *       不存在可复现的密钥, 无法真正解出明文。
 *     · 旧实现不检查解密结果, 随后把"解不开的密文"当明文重新 GCM 加密并写回,
 *       再把 fmt 置 2 —— 记录被永久损坏且不可回退。
 *   现在的行为: 发现旧格式只打诊断日志, 记录原样保留(fmt 不置 2), 不写回、不丢数据。
 *   若将来确需抢救旧数据, 必须先在真机用旧固件确认可解密, 再单独设计带备份、
 *   可回退的迁移, 不能在这里猜密钥长度。
 *   可达性: fmt 由 keyvault_set_pin() 无条件写 2, 而引入 fmt 写入的提交与引入本
 *   函数的提交同为 dd5a69d(v1.4.2) —— 只有"设 PIN 早于 v1.4.2"的设备才可能进入。 */
static void kv_migrate_if_needed(const char *pin, const uint8_t key[KV_KEY_LEN]) {
    (void)pin; (void)key;
    if (kv_nvs_get_u32(KV_FMT_NVS, 0) == 2) return;
    int n = kv_rcnt();
    if (n > 0) {
        ESP_LOGW(TAG, "检测到 %d 条旧加密格式记录: 历史密钥长度存在歧义, "
                      "自动迁移会永久损坏数据, 已跳过 (记录保持原样)", n);
    }
}

/* 派生数据密钥。只有返回 ESP_OK 时才可把 key 用于加解密; 失败时 key 已清零。 */
esp_err_t keyvault_unlock_secret(const char *pin, uint8_t key[KV_KEY_LEN]) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!kv_derive_key(pin, key)) return ESP_FAIL;
    kv_migrate_if_needed(pin, key);
    return ESP_OK;
}

int keyvault_cat_count(void) { return s_cat_count; }
const char *keyvault_cat_name(int i) { return (i < 0 || i >= s_cat_count) ? NULL : s_cat_names[i]; }
int keyvault_cat_add(const char *name) {
    if (!name || !name[0] || s_cat_count >= KEYVAULT_MAX_CATS) return -1;
    for (int i = 0; i < s_cat_count; i++) if (strcmp(s_cat_names[i], name) == 0) return -2;
    memset(s_cat_names[s_cat_count], 0, 32);
    strncpy(s_cat_names[s_cat_count], name, 31);
    s_cat_count++;
    kv_cats_save();
    return 0;
}
int keyvault_cat_rename(int i, const char *name) {
    if (i < 0 || i >= s_cat_count || !name || !name[0]) return -1;
    for (int j = 0; j < s_cat_count; j++) if (j != i && strcmp(s_cat_names[j], name) == 0) return -2;
    memset(s_cat_names[i], 0, 32);
    strncpy(s_cat_names[i], name, 31);
    kv_cats_save();
    return 0;
}
int keyvault_cat_remove(int i) {
    if (i < 0 || i >= s_cat_count) return -1;
    if (i < KEYVAULT_DEF_CATS) return -3;
    int n = kv_rcnt();
    for (int k = 0; k < n; k++) {
        uint8_t c;
        if (kv_rec_get(k, &c, NULL, 0, NULL, 0, NULL, NULL) == 0 && c == (uint8_t)i) return -2;
    }
    for (int j = i; j < s_cat_count - 1; j++) memcpy(s_cat_names[j], s_cat_names[j + 1], 32);
    s_cat_count--;
    kv_cats_save();
    /* 受影响记录分类重排: cat>i 的向前挪 1 */
    uint8_t cipher[256];
    for (int k = 0; k < n; k++) {
        uint8_t c; char nm[64], ac[64]; uint16_t cl;
        if (kv_rec_get(k, &c, nm, sizeof(nm), ac, sizeof(ac), cipher, &cl) != 0) continue;
        if (c > (uint8_t)i) kv_rec_set(k, (uint8_t)(c - 1), nm, ac, cipher, cl);
    }
    return 0;
}

int keyvault_add(const uint8_t key[KV_KEY_LEN], int cat, const char *group, const char *account, const char *secret) {
    if (!s_ready || !keyvault_has_pin()) return -1;
    if (cat < 0 || cat >= s_cat_count) return -2;
    size_t gl = strlen(group), al = account ? strlen(account) : 0, sl = strlen(secret);
    if (gl == 0 || gl > 60 || al > 60 || sl == 0 || sl > 128) return -2;
    uint8_t *cipher = heap_caps_malloc(KV_NONCE_LEN + sl + KV_TAG_LEN, MALLOC_CAP_8BIT);
    if (!cipher) return -3;
    size_t clen = kv_gcm_encrypt(key, (const uint8_t *)secret, sl, cipher);
    /* 加密失败(kv_gcm_encrypt 返回 0) 时绝不落盘: 否则会写入 clen=0 的记录并让
     * 计数 +1 —— 该条目之后既读不出明文也覆盖不回去, 用户表现为"密码丢了"。
     * 成功加密的长度恒为 sl+28 (>0), 因此 0 只可能是失败。 */
    if (clen == 0) { heap_caps_free(cipher); return -4; }
    int n = kv_rcnt();
    int rc = kv_rec_set(n, (uint8_t)cat, group, account, cipher, (uint16_t)clen);
    if (rc == 0) kv_rcnt_set(n + 1);
    heap_caps_free(cipher);
    return rc;
}

/* ★ 2026-10-18: 通用 blob 加解密 (给其它模块复用同一套 GCM 密钥流)
 *   out 需 ≥ n + 28 字节; 解密返回明文长度, 失败返回 0 */
size_t keyvault_blob_encrypt(const uint8_t key[KV_KEY_LEN], const uint8_t *in, size_t n, uint8_t *out) {
    return kv_gcm_encrypt(key, in, n, out);
}
size_t keyvault_blob_decrypt(const uint8_t key[KV_KEY_LEN], const uint8_t *in, size_t n, uint8_t *out) {
    return kv_gcm_decrypt(key, in, n, out);
}

int keyvault_count(void) { return kv_rcnt(); }

int keyvault_list(char names[][64], int cats[], int n_cap) {
    int n = kv_rcnt();
    if (n > n_cap) n = n_cap;
    for (int i = 0; i < n; i++) {
        uint8_t c; char nm[64];
        if (kv_rec_get(i, &c, nm, sizeof(nm), NULL, 0, NULL, NULL) != 0) { memcpy(names[i], "", 1); cats[i] = 0; continue; }
        size_t cp = strlen(nm);
        if (cp > 63) cp = 63;
        memcpy(names[i], nm, cp);
        names[i][cp] = 0;
        cats[i] = c;
    }
    return n;
}

int keyvault_get_account(const uint8_t key[KV_KEY_LEN], int idx, char *out, size_t out_cap) {
    (void)key;
    char ac[64];
    if (kv_rec_get(idx, NULL, NULL, 0, ac, sizeof(ac), NULL, NULL) != 0) return -1;
    size_t cp = strlen(ac);
    if (cp > out_cap - 1) cp = out_cap - 1;
    memcpy(out, ac, cp);
    out[cp] = 0;
    return 0;
}

int keyvault_get_secret(const uint8_t key[KV_KEY_LEN], int idx, char *out, size_t out_cap) {
    uint8_t c[256], plain[256]; uint16_t cl;
    if (kv_rec_get(idx, NULL, NULL, 0, NULL, 0, c, &cl) != 0) return -1;
    size_t sl = kv_gcm_decrypt(key, c, cl, plain);
    if (sl == 0) return -1;                 /* 密钥不对 / 数据被改 */
    while (sl > 0 && plain[sl - 1] == 0) sl--;
    size_t cp = (sl < out_cap - 1) ? sl : out_cap - 1;
    memcpy(out, plain, cp);
    out[cp] = 0;
    return 0;
}

int keyvault_remove(const uint8_t key[KV_KEY_LEN], int idx) {
    (void)key;
    int n = kv_rcnt();
    if (idx < 0 || idx >= n) return -1;
    if (idx == n - 1) {
        char k[16]; snprintf(k, sizeof(k), "%s%d", KV_NVS_RECPFX, idx);
        kv_nvs_erase_key(k);
    } else {
        uint8_t c[256]; uint16_t cl; char nm[64], ac[64]; uint8_t cat;
        if (kv_rec_get(n - 1, &cat, nm, sizeof(nm), ac, sizeof(ac), c, &cl) == 0)
            kv_rec_set(idx, cat, nm, ac, c, cl);
        { char k[16]; snprintf(k, sizeof(k), "%s%d", KV_NVS_RECPFX, n - 1); kv_nvs_erase_key(k); }
    }
    kv_rcnt_set(n - 1);
    return 0;
}

void keyvault_reset(void) {
    int n = kv_rcnt();
    for (int i = 0; i < n + 1; i++) {
        char k[16]; snprintf(k, sizeof(k), "%s%d", KV_NVS_RECPFX, i);
        kv_nvs_erase_key(k);
    }
    kv_nvs_erase_key(KV_NVS_RCNT);
    nvs_handle_t h;
    if (svc_nvs_open(KV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_erase_all(h);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
    s_fails = 0;
    s_cat_count = 0;
    kv_cats_load();
}

/* ---------- 导入 / 导出 (可选备份到 TF 卡) ----------
 * 文件格式:
 *   magic "KVEXP2"(6) + salt(16) + valid(u8) + pinh(32)
 *   + catnum(u8) + 分类名(每个 32B) + rcnt(u32)
 *   + 每条记录: cat(u8) glen(u8) group glen alen(u8) account alen clen(u16 LE) cipher clen
 * 旧 "KVEXP1" 文件 (无账号) 仍可导入, 解析为 group=name, 账号空.
 * 密文由 PIN 派生钥 AES 加密, 导出含 salt+pinh, 故可跨设备用同一 PIN 恢复. */
#define KVEX_MAGIC   "KVEXP2"
#define KVEX_MAGIC1  "KVEXP1"
#define KVEX_HDR_NAMELEN 32
int keyvault_export(const char *path) {
    uint32_t valid = kv_nvs_get_u32(KV_NVS_VALID, 0);
    uint8_t pinh[32] = {0};
    kv_nvs_get_blob(KV_NVS_PINHASH, pinh, sizeof(pinh));
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int rc = -1;
    if (fwrite(KVEX_MAGIC, 1, 6, f) == 6 &&
        fwrite(s_salt, 1, KV_SALT_LEN, f) == KV_SALT_LEN &&
        fwrite(&valid, 1, 1, f) == 1 &&
        fwrite(pinh, 1, sizeof(pinh), f) == sizeof(pinh)) {
        uint8_t cn = (uint8_t)s_cat_count;
        if (fwrite(&cn, 1, 1, f) == 1) {
            int ok = 1;
            for (int i = 0; i < s_cat_count; i++)
                if (fwrite(s_cat_names[i], 1, KVEX_HDR_NAMELEN, f) != KVEX_HDR_NAMELEN) { ok = 0; break; }
            if (ok) {
                int n = kv_rcnt();
                uint32_t un = (uint32_t)n;
                bool bad = (fwrite(&un, 4, 1, f) != 1);
                for (int i = 0; i < n && !bad; i++) {
                    uint8_t cat; char nm[64], ac[64]; uint8_t c[256]; uint16_t cl;
                    if (kv_rec_get(i, &cat, nm, sizeof(nm), ac, sizeof(ac), c, &cl) != 0) { bad = true; break; }
                    uint8_t gl = (uint8_t)strlen(nm), al = (uint8_t)strlen(ac);
                    if (fwrite(&cat, 1, 1, f) != 1 || fwrite(&gl, 1, 1, f) != 1) { bad = true; break; }
                    if (fwrite(nm, 1, gl, f) != gl) { bad = true; break; }
                    if (fwrite(&al, 1, 1, f) != 1) { bad = true; break; }
                    if (fwrite(ac, 1, al, f) != al) { bad = true; break; }
                    if (fwrite(&cl, 2, 1, f) != 1) { bad = true; break; }
                    if (fwrite(c, 1, cl, f) != cl) { bad = true; break; }
                }
                if (!bad) rc = 0;
            }
        }
    }
    fclose(f);
    if (rc != 0) remove(path);
    return rc;
}

int keyvault_import(const char *path) {
    EXT_RAM_BSS_ATTR static uint8_t buf[4096];   /* PSRAM, 省 4KB 内部 RAM */
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int rc = -1;
    bool v2 = false;
    if (fread(buf, 1, 6, f) == 6 && (memcmp(buf, KVEX_MAGIC, 6) == 0 || memcmp(buf, KVEX_MAGIC1, 6) == 0)) {
        v2 = (memcmp(buf, KVEX_MAGIC, 6) == 0);
        size_t o = 0;
        if (fread(buf + 6, 1, KV_SALT_LEN, f) == KV_SALT_LEN) o = 6 + KV_SALT_LEN;
        else o = 0;
        if (o) {
            uint8_t valid; uint8_t pinh[32];
            if (fread(&valid, 1, 1, f) == 1 && fread(pinh, 1, 32, f) == 32) {
                uint8_t cn;
                if (fread(&cn, 1, 1, f) == 1 && cn <= KEYVAULT_MAX_CATS) {
                    int ok = 1;
                    char cats[KEYVAULT_MAX_CATS][32];
                    memset(cats, 0, sizeof(cats));
                    for (int i = 0; i < cn; i++)
                        if (fread(cats[i], 1, KVEX_HDR_NAMELEN, f) != KVEX_HDR_NAMELEN) { ok = 0; break; }
                    if (ok) {
                        uint32_t n;
                        if (fread(&n, 4, 1, f) == 1 && n <= KEYVAULT_MAX_ENTRIES) {
                            int bad = 0;
                            uint8_t rec[KV_REC_MAXB];
                            for (uint32_t i = 0; i < n && !bad; i++) {
                                uint8_t cat, gl, al = 0;
                                if (fread(&cat, 1, 1, f) != 1 || fread(&gl, 1, 1, f) != 1 || gl > 60) { bad = 1; break; }
                                if (fread(rec, 1, gl, f) != gl) { bad = 1; break; }
                                size_t glen = gl;
                                if (v2) {
                                    if (fread(&al, 1, 1, f) != 1 || al > 60) { bad = 1; break; }
                                    if (fread(rec + glen, 1, al, f) != al) { bad = 1; break; }
                                }
                                size_t alen = v2 ? al : 0;
                                uint16_t cl;
                                if (fread(&cl, 2, 1, f) != 1 || cl > 200) { bad = 1; break; }
                                if (cat >= cn) { bad = 1; break; }
                                size_t cipher_off = glen + alen;
                                if (fread(rec + cipher_off, 1, cl, f) != cl) { bad = 1; break; }
                                /* 写入 V2 格式: [cat][glen][group][alen][account][cl][cipher] */
                                size_t w = 0; uint8_t b[KV_REC_MAXB];
                                b[w++] = cat; b[w++] = gl;
                                memcpy(b + w, rec, glen); w += glen;
                                b[w++] = al;
                                memcpy(b + w, rec + glen, alen); w += alen;
                                b[w++] = (uint8_t)(cl & 0xFF); b[w++] = (uint8_t)((cl >> 8) & 0xFF);
                                memcpy(b + w, rec + cipher_off, cl); w += cl;
                                char k[16]; snprintf(k, sizeof(k), "%s%u", KV_NVS_RECPFX, (unsigned)i);
                                kv_nvs_set_blob(k, b, w);
                            }
                            if (!bad) {
                                int nn = (int)n;
                                kv_rcnt_set(nn);
                                /* 覆盖 salt / pinh / valid / 分类 */
                                memcpy(s_salt, buf + 6, KV_SALT_LEN);
                                kv_nvs_set_blob(KV_NVS_SALT, s_salt, KV_SALT_LEN);
                                kv_nvs_set_blob(KV_NVS_PINHASH, pinh, 32);
                                kv_nvs_set_u32(KV_NVS_VALID, valid ? 1 : 0);
                                s_cat_count = cn;
                                for (int i = 0; i < cn; i++) memcpy(s_cat_names[i], cats[i], 32);
                                kv_cats_save();
                                s_fails = 0; kv_nvs_set_u32(KV_NVS_FAILS, 0);
                                rc = 0;
                            }
                        }
                    }
                }
            }
        }
    }
    fclose(f);
    return rc;
}
