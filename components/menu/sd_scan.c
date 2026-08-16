/**
 * @file sd_scan.c
 * @brief TF/SD 卡挂载 + 扫描 BBK 游戏目录
 *
 * 挂载 TF 卡到 /sdcard, 然后扫描 /sdcard/BBK/ 目录下的所有 .gam 文件.
 * 调用方通过 scan_bbk_games() 获取去后缀的文件名列表.
 */
#include "sd_scan.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SDSCAN";

/* 过滤 macOS Finder 生成的 AppleDouble 元数据文件 (._开头) 和 .DS_Store
 * 从 Mac 复制文件到 SD 卡时, 每个文件会配套生成一个同名 ._xxxx 元数据文件,
 * 占用列表空间且不可作为游戏/媒体加载, 一律在 readdir 循环开头跳过 */
#define IS_APPLE_METANAME(n)  ((n)[0] == '.' && ((n)[1] == '_' || ((n)[1] == 'D' && strcmp((n), ".DS_Store") == 0)))

/* 前向声明 - sd_full_unmount() 在 sd_full_remount() 定义之前调用 */
static void sd_full_unmount(void);
static int  sd_full_remount(uint8_t format_if_needed);

/* SD 卡 SDMMC 引脚 - 来自 ESP32-S3-RLCD-4.2 官方原理图 + 官方 Arduino 示例
 * 重要: SD 卡实际接到 ESP32-S3 的 SDMMC 控制器, 不是 SPI 控制器!
 * 之前误用 SPI 模式是错的, GPIO 17 不是 CS, 是 CD (卡检测) 等其他用途
 *
 * 硬件接法 (SD 卡座 SD1, 1-bit SD 模式):
 *   CMD  - GPIO21
 *   CLK  - GPIO38
 *   D0   - GPIO39
 *   D1/D2/D3 - NC (1-bit 模式不用)
 *   CD   - GPIO17 (卡检测, 可选, 这里不用)
 */
#define SD_CMD  GPIO_NUM_21
#define SD_CLK  GPIO_NUM_38
#define SD_D0   GPIO_NUM_39
/* CD (卡检测) 在 GPIO17, 暂不用 (ESP-IDF SDMMC 也不强制) */

static bool s_mounted = false;
static char s_mount_point[16] = "/sdcard";
/* 保存 sdmmc_card_t 句柄, 让 USB MSC 可以直接读写扇区 */
static sdmmc_card_t *s_card = NULL;

/* 挂载 SD 卡 (SDMMC 1-bit 模式, ESP32-S3 专用 SDMMC 控制器)
 *
 * 重要发现: Waveshare 官方 Arduino 示例代码用的是 SDMMC 模式!
 *   CustomSDPort(const char *SdName, int clk = 38, int cmd = 21, int d0 = 39, int width = 1);
 *
 * SDMMC 是 ESP32-S3 内置的 SD 卡专用硬件控制器, 比 SPI 快得多 (最高 40MHz)
 * 之前一直用 SPI 模式, 引脚虽然是 21/38/39 但走 SPI 协议卡根本不应答
 */
