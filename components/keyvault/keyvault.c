/**
 * keyvault.c — 密钥管理器 加密存储模块.
 *
 * 职责:
 *   - PIN 锁 + 暴力破解指数退避 (30s/60s/2m/5m), 跨重启持久化失败次数
 *   - 密钥(密码)以 AES-CBC 加密后存 SD 卡文件, 密钥由 PIN 经 SHA-256 派生
 *   - 密钥可按分类 (网站/程序/游戏/自定义...) 归类, 分类名存 NVS
 *   - 提供 增/删/列出/取用 密钥条目 (条目 = cat + name + secret)
 *   - 纯存储/加密, 不含 UI (UI 在 page_keymanager.c)
 *
 * 保存文件: /sdcard/keyvault.dat
 *   每条记录 = [cat(1)] [name_len(2)] [name] [secret_cipher_len(2)] [secret_cipher]
 *   说明: 本方案为"防直读 SD"级别. 派生钥 = SHA-256(PIN + device salt).
 */
#include "keyvault.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "esp_timer.h"

#define TAG "KEYV"

#define KV_NVS_NS      "keyvault"
#define KV_NVS_PINHASH "pinh"
#define KV_NVS_SALT    "salt"
#define KV_NVS_FAILS   "fails"
#define KV_NVS_VALID   "valid"
#define KV_NVS_CATN    "catnum"
#define KV_NVS_CATPFX  "cat"     /* 分类名键前缀: "cat0","cat1"... */

#define KV_PATH        "/sdcard/keyvault.dat"

#define KV_SALT_LEN    16
#define KV_IV_LEN      16
#define KV_KEY_LEN     16   /* AES-128 */

static const char *s_def_cats[KEYVAULT_DEF_CATS] = {
    "\xe7\xbd\x91\xe7\xab\x99",   /* 网站 */
    "\xe7\xa8\x8b\xe5\xba\x8f",   /* 程序 */
    "\xe6\xb8\xb8\xe6\x88\x8f",   /* 游戏 */
};

static const uint32_t s_lock_seconds[4] = { 30, 60, 120, 300 };

static bool     s_ready = false;
static uint8_t  s_salt[KV_SALT_LEN];
static uint32_t s_fails = 0;
static uint32_t s_lock_until_ms = 0;
static char     s_cat_names[KEYVAULT_MAX_CATS][32];
static int      s_cat_count = 0;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void kv_nvs_set_blob(const char *key, const void *d, size_t n) {
    nvs_handle_t h;
    if (nvs_open(KV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, d, n);
        nvs_commit(h);
        nvs_close(h);
    }
}
static void kv_nvs_set_u32(const char *key, uint32_t v) {
    nvs_handle_t h;
    if (nvs_open(KV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}
static uint32_t kv_nvs_get_u32(const char *key, uint32_t def) {
    uint32_t v = def;
    nvs_handle_t h;
    if (nvs_open(KV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, key, &v);
        nvs_close(h);
    }
    return v;
}
static int kv_nvs_get_blob(const char *key, void *d, size_t n) {
    nvs_handle_t h;
    if (nvs_open(KV_NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t l = n;
    int r = nvs_get_blob(h, key, d, &l);
    nvs_close(h);
    return (r == ESP_OK && l == n) ? 0 : -1;
}

/* 持久化分类名数组到 NVS: 键 "cat0".."cat{n-1}" + 数量 "catnum" */
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
        /* 首键缺失视为从未保存过 → 回退默认 */
        char key[16];
        snprintf(key, sizeof(key), "%s0", KV_NVS_CATPFX);
        uint8_t probe[32];
        if (kv_nvs_get_blob(key, probe, 32) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                char k2[16];
                snprintf(k2, sizeof(k2), "%s%d", KV_NVS_CATPFX, (int)i);
                if (kv_nvs_get_blob(k2, s_cat_names[i], 32) != 0) {
                    memset(s_cat_names[i], 0, 32);
                }
                s_cat_count++;
            }
            return;
        }
    }
    /* 首次: 建立内置默认分类 */
    for (int i = 0; i < KEYVAULT_DEF_CATS; i++) {
        memset(s_cat_names[i], 0, 32);
        strncpy(s_cat_names[i], s_def_cats[i], 31);
    }
    s_cat_count = KEYVAULT_DEF_CATS;
    kv_cats_save();
}

static void kv_derive_key(const char *pin, uint8_t key[KV_KEY_LEN]) {
    uint8_t mix[64];
    size_t m = 0;
    size_t plen = strlen(pin);
    for (size_t i = 0; i < plen; i++) mix[m++] = (uint8_t)pin[i];
    for (size_t i = 0; i < KV_SALT_LEN; i++) mix[m++] = s_salt[i];
    mbedtls_sha256(mix, m, mix, 0);
    memcpy(key, mix, KV_KEY_LEN);
}

static void kv_aes_crypt(int mode, const uint8_t *key, const uint8_t iv[KV_IV_LEN],
                         uint8_t *data, size_t len) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, KV_KEY_LEN * 8);
    uint8_t ivbuf[KV_IV_LEN];
    memcpy(ivbuf, iv, KV_IV_LEN);
    mbedtls_aes_crypt_cbc(&aes, mode, len, ivbuf, data, data);
    mbedtls_aes_free(&aes);
}

