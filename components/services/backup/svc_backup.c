/**
 * svc_backup.c — 通用备份策略服务实现
 *
 * 职责边界（详见 svc_backup.h）:
 *   只做"策略 + 文件保障"，不解析业务数据、不打包、不加密。
 *
 * 关键设计点:
 *   ① **原子写**: 先写 <path>.tmp，stat 校验长度，再 rename 覆盖目标。
 *      直接写目标文件的话，写到一半拔卡/断电会留下半截文件，而它看起来
 *      "存在"，恢复时才发现坏了 —— 那时原备份已经被覆盖，没得救。
 *   ② **双校验**: 头里的 len 必须与文件实际长度一致，payload 的 crc32 必须对。
 *      只查 crc 不查长度，遇到"尾部被补零"仍会通过；只查长度不查 crc，
 *      遇到"中间扇区坏块"会通过。两个都要。
 *   ③ **扩展名 .bkp**: page_fav 的备份扫描认目录下所有 .bak，
 *      若这里也用 .bak，收藏页的备份列表会混进别人的备份。
 *   ④ **缓冲走 PSRAM**: 备份数据不属于热路径，绝不占内部 RAM。
 */
#include "svc_backup.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "os_file.h"     /* 原子替换 + 掉电恢复 (FatFs 的 rename 不覆盖已存在目标) */

static const char *TAG = "BKP";

/* ==================== 文件格式常量 ====================
 * "BKRP1"(5) | ver(1) | flags(1) | len(u32 LE) | crc32(u32 LE) | payload
 * flags 目前恒 0（曾计划加密位，已明确不做 —— 见头文件的决策说明）。 */
#define BKP_MAGIC      "BKRP1"
#define BKP_MAGIC_LEN  5
#define BKP_VER        1
#define BKP_HDR_LEN    (BKP_MAGIC_LEN + 1 + 1 + 4 + 4)   /* 15 */
#define BKP_EXT        ".bkp"
#define BKP_PATH_MAX   80

/* ==================== 策略表 ====================
 * 上限 6 条够用（设置/书签/列表/进度…），全放 PSRAM，不占内部 RAM。
 * 只存指针，指向调用方传入的静态字符串 —— 因此要求字面量或全局，
 * 不能传栈上的临时数组（会在退出后变野指针）。 */
#define BKP_MAX_POL 6
EXT_RAM_BSS_ATTR static bkp_policy_t s_pol[BKP_MAX_POL];
static int s_pol_n;
static const bkp_policy_t s_pol_default = { "", NULL, BKP_SLOT_DEFAULT };

/* ==================== 小工具 ==================== */
static void wr_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* id 合法性: 只允许 [a-z0-9_]，长度 1..BKP_ID_MAX-1
 * ⛔ 故意不允许 '.' 和 '/' —— 这一条就把路径穿越（"../../nvs"）堵死了，
 *    不需要再对拼好的路径做额外检查。 */
static bool bkp_id_ok(const char *id) {
    if (!id) return false;
    size_t n = strlen(id);
    if (n == 0 || n >= BKP_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

void *bkp_alloc(size_t n) { return heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM); }
void  bkp_free(void *p)   { if (p) heap_caps_free(p); }

/* ==================== 策略 ==================== */
esp_err_t bkp_policy_set(const bkp_policy_t *p) {
    if (!p || !bkp_id_ok(p->id)) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < s_pol_n; i++) {
        if (strcmp(s_pol[i].id, p->id) == 0) { s_pol[i] = *p; return ESP_OK; }
    }
    if (s_pol_n >= BKP_MAX_POL) {
        ESP_LOGE(TAG, "策略表已满 (%d), 拒绝注册 %s", BKP_MAX_POL, p->id);
        return ESP_ERR_NO_MEM;
    }
    s_pol[s_pol_n++] = *p;
    return ESP_OK;
}

const bkp_policy_t *bkp_policy_get(const char *id) {
    if (id) {
        for (int i = 0; i < s_pol_n; i++)
            if (strcmp(s_pol[i].id, id) == 0) return &s_pol[i];
    }
    return &s_pol_default;
}