esp_err_t sd_mount(void) {
    if (s_mounted) {
        ESP_LOGI(TAG, "已挂载, 跳过");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=== sd_mount 开始 (SDMMC 1-bit 模式) ===");
    ESP_LOGI(TAG, "  SD 引脚: CLK=%d CMD=%d D0=%d", SD_CLK, SD_CMD, SD_D0);
    ESP_LOGI(TAG, "  堆剩余: %u 字节, DMA内部: %u 字节 (最大块 %u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    /* 1. 快速挂载 (V1.0.61): 优先 20MHz, 失败降 5MHz 再试一次
     *    参考 esp-box-emu / Retro-Go: 开机一次性挂载, 不做高频重试,
     *    旧版 3 档速度 + 200/300/500ms 延时, 无卡时开机要卡 ~1.5s */
    const uint32_t speeds_khz[] = { 20000, 5000 };
    const uint32_t delays_ms[]  = {  50,  100 };
    const int retry_count = sizeof(speeds_khz) / sizeof(speeds_khz[0]);

    for (int attempt = 0; attempt < retry_count; attempt++) {
        ESP_LOGI(TAG, "  [尝试 %d/%d] 速度 %lu kHz, 延时 %lu ms...",
                 attempt + 1, retry_count,
                 (unsigned long)speeds_khz[attempt],
                 (unsigned long)delays_ms[attempt]);

        vTaskDelay(pdMS_TO_TICKS(delays_ms[attempt]));

        /* 重试前彻底释放 SDMMC host, 避免 "already initialized" 跳过初始化
         * 导致 DMA buffer 无法重新分配 (内部 RAM 碎片化时报 not enough mem) */
        if (attempt > 0) {
            sdmmc_host_deinit();
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024,
        };

        /* SDMMC 主机 - ESP32-S3 内置 SD 卡专用控制器 */
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = speeds_khz[attempt];
        host.flags = SDMMC_HOST_FLAG_1BIT;  /* 1-bit SD 模式 (官方示例 width=1) */

        sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_cfg.width = 1;        /* 1-bit 模式 */
        slot_cfg.clk = SD_CLK;     /* GPIO 38 */
        slot_cfg.cmd = SD_CMD;     /* GPIO 21 */
        slot_cfg.d0 = SD_D0;       /* GPIO 39 */
        slot_cfg.d1 = -1;          /* 1-bit 模式不用 */
        slot_cfg.d2 = -1;
        slot_cfg.d3 = -1;
        slot_cfg.cd = -1;          /* 不用卡检测 */
        slot_cfg.wp = -1;          /* 不用写保护 */
        slot_cfg.flags = 0;

        esp_err_t ret = esp_vfs_fat_sdmmc_mount(s_mount_point, &host, &slot_cfg, &mount_cfg, &s_card);
        if (ret == ESP_OK) {
            s_mounted = true;
            ESP_LOGI(TAG, "  [尝试 %d] 挂载成功! card=%p 容量 %llu 字节",
                     attempt + 1, s_card,
                     (unsigned long long)((uint64_t)s_card->csd.capacity * 512));
            sdmmc_card_print_info(stdout, s_card);
            ESP_LOGI(TAG, "=== sd_mount 成功 ===");

            /* 自动检测并创建所有需要的目录 */
            const char *need_dirs[] = {
                "/sdcard/gam",
                "/sdcard/mp3",
                "/sdcard/screensaver",
                "/sdcard/dict",  /* 电子词典存档目录 */
                "/sdcard/gb",    /* GB 模拟器 ROM 目录 */
                "/sdcard/gbc",   /* GBC 模拟器 ROM 目录 */
                "/sdcard/nes",   /* NES 模拟器 ROM 目录 */
                "/sdcard/AB",    /* arduboy ROM 目录 */
                "/sdcard/books", /* 电子书目录 */
                NULL
            };
            for (int i = 0; need_dirs[i]; i++) {
                if (mkdir(need_dirs[i], 0755) == 0) {
                    ESP_LOGI(TAG, "  创建目录: %s", need_dirs[i]);
                } else if (errno == EEXIST) {
                    /* 已存在, 正常 */
                } else {
                    ESP_LOGW(TAG, "  创建目录失败: %s errno=%d", need_dirs[i], errno);
                }
            }

            return ESP_OK;
        }

        ESP_LOGW(TAG, "  [尝试 %d] 挂载失败: %s (0x%x)",
                 attempt + 1, esp_err_to_name(ret), ret);
        s_card = NULL;
        /* 失败后释放 host, 下次重试能重新初始化 */
        sdmmc_host_deinit();
    }

    ESP_LOGE(TAG, "=== sd_mount 全部 %d 次尝试都失败 ===", retry_count);
    ESP_LOGE(TAG, "  可能原因:");
    ESP_LOGE(TAG, "    - TF 卡没插好 (接触不良)");
    ESP_LOGE(TAG, "    - TF 卡文件系统不识别 (不是 FAT32/exFAT)");
    ESP_LOGE(TAG, "    - TF 卡损坏");
    ESP_LOGE(TAG, "  建议: 进菜单 → TF卡 → 格式化TF卡");
    return ESP_FAIL;
}

/* 卸载 SD 卡 (SDMMC 模式)
 * 重要: esp_vfs_fat_sdcard_unmount 只卸载 VFS+diskio, 不释放 SDMMC host.
 *       必须额外调用 sdmmc_host_deinit() 彻底释放 host 和 DMA buffer,
 *       否则重挂时 "host already initialized" 跳过初始化, DMA buffer 无法重新分配. */
void sd_unmount(void) {
    if (!s_mounted) return;
    esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    sdmmc_host_deinit();  /* 彻底释放 SDMMC host + DMA buffer */
    s_mounted = false;
    s_card = NULL;
    ESP_LOGI(TAG, "SD 卡已卸载 (SDMMC host 已释放)");
}

/* 卸载 VFS 但保留 card 句柄 (用于切换到 USB MSC 模式) */
int sd_unmount_vfs_keep_card(void) {
    if (!s_mounted || !s_card) return 0;

    ESP_LOGI(TAG, "[卸载VFS] 开始 (保留 SDMMC 主机和 card 句柄)");

    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);

    esp_err_t ret = esp_vfs_fat_unregister_path(s_mount_point);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[卸载VFS] esp_vfs_fat_unregister_path 失败: %s (0x%x)",
                 esp_err_to_name(ret), ret);
    }

    f_mount(NULL, "", 0);

    ff_sdmmc_set_disk_status_check(pdrv, false);

    s_mounted = false;

    ESP_LOGI(TAG, "[卸载VFS] 完成, card=%p 仍然有效, SDMMC 主机保持运行", s_card);
    return 0;
}

int sd_remount_vfs_from_card(void) {
    if (s_mounted) return 0;
    if (!s_card) return -1;

    ESP_LOGI(TAG, "[重挂VFS] 从已有 card=%p 重新挂载 FAT/VFS", s_card);

    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv == 0xFF) {
        ff_diskio_register_sdmmc(0, s_card);
        pdrv = 0;
        ESP_LOGI(TAG, "[重挂VFS] 重新注册 diskio, pdrv=%u", pdrv);
    }

    char drive_str[8];
    snprintf(drive_str, sizeof(drive_str), "%u:", pdrv);

    FATFS *fs = NULL;
    esp_err_t ret = esp_vfs_fat_register_cfg(
        &(esp_vfs_fat_conf_t){
            .base_path = s_mount_point,
            .fat_drive = drive_str,
            .max_files = 5,
        },
        &fs
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[重挂VFS] esp_vfs_fat_register_cfg 失败: %s (0x%x)",
                 esp_err_to_name(ret), ret);
        return -2;
    }

    FRESULT fres = f_mount(fs, drive_str, 1);
    if (fres != FR_OK) {
        ESP_LOGE(TAG, "[重挂VFS] f_mount 失败: %d", fres);
        esp_vfs_fat_unregister_path(s_mount_point);
        return -3;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "[重挂VFS] 成功 @ %s", s_mount_point);
    return 0;
}

/* 取得 sdmmc_card_t 句柄 (用于 USB MSC 直接访问) */
sdmmc_card_t *sd_get_card(void) { return s_card; }

/* 是否已挂载 (供 UI 状态显示用) */
bool sd_is_mounted(void) { return s_mounted && s_card != NULL; }

