#ifndef SD_SCAN_H
#define SD_SCAN_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include "sdmmc_cmd.h"   /* for sdmmc_card_t typedef */

#ifdef __cplusplus
extern "C" {
#endif

/* 挂载 TF/SD 卡到 /sdcard */
esp_err_t sd_mount(void);

/* 卸载 TF/SD 卡 */
void sd_unmount(void);

/* 扫描 /sdcard/gam/ 下所有 .gam 文件 (文曲星游戏)
 * 写入 out (去后缀), 返回文件数 */
int scan_bbk_games(char out[][64], int max_out);

/* 扫描 /sdcard/gam/ 下的子文件夹名 (按字母序, 跳过 . 和 ..)
 * 返回文件夹数 (>=0), 写入 out (每个名字 <= 63 字节 + NUL)
 * 失败 (无 SD 卡, 目录不存在) 返回 0 */
int scan_bbk_folders(char out[][64], int max_out);

/* 扫描指定子目录下的 .gam 文件 (folder_name 为 NULL/"" 时扫描 /sdcard/gam)
 * 返回 .gam 数 (>=0), 写入 out (每个名字去后缀)
 * 失败 (无 SD 卡, 目录不存在) 返回 0 */
int scan_bbk_games_in_folder(const char *folder_name, char out[][64], int max_out);

/* V1.0.46: GB 分栏菜单 (与电子词典一致) — 扫描 /sdcard/gb/ 下的子文件夹名 */
int scan_gb_folders(char out[][64], int max_out);
/* V1.0.46: 扫描指定 GB 子目录下的 .gb/.gbc 文件 (ext: ".gb" 或 ".gbc"; folder_name 为 NULL/"" 时扫描 /sdcard/gb) */
int scan_gb_games_in_folder(const char *folder_name, const char *ext, char out[][64], int max_out);
/* V1.0.46: 获取 GB 子目录中第 idx 个 .gb/.gbc 的完整路径 (ext: ".gb" 或 ".gbc") */
const char *gb_game_path_in_folder(const char *folder_name, int idx, const char *ext, char *out, size_t out_size);

/* V1.0.48: 平台通用扫描 (GB/GBC/NES/FC/arduboy 等各平台专用目录) — dir_path 为平台根目录 */
int scan_platform_folders(const char *dir_path, char out[][64], int max_out);
int scan_platform_games(const char *dir_path, const char *folder_name, const char *ext,
                        char out[][64], int max_out);
const char *platform_game_path(const char *dir_path, const char *folder_name, int idx,
                               const char *ext, char *out, size_t out_size);

/* 扫描 /sdcard/gb/ 下的 .gb 文件 (GB 模拟器)
 * 返回文件数 (>=0), 写入 out (每个名字去后缀)
 * 失败 (无 SD 卡, 目录不存在) 返回 0 */
int scan_gb_games(char out[][64], int max_out);

/* 获取第 idx 个 .gb 文件的完整路径 (写到 out, 长度 >= 160)
 * 找不到返回 NULL */
const char *gb_game_path(int idx, char *out, size_t out_size);

/* 获取指定子文件夹中第 idx 个 .gam 的完整路径 (写到 out, 长度 >= 160)
 * folder_name 为 NULL/"" 时使用 /sdcard/gam (相当于 bbk_game_path)
 * 找不到返回 NULL */
const char *bbk_game_path_in_folder(const char *folder_name, int idx, char *out, size_t out_size);

/* 格式化 TF 卡 + 自动创建 BBK/Gam 目录
 * 返回 0 成功, 负值失败 */
int sd_format_and_create_dirs(void);

/* 卸载 VFS 但保留 card 句柄 (切换 USB MSC 模式用) */
int sd_unmount_vfs_keep_card(void);

/* 从已有 card 句柄重新挂载 VFS/FAT (退出 USB MSC 模式用) */
int sd_remount_vfs_from_card(void);

/* 取得 sdmmc_card_t 句柄, 让 USB MSC 直接访问扇区 (已挂载时非 NULL) */
sdmmc_card_t *sd_get_card(void);

/* 是否已挂载 (供 UI 状态显示用) */
bool sd_is_mounted(void);

/* 启动 SD 卡监控任务 (轻量 watcher: 空闲时慢速补挂)
 * 在 sd_mount() 后调用一次 */
void sd_watcher_start(void);
/* 暂停/恢复 SD 监控 (游戏运行、USB MSC "挂载到电脑" 期间必须暂停) */
void sd_watcher_set_paused(bool paused);

/* 获取 TF 卡容量信息 (总字节, 已用, 空闲)
 * 返回 0 成功, 负值失败 */
int sd_get_info(uint64_t *total_bytes, uint64_t *free_bytes);

/* 获取第 idx 个游戏的完整路径 (写到 out, 长度 >= 128)
 * 找不到返回 NULL */
const char *bbk_game_path(int idx, char *out, size_t out_size);

/* === 通用文件浏览器 API === */

/* 文件类型 (用于 UI 显示不同图标) */
typedef enum {
    FB_TYPE_DIR = 0,   /* 目录 */
    FB_TYPE_GAM,       /* 文曲星 .gam 游戏 (可启动) */
    FB_TYPE_BIN,       /* BIOS ROM (8.BIN/E.BIN 之类) */
    FB_TYPE_OTHER,     /* 其它文件 */
} file_type_t;

/* 单个文件/目录项 */
typedef struct {
    char       name[64];  /* 文件名 (UTF-8) */
    uint32_t   size;      /* 字节数 (目录=0) */
    file_type_t type;
} file_entry_t;

/* 扫描任意路径下的文件和目录
 * - path: 绝对路径, e.g. "/sdcard", "/sdcard/gam"
 * - out: 输出数组
 * - max_out: 最大输出项数
 * 返回: 实际写入项数 (0 = 路径不存在或空)
 * 排序: 目录在前 (按字母序), 文件在后 (按字母序)
 * 不递归, 只扫当前层 */
int scan_path(const char *path, file_entry_t *out, int max_out);

/* 扫描屏保图片目录 /sdcard/screensaver/
 * 返回文件路径列表, 支持 .bmp, .jpg, .jpeg, .png, .gif */
int scan_screensaver_images(char paths[][128], int max_paths);

/* 文件重命名
 * 返回 0 成功, -1 失败 */
int sd_rename_file(const char *old_path, const char *new_path);

#ifdef __cplusplus
}
#endif

#endif /* SD_SCAN_H */
