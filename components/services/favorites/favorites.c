/* favorites.c - 游戏收藏管理 (按引擎独立, appdata 分区持久化).
 *
 * 存储位置: appdata 分区 "os_db/系统配置" 预留区 (offset 0x8A0000, 128KB).
 *   不再读写 TF 卡 (/sdcard/system/fav_*.txt 已废弃).
 * 格式: "FAVB"+ver 头, 之后每引擎依次写: 1字节数量 + 若干条 null 结尾路径.
 *
 * 用户需求:
 * - 每个游戏引擎的收藏独立
 * - NES 与 FC 使用相同 .nes 文件, 合并为一个引擎 (FC)
 * - 所有配置信息存放到 appdata (Flash), 不读 TF 卡
 */
#include "favorites.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "svc_nvs.h"
#include "esp_partition.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define TAG "FAV"

/* [2026-09-16] 收藏已并入 NVS(数据库)统一存储, 不再写 appdata */
#define FAV_NVS_NS   "os_fav"
#define FAV_NVS_KEY  "data"
/* [归整] 收藏=纯 Blob(FAVB+ver+各引擎路径), 保持 svc_nvs 直写; 不并入账号收藏(linfav) */
#define FAV_BLOB_MAX (8 * 1024)      /* 序列化上限 */

/* 各引擎独立存储: 路径二维数组 + 数量 + 指针数组 (与原版一致) */
EXT_RAM_BSS_ATTR static char s_paths[FAV_ENGINE_MAX][FAVORITES_MAX][FAVORITES_PATH_MAX];
static int s_count[FAV_ENGINE_MAX];
static bool s_loaded_cache = false;   /* [2026-09-16] NVS 已加载标记 */
EXT_RAM_BSS_ATTR static const char *s_path_ptrs[FAV_ENGINE_MAX][FAVORITES_MAX];



fav_engine_t favorites_engine_for_path(const char *path) {
    if (!path) return FAV_ENGINE_BBK;
    if (strncmp(path, "/sdcard/gbc/", 12) == 0) return FAV_ENGINE_GBC;
    if (strncmp(path, "/sdcard/gb/", 11) == 0)  return FAV_ENGINE_GB;
    if (strncmp(path, "/sdcard/nes/", 12) == 0) return FAV_ENGINE_FC;  /* NES 合并到 FC */
    if (strncmp(path, "/sdcard/AB/", 11) == 0)  return FAV_ENGINE_AB;
    if (strncmp(path, "/sdcard/books/", 13) == 0) return FAV_ENGINE_BOOK;
    if (strncmp(path, "/sdcard/lava/", 14) == 0)  return FAV_ENGINE_WQX;
    if (strncmp(path, "/sdcard/vpet/", 13) == 0)     return FAV_ENGINE_VPET;
    if (strncmp(path, "/sdcard/md/", 11) == 0)       return FAV_ENGINE_MD;    /* MD/Genesis */
    if (strncmp(path, "/sdcard/sms/", 12) == 0)      return FAV_ENGINE_SMS;   /* SMS/GG */
    return FAV_ENGINE_BBK;
}