/* 判断文件名后缀 (大小写不敏感) */
static bool has_ext(const char *name, const char *ext) {
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return false;
    const char *p = name + nlen - elen;
    for (size_t i = 0; i < elen; i++) {
        char a = p[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

/* 去除 UTF-8 BOM (避免误显示) */
static void strip_bom(char *s) {
    if ((uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) {
        size_t len = strlen(s);
        if (len >= 3) {
            memmove(s, s + 3, len - 3 + 1);  /* +1 包含 '\0' */
        }
    }
}

/* 扫描 /sdcard/gam/ 目录下的 .gam 文件
 * 单次最大 1000 个 (避免堆栈溢出)
 * 内存: 1 个 out[64] = 64 字节, 1000 个 = 64KB, 用 malloc 分配 */
int scan_bbk_games(char out[][64], int max_out) {
    if (max_out <= 0 || out == NULL) return 0;

    /* 尝试挂载 (失败时尝试小频率) */
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) {
            ESP_LOGW(TAG, "SD 卡未挂载, 扫描跳过");
            return 0;
        }
    }

    const char *dir_path = "/sdcard/gam";

    /* 检查目录是否存在 */
    struct stat st;
    if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "目录不存在: %s (stat失败, 尝试重新挂载SD卡)", dir_path);
        /* NVS flash 写操作可能导致 SDMMC 传输超时, FATFS 状态失效
         * s_mounted 仍为 true 但实际不可用, 需要重新挂载 */
        sd_unmount();
        if (sd_mount() != ESP_OK) return 0;
        if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ESP_LOGW(TAG, "重新挂载后目录仍不存在: %s", dir_path);
            return 0;
        }
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "打开目录失败: %s (opendir失败, 尝试重新挂载SD卡)", dir_path);
        sd_unmount();
        if (sd_mount() != ESP_OK) return 0;
        dir = opendir(dir_path);
        if (!dir) {
            ESP_LOGW(TAG, "重新挂载后仍无法打开目录: %s", dir_path);
            return 0;
        }
    }

    int n = 0;
    int skipped_non_reg = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_out) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        /* FATFS 上 d_type 可能为 DT_UNKNOWN, 需要用 stat() 兜底判断 */
        if (ent->d_type != DT_REG) {
            if (ent->d_type == DT_UNKNOWN) {
                /* DT_UNKNOWN: 用 stat() 判断是否普通文件 */
                char full[300];
                snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
                struct stat fst;
                if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode)) {
                    /* 是普通文件, 继续处理 */
                } else {
                    continue;  /* 不是文件 (目录或其他) */
                }
            } else {
                skipped_non_reg++;
                continue;  /* DT_DIR 或其他, 跳过 */
            }
        }
        if (!has_ext(ent->d_name, ".gam")) continue;  /* 只取 .gam */
        /* 复制文件名 (去掉 .gam) */
        char name[64];
        size_t nlen = strlen(ent->d_name);
        if (nlen > 60) nlen = 60;
        memcpy(name, ent->d_name, nlen);
        name[nlen] = '\0';
        /* 去除扩展名 */
        char *dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        strip_bom(name);
        /* 写入输出 */
        strncpy(out[n], name, 63);
        out[n][63] = '\0';
        n++;
    }
    closedir(dir);

    ESP_LOGI(TAG, "扫描 %s: %d 个 .gam (跳过非文件%d)",
             dir_path, n, skipped_non_reg);
    return n;
}

/* === 子文件夹扫描 / 跨目录游戏扫描 (左右分栏 UI 使用) === */

/* 将 gam 下子目录列表按字母序 (大小写不敏感) 冒泡排序, 方便 UI 稳定显示 */
static void sort_names_alpha(char items[][64], int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            if (strcasecmp(items[j], items[j + 1]) > 0) {
                char tmp[64];
                memcpy(tmp, items[j], 64);
                memcpy(items[j], items[j + 1], 64);
                memcpy(items[j + 1], tmp, 64);
            }
        }
    }
}

/* 内部: 通用目录打开 + 重新挂载兜底 */
static DIR *open_dir_with_remount(const char *dir_path) {
    struct stat st;
    if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "目录不存在: %s (尝试重新挂载SD卡)", dir_path);
        sd_unmount();
        if (sd_mount() != ESP_OK) return NULL;
        if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;
    }
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "打开目录失败: %s (尝试重新挂载SD卡)", dir_path);
        sd_unmount();
        if (sd_mount() != ESP_OK) return NULL;
        dir = opendir(dir_path);
    }
    return dir;
}

/* 扫描 /sdcard/gam/ 下的子文件夹名 */
int scan_bbk_folders(char out[][64], int max_out) {
    if (max_out <= 0 || out == NULL) return 0;
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return 0;
    }
    DIR *dir = open_dir_with_remount("/sdcard/gam");
    if (!dir) {
        ESP_LOGW(TAG, "scan_bbk_folders: 无法打开 /sdcard/gam");
        return 0;
    }
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_out) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        /* 跳过 . 和 .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        bool is_dir = false;
        if (ent->d_type == DT_DIR) {
            is_dir = true;
        } else if (ent->d_type == DT_UNKNOWN) {
            char full[300];
            snprintf(full, sizeof(full), "/sdcard/gam/%s", ent->d_name);
            struct stat fst;
            if (stat(full, &fst) == 0 && S_ISDIR(fst.st_mode)) {
                is_dir = true;
            }
        }
        if (!is_dir) continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen > 60) nlen = 60;
        memcpy(out[n], ent->d_name, nlen);
        out[n][nlen] = '\0';
        strip_bom(out[n]);
        /* 跳过空文件夹名 (BOM 残留等) */
        if (out[n][0] == '\0') continue;
        n++;
    }
    closedir(dir);
    sort_names_alpha(out, n);
    ESP_LOGI(TAG, "scan_bbk_folders: %d 个子文件夹", n);
    return n;
}

/* 内部: 扫描 dir_path 下 .gam 文件, 写入 out (去后缀). 返回数量 */
static int scan_gam_in_dir(const char *dir_path, char out[][64], int max_out) {
    if (max_out <= 0 || out == NULL) return 0;
    DIR *dir = open_dir_with_remount(dir_path);
    if (!dir) return 0;
    int n = 0;
    int skipped_non_reg = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_out) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        if (ent->d_type != DT_REG) {
            if (ent->d_type == DT_UNKNOWN) {
                char full[300];
                snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
                struct stat fst;
                if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode)) {
                    /* 是普通文件, 继续 */
                } else {
                    continue;
                }
            } else {
                skipped_non_reg++;
                continue;
            }
        }
        if (!has_ext(ent->d_name, ".gam")) continue;
        char name[64];
        size_t nlen = strlen(ent->d_name);
        if (nlen > 60) nlen = 60;
        memcpy(name, ent->d_name, nlen);
        name[nlen] = '\0';
        char *dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        strip_bom(name);
        /* 跳过空名 (e.g. ".gam" / BOM 残留) */
        if (name[0] == '\0') continue;
        strncpy(out[n], name, 63);
        out[n][63] = '\0';
        n++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "扫描 %s: %d 个 .gam (跳过非文件%d)",
             dir_path, n, skipped_non_reg);
    return n;
}

