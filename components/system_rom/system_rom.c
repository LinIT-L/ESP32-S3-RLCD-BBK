/**
 * @file system_rom.c
 * @brief 从 flash system 分区读取 8.BIN/E.BIN 写到 SD 卡 /system/gam4980/
 *
 * 集成: 把 4988.font + 0E00.DAT 合并成 system.bin (4MB) 烧录到 system 分区
 *       启动时如果 SD 卡上 /system/gam4980/8.BIN 和 E.BIN 缺失或大小不对,
 *       就从 flash 读出来写到 SD 卡.
 *
 * 使用 4KB 分块读写, 避免分配大 buffer.
 */
#include "system_rom.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "sdmmc_cmd.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

static const char *TAG = "SYSTEM_ROM";

#define CHUNK_SIZE 4096

/* 查找 system 分区 (subtype 0x40 = user-defined raw; 0x90 在 IDF5.5 被 tee_ota 占用) */
static const esp_partition_t *find_system_partition(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    (esp_partition_subtype_t)0x40, "system");
}

/* 检查文件是否已存在且大小正确 */
static bool file_ok(const char *path, size_t expected) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return (size_t)st.st_size == expected;
}

/* 分块从 flash 读到 SD 卡文件 */
static int write_rom_chunked(const esp_partition_t *part, size_t offset,
                             size_t total, const char *path) {
    uint8_t *buf = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DMA);
    if (!buf) buf = malloc(CHUNK_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "分配 %d buffer 失败", CHUNK_SIZE);
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "创建 %s 失败 (errno=%d)", path, errno);
        free(buf);
        return -1;
    }

    size_t written = 0;
    while (written < total) {
        size_t len = (total - written < CHUNK_SIZE) ? (total - written) : CHUNK_SIZE;
        if (esp_partition_read(part, offset + written, buf, len) != ESP_OK) {
            ESP_LOGE(TAG, "读取 flash 偏移 %u 失败", (unsigned)(offset + written));
            fclose(f);
            free(buf);
            return -1;
        }
        size_t n = fwrite(buf, 1, len, f);
        if (n != len) {
            ESP_LOGE(TAG, "写入 %s 不完整: %u/%u", path, (unsigned)n, (unsigned)len);
            fclose(f);
            free(buf);
            return -1;
        }
        written += len;
    }

    fclose(f);
    free(buf);
    return 0;
}

int system_rom_install_to_sd(void) {
    const esp_partition_t *part = find_system_partition();
    if (!part) {
        ESP_LOGW(TAG, "未找到 system 分区, 跳过");
        return -1;
    }
    ESP_LOGI(TAG, "找到 system 分区: address=0x%lx size=%lu KB",
             (unsigned long)part->address, (unsigned long)part->size / 1024);

    if (part->size < 4 * 1024 * 1024) {
        ESP_LOGE(TAG, "system 分区太小 (%lu), 至少要 4MB", (unsigned long)part->size);
        return -1;
    }

    /* 检查/创建目录 */
    struct stat st;
    if (stat("/sdcard/system", &st) != 0) {
        ESP_LOGI(TAG, "创建 /sdcard/system");
        if (mkdir("/sdcard/system", 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir /sdcard/system 失败: errno=%d", errno);
            return -1;
        }
    }
    if (stat("/sdcard/system/gam4980", &st) != 0) {
        ESP_LOGI(TAG, "创建 /sdcard/system/gam4980");
        if (mkdir("/sdcard/system/gam4980", 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir /sdcard/system/gam4980 失败: errno=%d", errno);
            return -1;
        }
    }

    const char *p_8bin = "/sdcard/system/gam4980/8.BIN";
    const char *p_ebin = "/sdcard/system/gam4980/E.BIN";
    const size_t half = 2 * 1024 * 1024;

    /* 检查文件是否存在且大小正确 */
    bool need_8bin = !file_ok(p_8bin, half);
    bool need_ebin = !file_ok(p_ebin, half);

    /* 额外检查: 文件大小对但内容可能是垃圾 (全 0xFF), 读首字节验证 */
    if (!need_8bin) {
        FILE *f = fopen(p_8bin, "rb");
        if (f) { uint8_t b; if (fread(&b, 1, 1, f) != 1 || b == 0xff) need_8bin = true; fclose(f); }
    }
    if (!need_ebin) {
        FILE *f = fopen(p_ebin, "rb");
        if (f) { uint8_t b; if (fread(&b, 1, 1, f) != 1 || b == 0xff) need_ebin = true; fclose(f); }
    }

    if (!need_8bin && !need_ebin) {
        ESP_LOGI(TAG, "8.BIN 和 E.BIN 已存在 (各 2MB), 跳过写入");
        return 0;
    }
    ESP_LOGI(TAG, "需要写入: 8.BIN=%s E.BIN=%s",
             need_8bin ? "YES" : "no", need_ebin ? "YES" : "no");

    if (need_8bin) {
        ESP_LOGI(TAG, "写入 8.BIN (分块 4KB)...");
        if (write_rom_chunked(part, 0, half, p_8bin) != 0) {
            ESP_LOGE(TAG, "8.BIN 写入失败");
            return -1;
        }
        ESP_LOGI(TAG, "8.BIN 写入成功");
    }

    if (need_ebin) {
        ESP_LOGI(TAG, "写入 E.BIN (分块 4KB)...");
        if (write_rom_chunked(part, half, half, p_ebin) != 0) {
            ESP_LOGE(TAG, "E.BIN 写入失败");
            return -1;
        }
        ESP_LOGI(TAG, "E.BIN 写入成功");
    }

    return 0;
}