static const char *bkp_dir_of(const char *id) {
    const bkp_policy_t *p = bkp_policy_get(id);
    return (p->dir && p->dir[0]) ? p->dir : BKP_DIR_DEFAULT;
}
static int bkp_slots_of(const char *id) {
    const bkp_policy_t *p = bkp_policy_get(id);
    int s = p->slots ? (int)p->slots : BKP_SLOT_DEFAULT;
    if (s < 1) s = 1;
    if (s > BKP_SLOT_MAX) s = BKP_SLOT_MAX;
    return s;
}

static void bkp_path(const char *id, int slot, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s_%02d" BKP_EXT, bkp_dir_of(id), id, slot);
}

/* ==================== 文件层 ==================== */
static void bkp_mkdir(const char *id) {
    /* 只建一层: 默认目录的父 (/sdcard) 由 SD 挂载保证存在;
     * 自定义多级目录由调用方自己先建好 —— 本模块不猜调用方的目录结构。 */
    mkdir(bkp_dir_of(id), 0777);
}

static bool bkp_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* 原子写: tmp → 校验长度 → rename
 * ★ 不先 remove 目标: FatFs 的 rename 在目标存在时直接覆盖;
 *   若先 remove 而 rename 失败, 旧备份就白丢了。 */
/* ★ 2026-11-22 (第4回合): 原来自己写 tmp → 校验长度 → `rename(tmp, path)`。
 *   缺陷: bkp_pick_slot() 在"份数已满"时返回**最旧的已存在槽位**, 而 FatFs 的
 *   f_rename 对已存在目标返回 FR_EXIST **不覆盖** (ff.c:5243-5245) ——
 *   于是"备份满 BKP_SLOT_MAX 份之后再备份永远失败", 且没有任何提示能看出原因。
 *   改用公共 os_file_write_atomic(): 旧档挪 .old → tmp 换名 → 成功才删 .old,
 *   覆盖成立, 掉电也不会把旧备份弄坏 (省掉本文件 25 行重复实现)。 */
static esp_err_t bkp_write_atomic(const char *path, const void *body, size_t len) {
    uint8_t hdr[BKP_HDR_LEN];
    memcpy(hdr, BKP_MAGIC, BKP_MAGIC_LEN);
    hdr[5] = BKP_VER;
    hdr[6] = 0;
    wr_u32le(hdr + 7, (uint32_t)len);
    wr_u32le(hdr + 11, esp_rom_crc32_le(0, (const uint8_t *)body, (uint32_t)len));

    int rc = os_file_write_atomic(path, hdr, BKP_HDR_LEN, body, len);
    if (rc != 0) {
        /* rc 语义见 os_file.h: -1 参数/路径过长, -2 打不开 tmp, -3 写入/长度校验失败,
         * -4 替换失败(旧档已回滚)。任何一种情况下**旧备份都保持原样**。 */
        ESP_LOGW(TAG, "备份写入失败 rc=%d, 旧备份未动: %s", rc, path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 读一份并做双校验。len 先从头里取，容量不够直接拒绝（不截断回灌）。 */
static esp_err_t bkp_read_file(const char *path, void *out, size_t cap, size_t *out_len) {
    /* 掉电恢复: 替换若断在"旧档已挪成 .old、新档未就位"之间, 正式 .bkp 会缺失,
     * 而 .old 是完整旧备份 —— 先复位再看, 否则这一份"凭空消失"。
     * 没有 .old 时只多一次 stat (= bkp_write_atomic 的配套动作)。 */
    (void)os_file_recover_auto(path);
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint8_t hdr[BKP_HDR_LEN];
    if (fread(hdr, 1, BKP_HDR_LEN, f) != BKP_HDR_LEN) { fclose(f); return ESP_FAIL; }
    if (memcmp(hdr, BKP_MAGIC, BKP_MAGIC_LEN) != 0) { fclose(f); return ESP_FAIL; }
    if (hdr[5] != BKP_VER) {
        ESP_LOGW(TAG, "备份版本 %u 不认识: %s", hdr[5], path);
        fclose(f);
        return ESP_FAIL;
    }
    uint32_t len = rd_u32le(hdr + 7);
    uint32_t crc = rd_u32le(hdr + 11);
    if (len > BKP_PAYLOAD_MAX) { fclose(f); return ESP_FAIL; }

    /* 长度必须与实际文件吻合（挡住"尾部被补零/被追加"） */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ESP_FAIL; }
    long fsz = ftell(f);
    if (fsz < 0 || (size_t)fsz != BKP_HDR_LEN + (size_t)len) {
        ESP_LOGW(TAG, "备份长度不符 (%ld vs %u): %s", fsz, (unsigned)(BKP_HDR_LEN + len), path);
        fclose(f);
        return ESP_FAIL;
    }
    if (fseek(f, BKP_HDR_LEN, SEEK_SET) != 0) { fclose(f); return ESP_FAIL; }

    if (len > cap) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    if (len && fread(out, 1, len, f) != len) { fclose(f); return ESP_FAIL; }
    fclose(f);

    if (len && esp_rom_crc32_le(0, (const uint8_t *)out, len) != crc) {
        ESP_LOGW(TAG, "备份校验失败(内容损坏): %s", path);
        return ESP_FAIL;
    }
    if (out_len) *out_len = len;
    return ESP_OK;
}

/* ==================== 槽位管理 ==================== */
typedef struct { int slot; uint32_t mtime; uint32_t size; } bkp_slot_t;

/* 收集现有槽位，按 mtime 降序（新→旧） */
static int bkp_collect(const char *id, bkp_slot_t *arr, int max) {
    int n = 0;
    int slots = bkp_slots_of(id);
    char p[BKP_PATH_MAX];
    for (int i = 1; i <= slots; i++) {
        bkp_path(id, i, p, sizeof(p));
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (n < max) {
            arr[n].slot  = i;
            arr[n].mtime = (uint32_t)st.st_mtime;
            arr[n].size  = (uint32_t)st.st_size;
            n++;
        }
    }
    /* 插入排序: 新→旧（份数 <= 16，不值得上 qsort） */
    for (int i = 1; i < n; i++) {
        bkp_slot_t k = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j].mtime < k.mtime) { arr[j + 1] = arr[j]; j--; }
        arr[j + 1] = k;
    }
    return n;
}