/* 扫描指定子目录下的 .gam (folder_name==NULL/"" 等价于 /sdcard/gam) */
int scan_bbk_games_in_folder(const char *folder_name, char out[][64], int max_out) {
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return 0;
    }
    char dir_path[160];
    if (folder_name == NULL || folder_name[0] == '\0') {
        snprintf(dir_path, sizeof(dir_path), "/sdcard/gam");
    } else {
        /* 防止用户目录名带 '/' 污染路径: 仅取 basename 部分 */
        const char *base = strrchr(folder_name, '/');
        base = base ? base + 1 : folder_name;
        snprintf(dir_path, sizeof(dir_path), "/sdcard/gam/%s", base);
    }
    return scan_gam_in_dir(dir_path, out, max_out);
}

/* 获取 folder_name 子目录中第 idx 个 .gam 的完整路径 */
const char *bbk_game_path_in_folder(const char *folder_name, int idx, char *out, size_t out_size) {
    if (out == NULL || out_size < 160) return NULL;
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return NULL;
    }
    char dir_path[160];
    if (folder_name == NULL || folder_name[0] == '\0') {
        snprintf(dir_path, sizeof(dir_path), "/sdcard/gam");
    } else {
        const char *base = strrchr(folder_name, '/');
        base = base ? base + 1 : folder_name;
        snprintf(dir_path, sizeof(dir_path), "/sdcard/gam/%s", base);
    }
    DIR *dir = open_dir_with_remount(dir_path);
    if (!dir) return NULL;
    int cur = 0;
    struct dirent *ent;
    const char *result = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        if (ent->d_type != DT_REG) {
            if (ent->d_type == DT_UNKNOWN) {
                /* full 需 >= dir_path(160) + 1(/) + NAME_MAX(255) + 1(NUL) */
                char full[512];
                snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
                struct stat fst;
                if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode)) {
                    /* 普通文件 */
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (!has_ext(ent->d_name, ".gam")) continue;
        if (cur == idx) {
            snprintf(out, out_size, "%s/%s", dir_path, ent->d_name);
            result = out;
            break;
        }
        cur++;
    }
    closedir(dir);
    return result;
}

/* ============ V1.0.46: GB 分栏菜单 (与电子词典一致) ============
 * 通用: 扫描 dir_path 下的子文件夹名 (GB/GBC/NES/FC/arduboy 等各平台共用) */
static int scan_folders_in_dir(const char *dir_path, char out[][64], int max_out) {
    if (max_out <= 0 || out == NULL) return 0;
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return 0;
    }
    DIR *dir = open_dir_with_remount(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "scan_folders_in_dir: 无法打开 %s", dir_path);
        return 0;
    }
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_out) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        bool is_dir = false;
        if (ent->d_type == DT_DIR) {
            is_dir = true;
        } else if (ent->d_type == DT_UNKNOWN) {
            char full[300];
            snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
            struct stat fst;
            if (stat(full, &fst) == 0 && S_ISDIR(fst.st_mode)) {
                is_dir = true;
            }
        }
        if (!is_dir) continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen > 60) nlen = 60;
        memcpy(out[n], ent->d_name, nlen);
        out[n][nlen] = '\0';
        strip_bom(out[n]);
        if (out[n][0] == '\0') continue;
        n++;
    }
    closedir(dir);
    sort_names_alpha(out, n);
    ESP_LOGI(TAG, "scan_folders_in_dir: %s, %d 个子文件夹", dir_path, n);
    return n;
}

int scan_gb_folders(char out[][64], int max_out) {
    return scan_folders_in_dir("/sdcard/gb", out, max_out);
}

/* 扫描 dir_path 下的子文件夹名 (各平台专用目录) */
int scan_platform_folders(const char *dir_path, char out[][64], int max_out) {
    return scan_folders_in_dir(dir_path, out, max_out);
}

/* 扫描指定平台子目录下指定后缀的文件 (folder_name==NULL/"" 等价于 dir_path)
 * ext: ".gb"/".gbc"/".nes"/".hex" 等, 决定按哪种扩展名过滤. */
static int scan_games_in_dir(const char *dir_path, const char *folder_name, const char *ext,
                             char out[][64], int max_out) {
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return 0;
    }
    char scan_path[160];
    if (folder_name == NULL || folder_name[0] == '\0') {
        snprintf(scan_path, sizeof(scan_path), "%s", dir_path);
    } else {
        const char *base = strrchr(folder_name, '/');
        base = base ? base + 1 : folder_name;
        snprintf(scan_path, sizeof(scan_path), "%s/%s", dir_path, base);
    }
    DIR *dir = open_dir_with_remount(scan_path);
    if (!dir) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_out) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        if (ent->d_type != DT_REG) {
            if (ent->d_type == DT_UNKNOWN) {
                char full[512];
                snprintf(full, sizeof(full), "%s/%s", scan_path, ent->d_name);
                struct stat fst;
                if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode)) {
                    /* 普通文件 */
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (!has_ext(ent->d_name, ext)) continue;
        char name[64];
        size_t nlen = strlen(ent->d_name);
        if (nlen > 60) nlen = 60;
        memcpy(name, ent->d_name, nlen);
        name[nlen] = '\0';
        char *dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        strip_bom(name);
        if (name[0] == '\0') continue;
        strncpy(out[n], name, 63);
        out[n][63] = '\0';
        n++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "扫描 %s: %d 个 %s", scan_path, n, ext);
    return n;
}

/* 扫描指定 GB 子目录下的 .gb/.gbc (folder_name==NULL/"" 等价于 /sdcard/gb) */
int scan_gb_games_in_folder(const char *folder_name, const char *ext, char out[][64], int max_out) {
    return scan_games_in_dir("/sdcard/gb", folder_name, ext, out, max_out);
}

/* 扫描指定平台子目录下指定后缀的文件 */
int scan_platform_games(const char *dir_path, const char *folder_name, const char *ext,
                        char out[][64], int max_out) {
    return scan_games_in_dir(dir_path, folder_name, ext, out, max_out);
}

