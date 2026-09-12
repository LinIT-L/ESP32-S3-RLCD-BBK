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
#define KV_NVS_CATPFX  "cat"     /* 分类名键前缀 */
#define KV_NVS_RECPFX  "r"       /* 记录键前缀 */
#define KV_NVS_RCNT    "rcnt"
#define KV_NVS_VER     "fver"    /* 记录格式版本 (2=带账号) */

#define KV_SALT_LEN    16
#define KV_IV_LEN      16
#define KV_KEY_LEN     16   /* AES-128 */
#define KV_REC_MAXB    (1 + 1 + 60 + 1 + 60 + 2 + 144)   /* 单条记录 blob 上限 (带 group+account) */

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
    /* 变长记录(单条)长度 l 往往 != 传入缓冲 n, 不能要求 l==n 否则记录读不出.
     * nvs_get_blob 自带边界保护, 由调用方按内嵌 gl/al/cl 解析即可. */
    return (r == ESP_OK) ? 0 : -1;
}
static void kv_nvs_erase_key(const char *key) {
    nvs_handle_t h;
    if (nvs_open(KV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
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
    if (gl > 60 || al > 60 || clen > 144) return -1;
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

static void kv_derive_key(const char *pin, uint8_t key[KV_KEY_LEN]) {
    uint8_t mix[64];
    size_t m = 0;
    size_t plen = strlen(pin);
    if (plen > 47) plen = 47;   /* mix[64]: 47(PIN)+16(盐)=63 封顶, 防长 PIN 栈越界 */
    for (size_t i = 0; i < plen; i++) mix[m++] = (uint8_t)pin[i];
    for (size_t i = 0; i < KV_SALT_LEN; i++) mix[m++] = s_salt[i];
    mbedtls_sha256(mix, m, mix, 0);
    memcpy(key, mix, KV_KEY_LEN);
}
static void kv_aes_crypt(int mode, const uint8_t *key, const uint8_t iv[KV_IV_LEN],
                         uint8_t *data, size_t len) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    /* 解密必须展开逆密钥表(setkey_dec), 否则解出乱码(原恒用 setkey_enc) */
    if (mode == MBEDTLS_AES_DECRYPT) mbedtls_aes_setkey_dec(&aes, key, KV_KEY_LEN * 8);
    else                             mbedtls_aes_setkey_enc(&aes, key, KV_KEY_LEN * 8);
    uint8_t ivbuf[KV_IV_LEN];
    memcpy(ivbuf, iv, KV_IV_LEN);
    mbedtls_aes_crypt_cbc(&aes, mode, len, ivbuf, data, data);
    mbedtls_aes_free(&aes);
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
    if (strlen(pin) < 8) return -2;   /* V1.6.x: PIN 至少 8 位 (兼作 AP Wi-Fi 密码) */
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
        s_fails = 0; kv_nvs_set_u32(KV_NVS_FAILS, 0); return 0;
    }
    s_fails++;
    kv_nvs_set_u32(KV_NVS_FAILS, s_fails);
    size_t idx = (s_fails >= 1 && s_fails <= 4) ? (s_fails - 1) : 3;
    s_lock_until_ms = now_ms() + s_lock_seconds[idx] * 1000UL;
    ESP_LOGW(TAG, "PIN 错误 %u 次, 锁定 %u 秒", (unsigned)s_fails, (unsigned)s_lock_seconds[idx]);
    return 1;
}
void keyvault_unlock_secret(const char *pin, uint8_t key[KV_KEY_LEN]) { kv_derive_key(pin, key); }

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
    size_t clen = ((sl + 15) / 16) * 16; if (clen < 16) clen = 16;
    uint8_t *cipher = heap_caps_malloc(clen, MALLOC_CAP_8BIT);
    if (!cipher) return -3;
    memset(cipher, 0, clen);
    memcpy(cipher, secret, sl);
    uint8_t iv[KV_IV_LEN] = {0};
    kv_aes_crypt(MBEDTLS_AES_ENCRYPT, key, iv, cipher, clen);
    int n = kv_rcnt();
    int rc = kv_rec_set(n, (uint8_t)cat, group, account, cipher, (uint16_t)clen);
    if (rc == 0) kv_rcnt_set(n + 1);
    heap_caps_free(cipher);
    return rc;
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
    uint8_t c[256]; uint16_t cl;
    if (kv_rec_get(idx, NULL, NULL, 0, NULL, 0, c, &cl) != 0) return -1;
    uint8_t iv[KV_IV_LEN] = {0};
    kv_aes_crypt(MBEDTLS_AES_DECRYPT, key, iv, c, cl);
    size_t sl = cl;
    while (sl > 0 && c[sl - 1] == 0) sl--;
    size_t cp = (sl < out_cap - 1) ? sl : out_cap - 1;
    memcpy(out, c, cp);
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
    if (nvs_open(KV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
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