/* 选一个槽: 优先空槽；满了覆盖最旧的。
 * ★ 按 mtime 而不是文件名序号挑最旧 —— 序号是复用过的，
 *   最小编号未必是最旧内容，按 mtime 才真的是"最旧那份"。 */
static int bkp_pick_slot(const char *id) {
    int slots = bkp_slots_of(id);
    char p[BKP_PATH_MAX];
    for (int i = 1; i <= slots; i++) {
        bkp_path(id, i, p, sizeof(p));
        if (!bkp_exists(p)) return i;
    }
    bkp_slot_t arr[BKP_SLOT_MAX];
    int n = bkp_collect(id, arr, BKP_SLOT_MAX);
    if (n <= 0) return -1;                 /* 都存在却都 stat 不出来 ⇒ 目录坏了 */
    ESP_LOGW(TAG, "%s 备份已满 %d 份, 覆盖最旧的 _%02d", id, slots, arr[n - 1].slot);
    return arr[n - 1].slot;
}

/* ==================== 对外: 保存 / 读取 ==================== */
esp_err_t bkp_save(const char *id, const void *data, size_t len) {
    if (!bkp_id_ok(id) || (!data && len)) return ESP_ERR_INVALID_ARG;
    if (len > BKP_PAYLOAD_MAX) return ESP_ERR_INVALID_SIZE;

    bkp_mkdir(id);
    int slot = bkp_pick_slot(id);
    if (slot <= 0) {
        ESP_LOGE(TAG, "%s: 备份目录不可读, 拒绝写入", id);
        return ESP_FAIL;
    }
    char path[BKP_PATH_MAX];
    bkp_path(id, slot, path, sizeof(path));

    esp_err_t r = bkp_write_atomic(path, data, len);
    if (r == ESP_OK)
        ESP_LOGI(TAG, "已备份 %s: %u B → _%02d", id, (unsigned)len, slot);
    return r;
}

esp_err_t bkp_load(const char *id, int slot, void *out, size_t cap, size_t *out_len) {
    if (!bkp_id_ok(id) || !out) return ESP_ERR_INVALID_ARG;

    int want = slot;
    if (want <= 0) {                        /* 0 = 最新一份 */
        bkp_slot_t arr[BKP_SLOT_MAX];
        int n = bkp_collect(id, arr, BKP_SLOT_MAX);
        if (n <= 0) return ESP_ERR_NOT_FOUND;
        want = arr[0].slot;
    }
    char path[BKP_PATH_MAX];
    bkp_path(id, want, path, sizeof(path));
    return bkp_read_file(path, out, cap, out_len);
}