/* 获取 dir_path 子目录中第 idx 个指定后缀文件的完整路径 */
static const char *game_path_in_dir(const char *dir_path, const char *folder_name, int idx,
                                    const char *ext, char *out, size_t out_size) {
    if (out == NULL || out_size < 160) return NULL;
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return NULL;
    }
    char scan_path[160];
    if (folder_name == NULL || folder_name[0] == '\0') {
        snprintf(scan_path, sizeof(scan_path), "%s", dir_path);
    } else {
        const char *base = strrchr(folder_name, '/');
        base = base ? base + 1 : folder_name;
        snprintf(scan_path, sizeof(scan_path), "%s/%s", dir_path, base);
    }
    DIR *dir = open_dir_with_remount(scan_path);
    if (!dir) return NULL;
    int cur = 0;
    struct dirent *ent;
    const char *result = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        if (ent->d_type != DT_REG) {
            if (ent->d_type == DT_UNKNOWN) {
                char full[512];
                snprintf(full, sizeof(full), "%s/%s", scan_path, ent->d_name);
                struct stat fst;
                if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode)) {
                    /* 普通文件 */
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (!has_ext(ent->d_name, ext)) continue;
        if (cur == idx) {
            snprintf(out, out_size, "%s/%s", scan_path, ent->d_name);
            result = out;
            break;
        }
        cur++;
    }
    closedir(dir);
    return result;
}

/* 获取 GB 子目录中第 idx 个 .gb/.gbc 的完整路径 (ext: ".gb" 或 ".gbc") */
const char *gb_game_path_in_folder(const char *folder_name, int idx, const char *ext, char *out, size_t out_size) {
    return game_path_in_dir("/sdcard/gb", folder_name, idx, ext, out, out_size);
}

/* 获取指定平台子目录中第 idx 个指定后缀文件的完整路径 */
const char *platform_game_path(const char *dir_path, const char *folder_name, int idx,
                               const char *ext, char *out, size_t out_size) {
    return game_path_in_dir(dir_path, folder_name, idx, ext, out, out_size);
}

/* 扫描 .gam 文件数 (不返回名字, 只统计) - 用于快速检测 */
int count_bbk_games(void) {
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return 0;
    }
    const char *dir_path = "/sdcard/gam";
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        if (ent->d_type == DT_REG) {
            if (has_ext(ent->d_name, ".gam")) n++;
        }
    }
    closedir(dir);
    return n;
}

/* ===== SD 卡监控 (轻量 watcher) =====
 * 参考 esp-box-emu / Retro-Go: 开机挂载一次, 不做高频重挂.
 * 本 watcher 只负责: 卡被拔出/挂载失败后, 在空闲时慢速补挂一次
 * (15s 间隔), 不再做动态调速 (高低速都是 20MHz, 纯死代码) 和
 * 周期性健康检查 (无 CD 检测脚时高频探活反而容易抢 SDMMC 总线).
 *
 * 关键: 游戏运行 (game_run_loop) 和 USB MSC "挂载到电脑" 期间必须
 * 暂停. 否则 watcher 发现 s_mounted==false 就去 sdmmc_host_deinit()+
 * 重新挂载, 会拆掉正在被 PC 读写的 SDMMC host, 导致设备挂死
 * (PC 端 "无法访问磁盘") 或游戏读 ROM 时看门狗复位.
 */
#define SD_WATCHER_PERIOD_MS   2000   /* 检查周期: 2s (纯变量判断, 零 IO) */
#define SD_REMOUNT_INTERVAL_MS 15000  /* 补挂间隔: 15s (掉卡后慢速恢复) */

static TaskHandle_t s_sd_watcher_h = NULL;       /* 监控任务句柄 */
static volatile bool s_watcher_paused = false;   /* 游戏/MSC 期间暂停 */

/* 监控任务: 仅在空闲且未挂载时, 每 15s 补挂一次 */
static void sd_watcher_task(void *arg) {
    uint32_t last_remount_attempt = 0;
    ESP_LOGI(TAG, "[SD Watcher] 启动, 补挂间隔 %d ms", SD_REMOUNT_INTERVAL_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SD_WATCHER_PERIOD_MS));
        /* 游戏运行 / USB MSC 期间: 完全不碰 SD (V1.0.60+) */
        if (s_watcher_paused) continue;
        /* 已挂载: 什么都不做 */
        if (s_mounted && s_card) continue;

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_remount_attempt < SD_REMOUNT_INTERVAL_MS) continue;
        last_remount_attempt = now;

        ESP_LOGW(TAG, "[SD Watcher] 未挂载, 尝试补挂...");
        /* 重挂前确保 host 已彻底释放 (sd_mount 内部会重新初始化) */
        sdmmc_host_deinit();
        vTaskDelay(pdMS_TO_TICKS(100));
        if (sd_mount() == ESP_OK) {
            ESP_LOGI(TAG, "[SD Watcher] 补挂成功!");
        } else {
            ESP_LOGW(TAG, "[SD Watcher] 补挂失败, %d ms 后再试",
                     SD_REMOUNT_INTERVAL_MS);
        }
    }
    vTaskDelete(NULL);
}

/* 暂停/恢复 SD 监控:
 *  - 游戏运行期间由 game_run_loop 暂停 (避免与读 ROM/存档冲突)
 *  - USB MSC "挂载到电脑" 期间由菜单暂停 (VFS 已卸载但 PC 在读写扇区) */
void sd_watcher_set_paused(bool paused) {
    s_watcher_paused = paused;
}

/* 启动 SD 卡监控任务 (轻量 watcher: 空闲时慢速补挂)
 * 在 sd_mount() 成功后调用一次 */
void sd_watcher_start(void) {
    if (s_sd_watcher_h) {
        ESP_LOGW(TAG, "[SD Watcher] 已存在, 跳过");
        return;
    }
    /* 优先级: 1 (与 UI 相同, 时间片轮转; V1.0.41: 从 5 降到 1, 不抢占 UI) */
    BaseType_t ret = xTaskCreate(
        sd_watcher_task, "sd_watcher",
        3072, NULL, 1, &s_sd_watcher_h);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "[SD Watcher] 创建失败: %d", ret);
        s_sd_watcher_h = NULL;
    } else {
        ESP_LOGI(TAG, "[SD Watcher] 创建成功");
    }
}

/* 完整的卸载 - 释放 SD 资源 (SDMMC 模式)
 * 包含 sdmmc_host_deinit() 彻底释放 host 和 DMA buffer,
 * 否则重挂时 host 已初始化被跳过, DMA buffer 无法重新分配. */
static void sd_full_unmount(void) {
    ESP_LOGI(TAG, "[卸载] 开始, s_mounted=%d, s_card=%p", s_mounted, s_card);

    /* 1. 卸载 VFS (如果还挂着) */
    if (s_mounted && s_card) {
        esp_err_t ret = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[卸载] VFS 卸载失败: %s (0x%x)", esp_err_to_name(ret), ret);
        }
    }
    s_mounted = false;
    s_card = NULL;

    /* 2. 彻底释放 SDMMC host + DMA buffer (关键!) */
    sdmmc_host_deinit();

    /* 3. 等待卡稳定 */
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "[卸载] 完成 (host 已释放)");
}