static int kv_write_rec(FILE *f, uint8_t cat, const char *name, const uint8_t *cipher, uint16_t clen) {
    uint16_t nl = (uint16_t)strlen(name);
    if (fwrite(&cat, 1, 1, f) != 1) return -1;
    if (fwrite(&nl, sizeof(nl), 1, f) != 1) return -1;
    if (fwrite(name, 1, nl, f) != nl) return -1;
    if (fwrite(&clen, sizeof(clen), 1, f) != 1) return -1;
    if (fwrite(cipher, 1, clen, f) != clen) return -1;
    return 0;
}

/* 跳到第 idx 条记录, 返回时文件指针位于该记录 secret_cipher 数据之后;
 * 若找到目标则把 cat 写入 *cat, 否则返回 -1. */
static int kv_seek_rec(FILE *f, int idx, uint8_t *cat) {
    int i = 0;
    while (1) {
        uint8_t c;
        uint16_t nl, cl;
        if (fread(&c, 1, 1, f) != 1) return -1;
        if (fread(&nl, sizeof(nl), 1, f) != 1) return -1;
        if (nl == 0 || nl > 60) return -1;
        if (fseek(f, nl, SEEK_CUR) != 0) return -1;
        if (fread(&cl, sizeof(cl), 1, f) != 1) return -1;
        if (cl == 0 || cl > 200) return -1;
        if (i == idx) { if (cat) *cat = c; return 0; }
        if (fseek(f, cl, SEEK_CUR) != 0) return -1;
        i++;
    }
}

/* ---------- API ---------- */
bool keyvault_has_pin(void) { return kv_nvs_get_u32(KV_NVS_VALID, 0) != 0; }

esp_err_t keyvault_init(void) {
    if (kv_nvs_get_blob(KV_NVS_SALT, s_salt, sizeof(s_salt)) != 0) {
        mbedtls_entropy_context ent;
        mbedtls_ctr_drbg_context ctr;
        mbedtls_entropy_init(&ent);
        mbedtls_ctr_drbg_init(&ctr);
        if (mbedtls_ctr_drbg_seed(&ctr, mbedtls_entropy_func, &ent, NULL, 0) == 0)
            mbedtls_ctr_drbg_random(&ctr, s_salt, sizeof(s_salt));
        mbedtls_ctr_drbg_free(&ctr);
        mbedtls_entropy_free(&ent);
        kv_nvs_set_blob(KV_NVS_SALT, s_salt, sizeof(s_salt));
    }
    s_fails = kv_nvs_get_u32(KV_NVS_FAILS, 0);
    s_ready = true;
    if (s_cat_count == 0) kv_cats_load();
    ESP_LOGI(TAG, "keyvault 就绪 (PIN%s, 失败=%u, 分类=%d)", keyvault_has_pin() ? "已设置" : "未设置",
             (unsigned)s_fails, s_cat_count);
    return ESP_OK;
}

int keyvault_set_pin(const char *pin) {
    if (!s_ready) return -1;
    if (strlen(pin) < 4) return -2;
    uint8_t ph[32];
    mbedtls_sha256((const uint8_t *)pin, strlen(pin), ph, 0);
    kv_nvs_set_blob(KV_NVS_PINHASH, ph, sizeof(ph));
    kv_nvs_set_u32(KV_NVS_VALID, 1);
    kv_nvs_set_u32(KV_NVS_FAILS, 0);
    s_fails = 0;
    return 0;
}

uint32_t keyvault_lock_remaining_sec(void) {
    uint32_t now = now_ms();
    if (s_lock_until_ms <= now) return 0;
    return (s_lock_until_ms - now + 999) / 1000;
}

uint32_t keyvault_fail_count(void) { return s_fails; }