int bkp_list(const char *id, bkp_entry_t *out, int max) {
    if (!bkp_id_ok(id) || !out || max <= 0) return -1;
    bkp_slot_t arr[BKP_SLOT_MAX];
    int n = bkp_collect(id, arr, BKP_SLOT_MAX);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        out[i].slot  = arr[i].slot;
        out[i].mtime = arr[i].mtime;
        out[i].size  = arr[i].size;
        snprintf(out[i].name, sizeof(out[i].name), "%s_%02d" BKP_EXT, id, arr[i].slot);
    }
    return n;
}

bool bkp_has_any(const char *id) {
    if (!bkp_id_ok(id)) return false;
    bkp_slot_t arr[BKP_SLOT_MAX];
    return bkp_collect(id, arr, BKP_SLOT_MAX) > 0;
}

esp_err_t bkp_remove(const char *id, int slot) {
    if (!bkp_id_ok(id)) return ESP_ERR_INVALID_ARG;
    int want = slot;
    if (want <= 0) {
        bkp_slot_t arr[BKP_SLOT_MAX];
        int n = bkp_collect(id, arr, BKP_SLOT_MAX);
        if (n <= 0) return ESP_ERR_NOT_FOUND;
        want = arr[0].slot;
    }
    char path[BKP_PATH_MAX];
    bkp_path(id, want, path, sizeof(path));
    if (remove(path) != 0) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

/* ==================== 可选: NVS 命名空间整体备份 ====================
 * 布局: "BKPNVS1"(7) | count(u16 LE) | 条目*
 * 条目: type(u8) | klen(u8) | vlen(u16 LE) | key | value
 * 只做字节搬运，不解释键的业务含义 —— 这正是"策略服务"该有的边界。 */
#define NVS_TLV_MAGIC     "BKPNVS1"
#define NVS_TLV_MAGIC_LEN 7

enum {
    NVT_U8 = 1, NVT_I16, NVT_U16, NVT_I32, NVT_U32, NVT_I64, NVT_U64, NVT_STR, NVT_BLOB
};

/* 取一个键的类型化取值。dst==NULL 只测长度（用于第一遍统计）。
 * 返回 payload 字节数；<0 表示该键跳过（类型不支持/读失败/过大）。 */
static int nvs_one_payload(nvs_handle_t h, const nvs_entry_info_t *in,
                           uint8_t *dst, size_t cap) {
    switch (in->type) {
    case NVS_TYPE_U8:  { uint8_t v = 0; if (nvs_get_u8(h, in->key, &v) != ESP_OK) return -1;
                         if (dst) { if (cap < 1) return -1; dst[0] = v; } return 1; }
    case NVS_TYPE_I16: { int16_t v = 0; if (nvs_get_i16(h, in->key, &v) != ESP_OK) return -1;
                         if (dst) { if (cap < 2) return -1; memcpy(dst, &v, 2); } return 2; }
    case NVS_TYPE_U16: { uint16_t v = 0; if (nvs_get_u16(h, in->key, &v) != ESP_OK) return -1;
                         if (dst) { if (cap < 2) return -1; memcpy(dst, &v, 2); } return 2; }
    case NVS_TYPE_I32: { int32_t v = 0; if (nvs_get_i32(h, in->key, &v) != ESP_OK) return -1;
                         if (dst) { if (cap < 4) return -1; memcpy(dst, &v, 4); } return 4; }
    case NVS_TYPE_U32: { uint32_t v = 0; if (nvs_get_u32(h, in->key, &v) != ESP_OK) return -1;
                         if (dst) { if (cap < 4) return -1; memcpy(dst, &v, 4); } return 4; }
    case NVS_TYPE_I64: { int64_t v = 0; if (nvs_get_i64(h, in->key, &v) != ESP_OK) return -1;
                         if (dst) { if (cap < 8) return -1; memcpy(dst, &v, 8); } return 8; }
    case NVS_TYPE_U64: { uint64_t v = 0; if (nvs_get_u64(h, in->key, &v) != ESP_OK) return -1;
                         if (dst) { if (cap < 8) return -1; memcpy(dst, &v, 8); } return 8; }
    case NVS_TYPE_STR: {
        size_t l = 0;
        if (nvs_get_str(h, in->key, NULL, &l) != ESP_OK) return -1;
        if (l == 0 || l > 65535) return -1;
        if (dst) {
            if (cap < l) return -1;
            if (nvs_get_str(h, in->key, (char *)dst, &l) != ESP_OK) return -1;
        }
        return (int)l;
    }
    case NVS_TYPE_BLOB: {
        size_t l = 0;
        if (nvs_get_blob(h, in->key, NULL, &l) != ESP_OK) return -1;
        if (l > 65535) return -1;
        if (dst) {
            if (cap < l) return -1;
            if (l && nvs_get_blob(h, in->key, dst, &l) != ESP_OK) return -1;
        }
        return (int)l;
    }
    default: return -1;
    }
}

static uint8_t nvs_type_code(nvs_type_t t) {
    switch (t) {
    case NVS_TYPE_U8:  return NVT_U8;
    case NVS_TYPE_I16: return NVT_I16;
    case NVS_TYPE_U16: return NVT_U16;
    case NVS_TYPE_I32: return NVT_I32;
    case NVS_TYPE_U32: return NVT_U32;
    case NVS_TYPE_I64: return NVT_I64;
    case NVS_TYPE_U64: return NVT_U64;
    case NVS_TYPE_STR: return NVT_STR;
    case NVS_TYPE_BLOB:return NVT_BLOB;
    default: return 0;
    }
}

/* 遍历导出。buf==NULL 只统计（*used 累加总长），否则填充并推进 *used。 */
static esp_err_t nvs_export_walk(nvs_handle_t h, const char *ns,
                                 uint8_t *buf, size_t cap, size_t *used, uint16_t *count) {
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &it);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;    /* 空命名空间不算错 */
    if (err != ESP_OK) return err;

    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        uint8_t tc = nvs_type_code(info.type);
        size_t klen = strlen(info.key);
        int plen = (tc && klen <= 255) ? nvs_one_payload(h, &info, NULL, 0) : -1;

        if (plen >= 0) {
            size_t need = 1 + 1 + 2 + klen + (size_t)plen;
            if (buf) {
                if (*used + need > cap) return ESP_ERR_INVALID_SIZE;
                uint8_t *p = buf + *used;
                p[0] = tc;
                p[1] = (uint8_t)klen;
                p[2] = (uint8_t)(plen & 0xFF);
                p[3] = (uint8_t)((plen >> 8) & 0xFF);
                memcpy(p + 4, info.key, klen);
                if (nvs_one_payload(h, &info, p + 4 + klen, (size_t)plen) != plen)
                    return ESP_FAIL;
                if (count) (*count)++;
            } else if (count) {
                (*count)++;
            }
            *used += need;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return ESP_OK;
}

esp_err_t bkp_save_nvs(const char *id, const char *nvs_ns) {
    if (!bkp_id_ok(id) || !nvs_ns || !nvs_ns[0]) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    if (nvs_open(nvs_ns, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 命名空间 %s 打不开, 备份空内容", nvs_ns);
        return bkp_save(id, "", 0);
    }

    /* 第一遍: 统计总长 */
    size_t need = NVS_TLV_MAGIC_LEN + 2;
    uint16_t cnt = 0;
    esp_err_t err = nvs_export_walk(h, nvs_ns, NULL, 0, &need, &cnt);
    if (err != ESP_OK) { nvs_close(h); return err; }
    if (need > BKP_PAYLOAD_MAX) { nvs_close(h); return ESP_ERR_INVALID_SIZE; }

    /* 第二遍: 填充 */
    uint8_t *buf = bkp_alloc(need);
    if (!buf) { nvs_close(h); return ESP_ERR_NO_MEM; }
    size_t used = NVS_TLV_MAGIC_LEN + 2;
    memcpy(buf, NVS_TLV_MAGIC, NVS_TLV_MAGIC_LEN);
    buf[7] = (uint8_t)(cnt & 0xFF);
    buf[8] = (uint8_t)(cnt >> 8);
    cnt = 0;
    err = nvs_export_walk(h, nvs_ns, buf, need, &used, &cnt);
    nvs_close(h);
    if (err != ESP_OK) { bkp_free(buf); return err; }

    esp_err_t r = bkp_save(id, buf, used);
    bkp_free(buf);
    if (r == ESP_OK) ESP_LOGI(TAG, "已备份 NVS[%s] %u 个键, %u B", nvs_ns, (unsigned)cnt, (unsigned)used);
    return r;
}

esp_err_t bkp_load_nvs(const char *id, int slot, const char *nvs_ns) {
    if (!bkp_id_ok(id) || !nvs_ns || !nvs_ns[0]) return ESP_ERR_INVALID_ARG;

    /* 先探长度: 用最大容量一次读入（真实备份都是几十 KB 级） */
    size_t cap = 64 * 1024;
    uint8_t *buf = bkp_alloc(cap);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t len = 0;
    esp_err_t err = bkp_load(id, slot, buf, cap, &len);
    if (err != ESP_OK) {
        /* 缓冲不够就把头里的长度取出来重试一次 */
        if (err == ESP_ERR_INVALID_SIZE) {
            bkp_free(buf);
            cap = BKP_PAYLOAD_MAX;
            buf = bkp_alloc(cap);
            if (!buf) return ESP_ERR_NO_MEM;
            err = bkp_load(id, slot, buf, cap, &len);
        }
        if (err != ESP_OK) { bkp_free(buf); return err; }
    }
    if (len < NVS_TLV_MAGIC_LEN + 2 || memcmp(buf, NVS_TLV_MAGIC, NVS_TLV_MAGIC_LEN) != 0) {
        bkp_free(buf);
        ESP_LOGE(TAG, "%s 不是 NVS 备份包", id);
        return ESP_FAIL;
    }

    nvs_handle_t h;
    if (nvs_open(nvs_ns, NVS_READWRITE, &h) != ESP_OK) { bkp_free(buf); return ESP_FAIL; }

    uint16_t cnt = (uint16_t)buf[7] | ((uint16_t)buf[8] << 8);
    size_t pos = NVS_TLV_MAGIC_LEN + 2;
    int done = 0;
    for (uint16_t i = 0; i < cnt; i++) {
        if (pos + 4 > len) { err = ESP_FAIL; break; }
        uint8_t  tc   = buf[pos];
        size_t   klen = buf[pos + 1];
        size_t   vlen = (size_t)buf[pos + 2] | ((size_t)buf[pos + 3] << 8);
        pos += 4;
        if (pos + klen + vlen > len) { err = ESP_FAIL; break; }
        char key[16];
        if (klen == 0 || klen >= sizeof(key)) { err = ESP_FAIL; break; }
        memcpy(key, buf + pos, klen);
        key[klen] = 0;
        const uint8_t *v = buf + pos + klen;
        pos += klen + vlen;

        esp_err_t se = ESP_FAIL;
        switch (tc) {
        case NVT_U8:  se = nvs_set_u8(h, key, v[0]); break;
        case NVT_I16: { int16_t x; memcpy(&x, v, 2); se = nvs_set_i16(h, key, x); break; }
        case NVT_U16: { uint16_t x; memcpy(&x, v, 2); se = nvs_set_u16(h, key, x); break; }
        case NVT_I32: { int32_t x; memcpy(&x, v, 4); se = nvs_set_i32(h, key, x); break; }
        case NVT_U32: { uint32_t x; memcpy(&x, v, 4); se = nvs_set_u32(h, key, x); break; }
        case NVT_I64: { int64_t x; memcpy(&x, v, 8); se = nvs_set_i64(h, key, x); break; }
        case NVT_U64: { uint64_t x; memcpy(&x, v, 8); se = nvs_set_u64(h, key, x); break; }
        case NVT_STR:  se = nvs_set_str(h, key, (const char *)v); break;
        case NVT_BLOB: se = nvs_set_blob(h, key, v, vlen); break;
        default: break;
        }
        if (se == ESP_OK) done++;
        else ESP_LOGW(TAG, "恢复键 %s 失败 (%s)", key, esp_err_to_name(se));
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    bkp_free(buf);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "已恢复 NVS[%s] %d/%u 个键", nvs_ns, done, (unsigned)cnt);
    return err;
}