/* 完全重新挂载 (SDMMC 模式)
 * ESP-IDF 的 esp_vfs_fat_sdmmc_mount() 内部会处理 SDMMC 总线初始化 */
static int sd_full_remount(uint8_t format_if_needed) {
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_if_needed,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = format_if_needed ? 5000 : 20000;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;
    slot_cfg.clk = SD_CLK;
    slot_cfg.cmd = SD_CMD;
    slot_cfg.d0 = SD_D0;
    slot_cfg.d1 = -1;
    slot_cfg.d2 = -1;
    slot_cfg.d3 = -1;
    slot_cfg.cd = -1;
    slot_cfg.wp = -1;
    slot_cfg.flags = 0;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(s_mount_point, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "挂载失败: %s (0x%x)", esp_err_to_name(ret), ret);
        s_card = NULL;
        return -2;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "挂载成功 @ %s, card=%p (SDMMC 1-bit, 格式化=%d)",
             s_mount_point, s_card, format_if_needed);
    return 0;
}

/* 格式化 TF 卡 + 创建 BBK/Gam, GB/Gam 目录 (SDMMC 模式)
 * 流程:
 *   1. 完全卸载
 *   2. 探测卡 (低速, 5MHz)
 *   3. 物理擦除
 *   4. 用 format_if_mount_failed=true 重新挂载, 触发自动 mkfs
 *   5. 创建目录
 * 返回:
 *    0  成功
 *   -1  SDMMC 主机初始化失败
 *   -2  SDMMC 设备 (slot) 初始化失败
 *   -3  SD 卡探测失败
 *   -4  物理擦除失败
 *   -5  格式化后挂载失败
 *   -6  创建目录失败 (警告, 不返回失败)
 */
int sd_format_and_create_dirs(void) {
    ESP_LOGW(TAG, "========== 开始格式化 TF 卡 (SDMMC 模式) ==========");
    ESP_LOGW(TAG, "[进度 ⑧] 完全卸载旧挂载...");

    /* 1. 完全卸载 */
    sd_full_unmount();

    /* 2. 探测卡 (低速) */
    ESP_LOGW(TAG, "[进度 ⑨] 探测卡 (5MHz 低速, SDMMC)...");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 5000;  /* 5MHz 低速更稳 */
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;
    slot_cfg.clk = SD_CLK;
    slot_cfg.cmd = SD_CMD;
    slot_cfg.d0 = SD_D0;
    slot_cfg.d1 = -1;
    slot_cfg.d2 = -1;
    slot_cfg.d3 = -1;
    slot_cfg.cd = -1;
    slot_cfg.wp = -1;
    slot_cfg.flags = 0;

    ESP_LOGW(TAG, "[进度 ⑩] 初始化 SDMMC 主机...");
    esp_err_t ret = sdmmc_host_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[进度 ⑩] SDMMC 主机初始化失败: %s (0x%x)", esp_err_to_name(ret), ret);
        return -2;
    }

    ESP_LOGW(TAG, "[进度 ⑪] 初始化 SDMMC slot (GPIO 38/21/39)...");
    ret = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[进度 ⑪] SDMMC slot 失败: %s (0x%x)", esp_err_to_name(ret), ret);
        sdmmc_host_deinit();
        return -2;
    }

    ESP_LOGW(TAG, "[进度 ⑫] 探测 SD 卡...");
    /* V1.0.68: 修复格式化失效: sdmmc_card_init 需要调用方提供 card 结构体地址,
     * 之前传 NULL 导致写入空指针/探测必然失败, "格式化"实际从未执行. */
    sdmmc_card_t card_struct;
    memset(&card_struct, 0, sizeof(card_struct));
    sdmmc_card_t *card = &card_struct;
    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[进度 ⑫] 探测 SD 卡失败: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGE(TAG, "         可能原因: TF 卡没插 / 接触不良 / 卡损坏");
        sdmmc_host_deinit();
        return -3;
    }
    ESP_LOGI(TAG, "[进度 ⑫] 探测到卡: 容量 %lu 扇区, 名称 %s",
             (unsigned long)card->csd.capacity, card->cid.name);
    sdmmc_card_print_info(stdout, card);

    /* 3. 物理擦除 (彻底清除所有数据) */
    ESP_LOGW(TAG, "[进度 ⑬] 物理擦除卡 (这可能需要数十秒)...");
    uint32_t sector_count = card->csd.capacity;
    ESP_LOGI(TAG, "         总扇区: %lu", (unsigned long)sector_count);
    ret = sdmmc_erase_sectors(card, 0, sector_count, SDMMC_ERASE_ARG);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "         ERASE 失败, 尝试 DISCARD...");
        ret = sdmmc_erase_sectors(card, 0, sector_count, SDMMC_DISCARD_ARG);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[进度 ⑬] 擦除失败: %s (0x%x)", esp_err_to_name(ret), ret);
        sdmmc_host_deinit();
        return -4;
    }
    ESP_LOGI(TAG, "[进度 ⑬] 擦除完成");

    /* 4. 释放 host, 准备重新挂载 */
    ESP_LOGW(TAG, "[进度 ⑭] 释放 host, 等待卡稳定...");
    sdmmc_host_deinit();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 5. 重新挂载 (format_if_mount_failed=true 触发 mkfs) */
    ESP_LOGW(TAG, "[进度 ⑮] 重新挂载 (format_if_mount_failed=true 触发 mkfs)...");
    int rc = sd_full_remount(1);
    if (rc != 0) {
        ESP_LOGE(TAG, "[进度 ⑮] 挂载失败 rc=%d", rc);
        ESP_LOGE(TAG, "         可能原因: mkfs 失败 / 卡读写出错");
        return -5;
    }
    ESP_LOGI(TAG, "[进度 ⑮] mkfs + 挂载成功");

    /* 6. 创建 gam, screensaver 目录 */
    ESP_LOGW(TAG, "[进度 ⑯] 创建 gam, screensaver 目录...");
    int dir_err = 0;
    const char *dirs[] = { "/sdcard/gam", "/sdcard/screensaver", "/sdcard/dict",
                           "/sdcard/gb", "/sdcard/gbc", "/sdcard/nes", "/sdcard/AB",
                           "/sdcard/books", NULL };
    for (int i = 0; dirs[i]; i++) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s", dirs[i]);
        /* 递归创建父目录 */
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        if (mkdir(tmp, 0755) == 0) {
            ESP_LOGI(TAG, "         创建目录: %s", tmp);
        } else if (errno == EEXIST) {
            ESP_LOGI(TAG, "         目录已存在: %s", tmp);
        } else {
            ESP_LOGW(TAG, "         创建目录失败: %s errno=%d", tmp, errno);
            dir_err++;
        }
    }
    if (dir_err > 0) {
        ESP_LOGE(TAG, "[进度 ⑮] 部分目录创建失败 (%d 个)", dir_err);
        /* 不返回失败 - 目录可能下次挂载时会自动建 */
    } else {
        ESP_LOGI(TAG, "[进度 ⑮] 目录创建完成");
    }

    ESP_LOGW(TAG, "========== 格式化完成 ==========");
    return 0;
}