int keyvault_verify_pin(const char *pin) {
    if (!s_ready || !keyvault_has_pin()) return -1;
    if (keyvault_lock_remaining_sec() > 0) return 2;
    uint8_t ph[32];
    mbedtls_sha256((const uint8_t *)pin, strlen(pin), ph, 0);
    uint8_t stored[32];
    if (kv_nvs_get_blob(KV_NVS_PINHASH, stored, sizeof(stored)) != 0) return -1;
    if (memcmp(ph, stored, sizeof(ph)) == 0) {
        s_fails = 0;
        kv_nvs_set_u32(KV_NVS_FAILS, 0);
        return 0;
    }
    s_fails++;
    kv_nvs_set_u32(KV_NVS_FAILS, s_fails);
    size_t idx = (s_fails >= 1 && s_fails <= 4) ? (s_fails - 1) : 3;
    s_lock_until_ms = now_ms() + s_lock_seconds[idx] * 1000UL;
    ESP_LOGW(TAG, "PIN 错误 %u 次, 锁定 %u 秒", (unsigned)s_fails, (unsigned)s_lock_seconds[idx]);
    return 1;
}

void keyvault_unlock_secret(const char *pin, uint8_t key[KV_KEY_LEN]) {
    kv_derive_key(pin, key);
}

/* ---- 分类 ---- */
int keyvault_cat_count(void) { return s_cat_count; }
const char *keyvault_cat_name(int i) {
    if (i < 0 || i >= s_cat_count) return NULL;
    return s_cat_names[i];
}
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
    if (i < KEYVAULT_DEF_CATS) return -3;   /* 内置不可删 */
    /* 该分类下仍有密钥则禁止删除 */
    FILE *f = fopen(KV_PATH, "rb");
    if (f) {
        while (1) {
            uint8_t c;
            uint16_t nl, cl;
            if (fread(&c, 1, 1, f) != 1) break;
            if (fread(&nl, sizeof(nl), 1, f) != 1) break;
            if (fseek(f, nl, SEEK_CUR) != 0) break;
            if (fread(&cl, sizeof(cl), 1, f) != 1) break;
            if (c == (uint8_t)i) { fclose(f); return -2; }
            if (fseek(f, cl, SEEK_CUR) != 0) break;
        }
        fclose(f);
    }
    /* 分类名前移, 并把受影响记录的 cat 重排 (i 之后的向前挪 1) */
    for (int j = i; j < s_cat_count - 1; j++) memcpy(s_cat_names[j], s_cat_names[j + 1], 32);
    s_cat_count--;
    kv_cats_save();
    f = fopen(KV_PATH, "rb+");
    if (f) {
        while (1) {
            long rec_start = ftell(f);
            uint8_t c;
            uint16_t nl, cl;
            if (fread(&c, 1, 1, f) != 1) break;
            if (fread(&nl, sizeof(nl), 1, f) != 1) break;
            if (fseek(f, nl, SEEK_CUR) != 0) break;
            if (fread(&cl, sizeof(cl), 1, f) != 1) break;
            if (c > (uint8_t)i) {
                uint8_t nc = c - 1;
                fseek(f, rec_start, SEEK_SET);
                fwrite(&nc, 1, 1, f);
                fseek(f, 0, SEEK_CUR);
            }
            if (fseek(f, cl, SEEK_CUR) != 0) break;
        }
        fclose(f);
    }
    return 0;
}

int keyvault_add(const uint8_t key[KV_KEY_LEN], int cat, const char *name, const char *secret) {
    if (!s_ready || !keyvault_has_pin()) return -1;
    if (cat < 0 || cat >= s_cat_count) return -2;
    size_t nl = strlen(name), sl = strlen(secret);
    if (nl == 0 || nl > 60 || sl == 0 || sl > 128) return -2;
    size_t clen = ((sl + 15) / 16) * 16;
    if (clen < 16) clen = 16;
    uint8_t *cipher = heap_caps_malloc(clen, MALLOC_CAP_8BIT);
    if (!cipher) return -3;
    memset(cipher, 0, clen);
    memcpy(cipher, secret, sl);
    uint8_t iv[KV_IV_LEN] = {0};
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context ctr;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&ctr);
    if (mbedtls_ctr_drbg_seed(&ctr, mbedtls_entropy_func, &ent, NULL, 0) == 0)
        mbedtls_ctr_drbg_random(&ctr, iv, sizeof(iv));
    mbedtls_ctr_drbg_free(&ctr);
    mbedtls_entropy_free(&ent);
    kv_aes_crypt(MBEDTLS_AES_ENCRYPT, key, iv, cipher, clen);
    int rc = -4;
    FILE *f = fopen(KV_PATH, "ab");
    if (f) {
        if (kv_write_rec(f, (uint8_t)cat, name, cipher, (uint16_t)clen) == 0) rc = 0;
        fclose(f);
    }
    heap_caps_free(cipher);
    return rc;
}