/* 将全部引擎的收藏序列化后写入 NVS(数据库), 不再写 appdata */
static void save_all(void) {
    uint8_t *buf = heap_caps_malloc(FAV_BLOB_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGE(TAG, "无内存写收藏"); return; }
    memcpy(buf, "FAVB", 4);
    buf[4] = 1;
    uint32_t off = 5;
    for (int e = 0; e < FAV_ENGINE_MAX; e++) {
        if (off + 1 > FAV_BLOB_MAX) break;
        buf[off++] = (uint8_t)s_count[e];
        for (int i = 0; i < s_count[e] && off + 1 <= FAV_BLOB_MAX; i++) {
            size_t len = strlen(s_paths[e][i]) + 1;
            if (off + len > FAV_BLOB_MAX) break;
            memcpy(buf + off, s_paths[e][i], len);
            off += len;
        }
    }
    nvs_handle_t h;
    if (svc_nvs_open(FAV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t er = svc_nvs_set_blob(h, FAV_NVS_KEY, buf, off);
        esp_err_t ec = svc_nvs_commit(h);
        svc_nvs_close(h);
        if (er == ESP_OK && ec == ESP_OK) ESP_LOGI(TAG, "收藏已写入 NVS (+%uB)", (unsigned)off);
        else ESP_LOGE(TAG, "收藏写 NVS 失败");
    }
    heap_caps_free(buf);
}

/* 从 appdata 分区读取收藏 */
static void load_all(void) {
    for (int e = 0; e < FAV_ENGINE_MAX; e++) {
        s_count[e] = 0;
        for (int i = 0; i < FAVORITES_MAX; i++) s_path_ptrs[e][i] = NULL;
    }
    if (s_loaded_cache) return;   /* 已加载过, 避免重复 */
    uint8_t *buf = heap_caps_malloc(FAV_BLOB_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    nvs_handle_t h = 0;
    size_t blen = FAV_BLOB_MAX;
    esp_err_t er;
    if (svc_nvs_open(FAV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        er = svc_nvs_get_blob(h, FAV_NVS_KEY, buf, &blen);
        svc_nvs_close(h);
    } else { er = ESP_ERR_NOT_FOUND; }
    if (er != ESP_OK || blen < 5 || memcmp(buf, "FAVB", 4) != 0) {
        heap_caps_free(buf);
        ESP_LOGI(TAG, "NVS 无有效收藏 (首次运行), 使用空列表");
        s_loaded_cache = true;
        return;
    }
    uint32_t off = 5;   /* 跳过 magic + version */
    for (int e = 0; e < FAV_ENGINE_MAX; e++) {
        int cnt = buf[off++];
        if (cnt > FAVORITES_MAX) cnt = FAVORITES_MAX;
        for (int i = 0; i < cnt && off < blen; i++) {
            const char *p = (const char *)(buf + off);
            size_t len = strnlen(p, blen - off);
            if (len == 0) { off++; continue; }
            if (len >= FAVORITES_PATH_MAX) len = FAVORITES_PATH_MAX - 1;
            memcpy(s_paths[e][s_count[e]], p, len);
            s_paths[e][s_count[e]][len] = '\0';
            s_path_ptrs[e][s_count[e]] = s_paths[e][s_count[e]];
            s_count[e]++;
            off += len + 1;
        }
    }
    heap_caps_free(buf);
    s_loaded_cache = true;
    ESP_LOGI(TAG, "已从 NVS 加载收藏");
}

void favorites_init(void) {
    load_all();
}

bool favorites_contains(fav_engine_t e, const char *path) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return false;
    if (!path || path[0] == '\0') return false;
    for (int i = 0; i < s_count[e]; i++)
        if (strcmp(s_paths[e][i], path) == 0) return true;
    return false;
}

bool favorites_toggle(fav_engine_t e, const char *path) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return false;
    if (!path || path[0] == '\0') { ESP_LOGW(TAG, "拒绝空路径"); return false; }
    int found = -1;
    for (int i = 0; i < s_count[e]; i++)
        if (strcmp(s_paths[e][i], path) == 0) { found = i; break; }
    if (found >= 0) {
        for (int i = found; i < s_count[e] - 1; i++) {
            strncpy(s_paths[e][i], s_paths[e][i+1], FAVORITES_PATH_MAX);
            s_path_ptrs[e][i] = s_paths[e][i];
        }
        s_count[e]--;
        s_path_ptrs[e][s_count[e]] = NULL;
        save_all();
        ESP_LOGI(TAG, "已取消收藏: %s (剩余 %d)", path, s_count[e]);
        return false;
    }
    if (s_count[e] >= FAVORITES_MAX) { ESP_LOGW(TAG, "收藏已满 (%d)", FAVORITES_MAX); return false; }
    strncpy(s_paths[e][s_count[e]], path, FAVORITES_PATH_MAX - 1);
    s_paths[e][s_count[e]][FAVORITES_PATH_MAX - 1] = '\0';
    s_path_ptrs[e][s_count[e]] = s_paths[e][s_count[e]];
    s_count[e]++;
    save_all();
    ESP_LOGI(TAG, "已收藏: %s (共 %d)", path, s_count[e]);
    return true;
}

bool favorites_add(fav_engine_t e, const char *path) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return false;
    if (!path || path[0] == '\0') { ESP_LOGW(TAG, "拒绝空路径"); return false; }
    if (favorites_contains(e, path)) { ESP_LOGW(TAG, "已在列表中: %s", path); return false; }
    if (s_count[e] >= FAVORITES_MAX) { ESP_LOGW(TAG, "已满 (%d)", FAVORITES_MAX); return false; }
    size_t plen = strnlen(path, FAVORITES_PATH_MAX - 1);
    memcpy(s_paths[e][s_count[e]], path, plen);
    s_paths[e][s_count[e]][plen] = '\0';
    s_path_ptrs[e][s_count[e]] = s_paths[e][s_count[e]];
    s_count[e]++;
    save_all();
    return true;
}

void favorites_remove(fav_engine_t e, const char *path) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return;
    int found = -1;
    for (int i = 0; i < s_count[e]; i++)
        if (strcmp(s_paths[e][i], path) == 0) { found = i; break; }
    if (found < 0) return;
    for (int i = found; i < s_count[e] - 1; i++) {
        strncpy(s_paths[e][i], s_paths[e][i+1], FAVORITES_PATH_MAX);
        s_path_ptrs[e][i] = s_paths[e][i];
    }
    s_count[e]--;
    s_path_ptrs[e][s_count[e]] = NULL;
    save_all();
}

const char *const *favorites_list(fav_engine_t e, int *count) {
    if (e < 0 || e >= FAV_ENGINE_MAX) { if (count) *count = 0; return NULL; }
    if (count) *count = s_count[e];
    return s_path_ptrs[e];
}

int favorites_count(fav_engine_t e) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return 0;
    return s_count[e];
}

/* 清理所有引擎中"文件已不存在"的失效收藏 (打开收藏栏时调用) */
void favorites_prune_missing(void) {
    for (int e = 0; e < FAV_ENGINE_MAX; e++) {
        for (int i = s_count[e] - 1; i >= 0; i--) {
            struct stat st;
            if (stat(s_paths[e][i], &st) != 0) {
                ESP_LOGW(TAG, "清理失效收藏: %s", s_paths[e][i]);
                favorites_remove((fav_engine_t)e, s_paths[e][i]);
            }
        }
    }
}