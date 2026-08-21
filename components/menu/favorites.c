/* favorites.c - 收藏管理实现 (按引擎独立, TF 卡文件持久化)
 *
 * 存储位置: /sdcard/system/fav_*.txt (每个引擎一个独立文件, 互不干扰)
 *   - 电子词典: /sdcard/system/fav_bbk.txt
 *   - GB:       /sdcard/system/fav_gb.txt
 *   - GBC:      /sdcard/system/fav_gbc.txt
 *   - NES:      /sdcard/system/fav_nes.txt   (共用 .nes)
 *   - arduboy:  /sdcard/system/fav_ab.txt
 * 格式: 每行一个游戏路径, UTF-8, 路径中不含 '\n' 假设
 *
 * 用户需求:
 * - 每个游戏引擎的收藏独立存储 (分成不同的 txt)
 * - NES 与 FC 使用相同 .nes 文件, 合并为一个引擎 (FC)
 */
#include "favorites.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#define TAG "FAV"
/* 各引擎独立持久化文件 */
static const char *const s_files[FAV_ENGINE_MAX] = {
    "/sdcard/system/fav_bbk.txt",   /* FAV_ENGINE_BBK */
    "/sdcard/system/fav_gb.txt",    /* FAV_ENGINE_GB  */
    "/sdcard/system/fav_gbc.txt",   /* FAV_ENGINE_GBC */
    "/sdcard/system/fav_nes.txt",   /* FAV_ENGINE_FC  (NES) */
    "/sdcard/system/fav_ab.txt",    /* FAV_ENGINE_AB  */
    "/sdcard/system/fav_book.txt",  /* FAV_ENGINE_BOOK (电子书) */
    "/sdcard/system/fav_wqx.txt",   /* FAV_ENGINE_WQX (文曲星) */
    "/sdcard/system/fav_vpet.txt",  /* FAV_ENGINE_VPET (暴龙机) */
};

/* 各引擎独立存储: 路径二维数组 + 数量 + 指针数组.
 * 路径访问频率低 (仅进收藏栏时读), PSRAM 无性能影响. */
EXT_RAM_BSS_ATTR static char s_paths[FAV_ENGINE_MAX][FAVORITES_MAX][FAVORITES_PATH_MAX];
static int s_count[FAV_ENGINE_MAX];
/* 指针数组: 每个元素指向 s_paths[e][i] 对应行的起始地址.
 * 原因: 直接返回 (const char **)s_paths 是 UB — 2D 数组的内存布局
 *       (连续 160 字节/行) 不等同于指针数组的布局 (每行 4 字节指针).
 *       使用单独的指针数组可避免这个问题, 同时 favorites_list() 返回的
 *       指针就是 s_paths[e][i] 的稳定地址, 不会被后续 add/remove 影响.
 * V1.0.96: 移到 PSRAM (仅在收藏栏渲染/遍历时用, 访问频率极低, 省 1.5KB 内部 RAM). */
EXT_RAM_BSS_ATTR static const char *s_path_ptrs[FAV_ENGINE_MAX][FAVORITES_MAX];

/* 确保 /sdcard/system 目录存在 (若失败静默, 后续 save 也会失败) */
static void ensure_dir(void) {
    struct stat st;
    if (stat("/sdcard/system", &st) == 0) return;
    mkdir("/sdcard/system", 0755);
}

fav_engine_t favorites_engine_for_path(const char *path) {
    if (!path) return FAV_ENGINE_BBK;
    /* 注意: 先匹配较长前缀, 避免 /sdcard/gb 误匹配到 /sdcard/gbc */
    if (strncmp(path, "/sdcard/gbc/", 12) == 0) return FAV_ENGINE_GBC;
    if (strncmp(path, "/sdcard/gb/", 11) == 0)  return FAV_ENGINE_GB;
    if (strncmp(path, "/sdcard/nes/", 12) == 0) return FAV_ENGINE_FC;  /* NES 合并到 FC */
    if (strncmp(path, "/sdcard/AB/", 11) == 0)  return FAV_ENGINE_AB;
    if (strncmp(path, "/sdcard/books/", 13) == 0) return FAV_ENGINE_BOOK;
    if (strncmp(path, "/sdcard/lavaXOS/", 17) == 0)  return FAV_ENGINE_WQX;  /* wqx 文曲星 */
    if (strncmp(path, "/sdcard/vpet/", 13) == 0)     return FAV_ENGINE_VPET; /* 暴龙机 */
    return FAV_ENGINE_BBK;
}