/* 获取 TF 卡容量信息
 * 返回: 0 成功, -1 挂载失败 (卡未插或未格式化), -2 VFS info 失败
 *
 * 优化: 如果已经挂载, 直接用 esp_vfs_fat_info, 不要 unmount+remount
 * 原因: 之前的逻辑每次都卸载+重挂载, 但 SPI 设备复用 LCD 的 SPI2 时, 重新初始化
 *       容易因为时钟频率/状态残留导致 remount 失败, 误报"请格式化"
 */
int sd_get_info(uint64_t *total_bytes, uint64_t *free_bytes) {
    ESP_LOGW(TAG, "[卡信息] 进入, s_mounted=%d, s_card=%p, heap_free=%u",
             s_mounted, s_card, (unsigned)esp_get_free_heap_size());

    /* 1. 优先直接读 VFS info (如果已挂载且 card 有效) */
    if (s_mounted && s_card != NULL) {
        ESP_LOGI(TAG, "[卡信息] 已挂载, 直接读 VFS info");
        uint64_t total = 0, free = 0;
        int ret = esp_vfs_fat_info(s_mount_point, &total, &free);
        if (ret == ESP_OK) {
            if (total_bytes) *total_bytes = total;
            if (free_bytes) *free_bytes = free;
            ESP_LOGI(TAG, "[卡信息] 总 %llu 字节, 剩 %llu 字节",
                     (unsigned long long)total, (unsigned long long)free);
            return 0;
        }
        ESP_LOGW(TAG, "[卡信息] VFS info 失败: %s (0x%x), 尝试 remount",
                 esp_err_to_name(ret), ret);
        /* VFS info 失败, 落入下面的 remount 路径 */
    }

    /* 2. 未挂载 或 VFS info 失败: 重新挂载 */
    ESP_LOGW(TAG, "[卡信息] 重新挂载中...");
    sd_full_unmount();
    int rc = sd_full_remount(0);  /* format_if_needed=0, 不自动 mkfs */
    ESP_LOGW(TAG, "[卡信息] remount rc=%d, heap_free=%u", rc, (unsigned)esp_get_free_heap_size());
    if (rc != 0) {
        ESP_LOGE(TAG, "[卡信息] 挂载失败 rc=%d", rc);
        s_mounted = false;
        s_card = NULL;
        return -1;
    }
    ESP_LOGI(TAG, "[卡信息] 挂载成功, 读 VFS info");
    uint64_t total = 0, free = 0;
    int ret = esp_vfs_fat_info(s_mount_point, &total, &free);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[卡信息] VFS info 失败: %s (0x%x)", esp_err_to_name(ret), ret);
        return -2;
    }
    if (total_bytes) *total_bytes = total;
    if (free_bytes) *free_bytes = free;
    ESP_LOGI(TAG, "[卡信息] 总 %llu 字节, 剩 %llu 字节",
             (unsigned long long)total, (unsigned long long)free);
    return 0;
}

/* 返回 .gam 文件的完整路径 (外部 buffer 长度 >= 128)
 * 找不到时返回 NULL. */
const char *bbk_game_path(int idx, char *out, size_t out_size) {
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return NULL;
    }
    if (out_size < 128) return NULL;

    const char *dir_path = "/sdcard/gam";
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "bbk_game_path: opendir失败, 尝试重新挂载SD卡");
        sd_unmount();
        if (sd_mount() != ESP_OK) return NULL;
        dir = opendir(dir_path);
        if (!dir) return NULL;
    }

    int cur = 0;
    struct dirent *ent;
    const char *result = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        /* FATFS 兼容: DT_UNKNOWN 时用 stat() 兜底 */
        if (ent->d_type != DT_REG) {
            if (ent->d_type == DT_UNKNOWN) {
                char full[300];
                snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
                struct stat fst;
                if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode)) {
                    /* 是普通文件 */
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (!has_ext(ent->d_name, ".gam")) continue;
        if (cur == idx) {
            snprintf(out, out_size, "%s/%s", dir_path, ent->d_name);
            result = out;
            break;
        }
        cur++;
    }
    closedir(dir);
    return result;
}

/* === GB 游戏扫描 ===
 * GB 模拟器扫描 /sdcard/gb/ 目录
 * 复用 open_dir_with_remount() + has_ext() 工具函数 */

/* 通用: 扫描 dir_path 目录下指定后缀 (如 ".gb") 的文件
 * 写入 out (每个名字去后缀), 返回文件数 */
static int scan_files_by_ext(const char *dir_path, const char *ext,
                             char out[][64], int max_out) {
    if (max_out <= 0 || out == NULL) return 0;
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return 0;
    }
    DIR *dir = open_dir_with_remount(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "scan_files_by_ext: 无法打开 %s", dir_path);
        return 0;
    }
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_out) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        if (ent->d_type != DT_REG) {
            if (ent->d_type == DT_UNKNOWN) {
                char full[300];
                snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
                struct stat fst;
                if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode)) {
                    /* 是普通文件, 继续 */
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (!has_ext(ent->d_name, ext)) continue;
        char name[64];
        size_t nlen = strlen(ent->d_name);
        if (nlen > 60) nlen = 60;
        memcpy(name, ent->d_name, nlen);
        name[nlen] = '\0';
        char *dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        strip_bom(name);
        if (name[0] == '\0') continue;
        strncpy(out[n], name, 63);
        out[n][63] = '\0';
        n++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "扫描 %s: %d 个 %s", dir_path, n, ext);
    return n;
}