int keyvault_count(void) {
    FILE *f = fopen(KV_PATH, "rb");
    if (!f) return 0;
    int n = 0;
    while (1) {
        uint8_t c;
        uint16_t nl, cl;
        if (fread(&c, 1, 1, f) != 1) break;
        if (fread(&nl, sizeof(nl), 1, f) != 1) break;
        if (nl == 0 || nl > 60) break;
        if (fseek(f, nl, SEEK_CUR) != 0) break;
        if (fread(&cl, sizeof(cl), 1, f) != 1) break;
        if (fseek(f, cl, SEEK_CUR) != 0) break;
        n++;
    }
    fclose(f);
    return n;
}

int keyvault_list(char names[][64], int cats[], int n_cap) {
    FILE *f = fopen(KV_PATH, "rb");
    if (!f) return 0;
    int n = 0;
    while (n < n_cap) {
        uint8_t c;
        uint16_t nl, cl;
        if (fread(&c, 1, 1, f) != 1) break;
        if (fread(&nl, sizeof(nl), 1, f) != 1) break;
        if (nl == 0 || nl > 60) break;
        if (nl >= 63) nl = 62;
        if (fread(names[n], 1, nl, f) != nl) break;
        names[n][nl] = 0;
        if (fread(&cl, sizeof(cl), 1, f) != 1) break;
        cats[n] = c;
        if (fseek(f, cl, SEEK_CUR) != 0) break;
        n++;
    }
    fclose(f);
    return n;
}

int keyvault_get_secret(const uint8_t key[KV_KEY_LEN], int idx, char *out, size_t out_cap) {
    FILE *f = fopen(KV_PATH, "rb");
    if (!f) return -1;
    if (kv_seek_rec(f, idx, NULL) != 0) { fclose(f); return -1; }
    uint16_t cl;
    if (fread(&cl, sizeof(cl), 1, f) != 1) { fclose(f); return -1; }
    if (cl == 0 || cl > 200) { fclose(f); return -1; }
    uint8_t *c = heap_caps_malloc(cl, MALLOC_CAP_8BIT);
    if (!c) { fclose(f); return -1; }
    uint8_t iv[KV_IV_LEN] = {0};
    int rc = -1;
    if (fread(c, 1, cl, f) == cl) {
        kv_aes_crypt(MBEDTLS_AES_DECRYPT, key, iv, c, cl);
        size_t sl = cl;
        while (sl > 0 && c[sl - 1] == 0) sl--;
        if (out_cap < sl + 1) sl = out_cap - 1;
        memcpy(out, c, sl);
        out[sl] = 0;
        rc = 0;
    }
    heap_caps_free(c);
    fclose(f);
    return rc;
}

int keyvault_remove(const uint8_t key[KEYVAULT_KEY_LEN], int idx) {
    (void)key;
    static uint8_t  kcat[64];
    static char     kn[64][64];
    static uint8_t  keep[64][256];
    static uint16_t kcl[64];
    int knum = 0;
    FILE *f = fopen(KV_PATH, "rb");
    if (!f) return -1;
    while (knum < 64) {
        uint8_t c;
        uint16_t nl, cl;
        if (fread(&c, 1, 1, f) != 1) break;
        if (fread(&nl, sizeof(nl), 1, f) != 1) break;
        if (nl == 0 || nl > 60) break;
        if (fread(kn[knum], 1, nl, f) != nl) break;
        kn[knum][nl] = 0;
        if (fread(&cl, sizeof(cl), 1, f) != 1) break;
        if (cl == 0 || cl > 200) break;
        if (fread(keep[knum], 1, cl, f) != cl) break;
        kcat[knum] = c;
        kcl[knum] = (uint16_t)cl;
        knum++;
    }
    fclose(f);
    f = fopen(KV_PATH, "wb");
    if (!f) return -1;
    for (int k = 0; k < knum; k++) {
        if (k == idx) continue;
        if (kv_write_rec(f, kcat[k], kn[k], keep[k], kcl[k]) != 0) { fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

void keyvault_reset(void) {
    remove(KV_PATH);
    nvs_handle_t h;
    if (nvs_open(KV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_fails = 0;
    s_cat_count = 0;
    kv_cats_load();   /* 重建默认分类 */
}