/* 将指定引擎的 s_paths[e] 序列化到 TF 卡文件 (每行一个路径) */
static int save_to_sd(fav_engine_t e) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return -1;
    ensure_dir();
    FILE *f = fopen(s_files[e], "wb");
    if (!f) {
        ESP_LOGE(TAG, "打开 %s 写入失败 (errno=%d)", s_files[e], errno);
        return -1;
    }
    for (int i = 0; i < s_count[e]; i++) {
        fprintf(f, "%s\n", s_paths[e][i]);
    }
    fclose(f);
    ESP_LOGI(TAG, "已保存 %d 个收藏到 %s", s_count[e], s_files[e]);
    return 0;
}

/* 从 TF 卡加载指定引擎的收藏 */
static void load_from_sd(fav_engine_t e) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return;
    s_count[e] = 0;
    /* 同步清空指针数组, 避免残留指向已删除行的地址 */
    for (int i = 0; i < FAVORITES_MAX; i++) s_path_ptrs[e][i] = NULL;
    FILE *f = fopen(s_files[e], "r");
    if (!f) {
        /* 文件不存在是正常的 (第一次启动 / 用户清空) */
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "打开 %s 失败 (errno=%d), 使用空列表", s_files[e], errno);
        }
        return;
    }
    char line[FAVORITES_PATH_MAX];
    while (s_count[e] < FAVORITES_MAX && fgets(line, sizeof(line), f)) {
        /* 去掉行尾换行符 */
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) {
            line[--l] = '\0';
        }
        /* 跳过空行 */
        bool valid = false;
        for (const char *p = line; *p; p++) {
            if (*p != ' ' && *p != '\t') { valid = true; break; }
        }
        if (!valid) continue;
        strncpy(s_paths[e][s_count[e]], line, FAVORITES_PATH_MAX - 1);
        s_paths[e][s_count[e]][FAVORITES_PATH_MAX - 1] = '\0';
        /* 关键: 维护指针数组, 避免 (const char **)s_paths 的 UB */
        s_path_ptrs[e][s_count[e]] = s_paths[e][s_count[e]];
        s_count[e]++;
    }
    fclose(f);
}

void favorites_init(void) {
    for (int e = 0; e < FAV_ENGINE_MAX; e++) {
        s_count[e] = 0;
        load_from_sd((fav_engine_t)e);
        ESP_LOGI(TAG, "已加载 %d 个收藏 (从 %s)", s_count[e], s_files[e]);
        for (int i = 0; i < s_count[e]; i++) {
            ESP_LOGI(TAG, "  [%d] favorites[%d]='%s'", e, i, s_paths[e][i]);
        }
    }
}

bool favorites_contains(fav_engine_t e, const char *path) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return false;
    if (!path || path[0] == '\0') return false;
    for (int i = 0; i < s_count[e]; i++) {
        if (strcmp(s_paths[e][i], path) == 0) return true;
    }
    return false;
}