/* 通用: 获取 dir_path 下指定后缀第 idx 个文件的完整路径 */
static const char *file_path_by_ext(const char *dir_path, const char *ext,
                                    int idx, char *out, size_t out_size) {
    if (out == NULL || out_size < 160) return NULL;
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return NULL;
    }
    DIR *dir = open_dir_with_remount(dir_path);
    if (!dir) return NULL;
    int cur = 0;
    struct dirent *ent;
    const char *result = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        if (ent->d_type != DT_REG) {
            if (ent->d_type == DT_UNKNOWN) {
                char full[300];
                snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
                struct stat fst;
                if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode)) {
                    /* 是普通文件 */
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }
        if (!has_ext(ent->d_name, ext)) continue;
        if (cur == idx) {
            snprintf(out, out_size, "%s/%s", dir_path, ent->d_name);
            result = out;
            break;
        }
        cur++;
    }
    closedir(dir);
    return result;
}

/* 扫描 /sdcard/gb/ 下的 .gb 文件 */
int scan_gb_games(char out[][64], int max_out) {
    return scan_files_by_ext("/sdcard/gb", ".gb", out, max_out);
}

/* 获取第 idx 个 .gb 文件的完整路径 */
const char *gb_game_path(int idx, char *out, size_t out_size) {
    return file_path_by_ext("/sdcard/gb", ".gb", idx, out, out_size);
}

/* 文件浏览器: 通用目录扫描
 * 返回 0+ = 项数, 负值 = 错误 */
static int entry_cmp(const void *a, const void *b) {
    const file_entry_t *ea = (const file_entry_t *)a;
    const file_entry_t *eb = (const file_entry_t *)b;
    /* 目录优先 */
    bool a_dir = (ea->type == FB_TYPE_DIR);
    bool b_dir = (eb->type == FB_TYPE_DIR);
    if (a_dir != b_dir) return a_dir ? -1 : 1;
    /* 同类型按名字字母序 (大小写不敏感) */
    return strcasecmp(ea->name, eb->name);
}

int scan_path(const char *path, file_entry_t *out, int max_out) {
    if (!path || !out || max_out <= 0) return 0;

    if (!s_mounted) {
        if (sd_mount() != ESP_OK) {
            ESP_LOGW(TAG, "scan_path: SD 未挂载");
            return -1;
        }
    }

    /* 检查路径存在 */
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "scan_path: 路径不存在: %s", path);
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "scan_path: 不是目录: %s", path);
        return -1;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "scan_path: 打开失败: %s (errno=%d)", path, errno);
        return 0;
    }

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_out) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        /* 跳过 . 和 .. */
        if (strcmp(ent->d_name, ".") == 0) continue;
        /* 跳过 system (系统区, 装机时由 system_rom_install 创建) */
        if (strcmp(ent->d_name, "system") == 0) continue;

        file_entry_t *e = &out[n];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, ent->d_name, sizeof(e->name) - 1);

        if (ent->d_type == DT_DIR) {
            e->type = FB_TYPE_DIR;
            e->size = 0;
            n++;
        } else if (ent->d_type == DT_REG) {
            /* 文件类型判定 */
            if (has_ext(ent->d_name, ".gam")) {
                e->type = FB_TYPE_GAM;
            } else if (has_ext(ent->d_name, ".bin")) {
                e->type = FB_TYPE_BIN;
            } else {
                e->type = FB_TYPE_OTHER;
            }
            /* 拿文件大小 */
            char full[256];
            /* 限制 path + "/" + name 长度避免截断警告 */
            int written = snprintf(full, sizeof(full), "%s/", path);
            if (written > 0 && (size_t)written < sizeof(full) - 1) {
                strncpy(full + written, ent->d_name, sizeof(full) - (size_t)written - 1);
                full[sizeof(full) - 1] = '\0';
            }
            struct stat fst;
            if (stat(full, &fst) == 0) {
                e->size = (uint32_t)fst.st_size;
            } else {
                e->size = 0;
            }
            n++;
        }
        /* DT_UNKNOWN 等其它类型跳过 */
    }
    closedir(dir);

    /* 排序: 目录在前, 文件在后, 各按字母序 */
    qsort(out, n, sizeof(file_entry_t), entry_cmp);

    ESP_LOGI(TAG, "scan_path %s: %d 项", path, n);
    return n;
}

/* 扫描屏保图片目录 /sdcard/screensaver/
 * 返回文件完整路径列表, 支持 .bmp, .jpg, .jpeg, .png, .gif */
int scan_screensaver_images(char paths[][128], int max_paths) {
    if (max_paths <= 0 || paths == NULL) return 0;

    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return 0;
    }

    const char *dir_path = "/sdcard/screensaver";
    struct stat st;
    if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "屏保目录不存在: %s", dir_path);
        return 0;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "打开屏保目录失败: %s", dir_path);
        return 0;
    }

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max_paths) {
        if (IS_APPLE_METANAME(ent->d_name)) continue;
        if (ent->d_type != DT_REG) continue;
        const char *name = ent->d_name;
        if (!has_ext(name, ".bmp") && !has_ext(name, ".jpg") && 
            !has_ext(name, ".jpeg") && !has_ext(name, ".png") && 
            !has_ext(name, ".gif")) continue;
        size_t name_len = strlen(name);
        if (name_len > 100) name_len = 100;
        snprintf(paths[n], 128, "%s/%.*s", dir_path, (int)name_len, name);
        n++;
    }
    closedir(dir);

    ESP_LOGI(TAG, "扫描屏保图片: %d 个", n);
    return n;
}

/* 文件重命名 */
int sd_rename_file(const char *old_path, const char *new_path) {
    if (!old_path || !new_path) return -1;
    
    if (!s_mounted) {
        if (sd_mount() != ESP_OK) return -1;
    }
    
    if (rename(old_path, new_path) == 0) {
        ESP_LOGI(TAG, "文件重命名成功: %s -> %s", old_path, new_path);
        return 0;
    }
    
    ESP_LOGE(TAG, "文件重命名失败: %s -> %s, errno=%d", old_path, new_path, errno);
    return -1;
}