bool favorites_toggle(fav_engine_t e, const char *path) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return false;
    if (!path || path[0] == '\0') {
        ESP_LOGW(TAG, "favorites_toggle 拒绝空路径");
        return false;
    }
    int found = -1;
    for (int i = 0; i < s_count[e]; i++) {
        if (strcmp(s_paths[e][i], path) == 0) { found = i; break; }
    }
    if (found >= 0) {
        /* 移除: 后续条目前移, 同步前移指针数组 */
        for (int i = found; i < s_count[e] - 1; i++) {
            strncpy(s_paths[e][i], s_paths[e][i+1], FAVORITES_PATH_MAX);
            s_path_ptrs[e][i] = s_paths[e][i];
        }
        s_count[e]--;
        s_path_ptrs[e][s_count[e]] = NULL;  /* 释放最后一格的指针, 防止越界访问 */
        save_to_sd(e);
        ESP_LOGI(TAG, "已取消收藏: %s (剩余 %d)", path, s_count[e]);
        return false;
    }
    if (s_count[e] >= FAVORITES_MAX) {
        ESP_LOGW(TAG, "收藏已满 (%d), 无法添加", FAVORITES_MAX);
        return false;
    }
    strncpy(s_paths[e][s_count[e]], path, FAVORITES_PATH_MAX - 1);
    s_paths[e][s_count[e]][FAVORITES_PATH_MAX - 1] = '\0';
    /* 关键: 维护指针数组, 避免 (const char **)s_paths 的 UB */
    s_path_ptrs[e][s_count[e]] = s_paths[e][s_count[e]];
    s_count[e]++;
    save_to_sd(e);
    ESP_LOGI(TAG, "已收藏: %s (共 %d)", path, s_count[e]);
    return true;
}

bool favorites_add(fav_engine_t e, const char *path) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return false;
    if (!path || path[0] == '\0') {
        ESP_LOGW(TAG, "favorites_add 拒绝空路径");
        return false;
    }
    if (favorites_contains(e, path)) {
        ESP_LOGW(TAG, "favorites_add: 路径已在列表中: %s", path);
        return false;
    }
    if (s_count[e] >= FAVORITES_MAX) {
        ESP_LOGW(TAG, "favorites_add: 已满 (%d), 拒绝: %s", FAVORITES_MAX, path);
        return false;
    }
    /* 写入路径, 确保 NUL 结尾 */
    size_t plen = strnlen(path, FAVORITES_PATH_MAX - 1);
    memcpy(s_paths[e][s_count[e]], path, plen);
    s_paths[e][s_count[e]][plen] = '\0';
    /* 关键: 维护指针数组, 避免 (const char **)s_paths 的 UB */
    s_path_ptrs[e][s_count[e]] = s_paths[e][s_count[e]];
    s_count[e]++;
    int rc = save_to_sd(e);
    ESP_LOGW(TAG, "favorites_add: 成功 %s (s_count=%d, save_to_sd rc=%d)", path, s_count[e], rc);
    return true;
}

void favorites_remove(fav_engine_t e, const char *path) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return;
    int found = -1;
    for (int i = 0; i < s_count[e]; i++) {
        if (strcmp(s_paths[e][i], path) == 0) { found = i; break; }
    }
    if (found < 0) return;
    /* 后续条目前移, 同步前移指针数组 */
    for (int i = found; i < s_count[e] - 1; i++) {
        strncpy(s_paths[e][i], s_paths[e][i+1], FAVORITES_PATH_MAX);
        s_path_ptrs[e][i] = s_paths[e][i];
    }
    s_count[e]--;
    s_path_ptrs[e][s_count[e]] = NULL;  /* 释放最后一格的指针, 防止越界访问 */
    save_to_sd(e);
}

const char *const *favorites_list(fav_engine_t e, int *count) {
    if (e < 0 || e >= FAV_ENGINE_MAX) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = s_count[e];
    /* 返回指针数组 (s_path_ptrs[e]), 而非 s_paths 的 2D 数组 (后者按行布局, 不是指针布局) */
    return s_path_ptrs[e];
}

int favorites_count(fav_engine_t e) {
    if (e < 0 || e >= FAV_ENGINE_MAX) return 0;
    return s_count[e];
}

/* V1.0.9x: 清理所有引擎中"文件已不存在"(被改名/删除)的失效收藏.
 * 打开收藏栏时调用. 向后遍历, 因为 favorites_remove 是前移式删除, 低位索引不受影响. */
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
