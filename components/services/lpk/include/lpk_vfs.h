/**
 * lpk_vfs.h — 把 LPK 挂到 ESP-IDF VFS 的 `/lpk`（真机唯一入口）
 *
 * 为什么需要这一层
 * ----------------
 * 仙剑是 C 引擎，读盘走的是 `fopen/fseek/fread`（util.c → palcommon.c），
 * 不可能为了它把整个引擎改成"调用 lpk_xxx()"。所以只能**让它以为在读写普通文件**：
 *   把 `/lpk` 注册成一个 VFS 文件系统，引擎看到的 `/lpk/ABC.MKF` 就是个普通文件。
 * 真正的分流判定在 lpk_mount.c（资源走包、存档走卡），本文件只做三件事：
 *   ① 把 VFS 的 open/read/write/lseek/stat/access/mkdir/... 翻译成 lpk_m_*；
 *   ② 把"真实 FAT"那一半用 POSIX 同名函数实现，注入成 lpk_backing_t；
 *   ③ 管好句柄、并发（一把互斥锁护住包里那个共享 FILE*）与挂载/卸载。
 *
 * 挂载点为什么是 `/lpk`
 * ---------------------
 * ESP-IDF 的 `ESP_VFS_PATH_MAX = 15`，`esp_vfs_register()` 里
 * `is_path_prefix_valid()` 还要求前缀 **以 '/' 开头、不以 '/' 结尾、长度 2..15**。
 * `/lpk` 是 4 个字符，最短且语义清楚。（`/sdcard/lpk` 这种长前缀直接注册失败。）
 *
 * ⚠ 与 `/sdcard` 的关系：两个挂载点平级，互不覆盖 —— 写操作落到 `/sdcard/...`
 *   时仍然由 FATFS 处理，本层只在 `/lpk/...` 上生效。
 */
#ifndef LPK_VFS_H
#define LPK_VFS_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lpk1.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 挂载点（不要改成别的：见文件头 ESP_VFS_PATH_MAX 说明） */
#define LPK_VFS_MNT "/lpk"

/* 一条"可写旁路"登记：目录（"save/"）或单文件（"sdlpal.cfg"）。
 * 目录必须带结尾斜杠；单文件不带。两类都只能落在真实 FAT 上。 */
typedef struct {
    const char *path;
    bool        is_dir;
} lpk_vfs_wr_t;

/**
 * 挂载：打开 LPK + 注册 VFS。可重复调用（先自动卸载旧的）。
 *
 * @param lpk_path  卡上的 .lpk 绝对路径
 * @param base_dir  可写旁路的根（真实 FAT 目录，如 /sdcard/apps/app/pal）
 * @param w         可写路径表（可 NULL）
 * @param nw        条数
 *
 * 返回 ESP_OK / ESP_ERR_INVALID_STATE（已被别的文件系统占用）/ 由 LPK 错误码映射的 ESP_ERR。
 * 失败时**不会**留下半挂载状态。
 */
esp_err_t lpk_vfs_mount(const char *lpk_path, const char *base_dir,
                        const lpk_vfs_wr_t *w, int nw);

/* 卸载。句柄表里还开着的真实 FAT 文件会被关掉（防 FatFs 卷锁泄漏），
 * 包内句柄无需收（只读、无系统资源）。未挂载时调它返回 ESP_OK，幂等。 */
esp_err_t lpk_vfs_unmount(void);

bool        lpk_vfs_is_mounted(void);
const char *lpk_vfs_lpk_path(void);     /* 未挂载返回 "" */
int         lpk_vfs_file_count(void);   /* 包内资源条数 */
int         lpk_vfs_open_handles(void); /* 当前打开中的句柄数（诊断/泄漏检查） */

/**
 * 设备侧自检（串口命令 `lpkv` 调它）：**通过 VFS 本身**跑一遍仙剑的资源访问序列。
 *   · 打开 ABC/GOP/FBP，读 MKF 索引，按偏移随机读块，关掉重开；
 *   · 资源只读（写必须失败）；save/ 与 sdlpal.cfg 写进去要能在真实 FAT 上看到；
 *   · 句柄耗尽后释放能复用；错误路径/目录穿越被拒。
 * @param full_crc  true = 逐条读全包并比对每条资源的 CRC32（41 MB，几十秒）
 * @param out       结果文本（逐行，含 PASS/FAIL）
 * @return          0 = 全过，>0 = 失败条数
 */
int lpk_vfs_selftest(bool full_crc, char *out, size_t cap);

/* ---- PSRAM 整资源缓存（2026-11-01）----
 * 打开资源时把它**整个**读进 PSRAM（单资源 ≤2 MB、总量 ≤4 MB、LRU 淘汰），
 * 之后所有 read 都是 memcpy。目的：把"几十次 seek+小读"换成"一次连续大读"，
 * 并让重复打开（切场景/回菜单）完全不再碰 SD。
 * 运行期可关（串口 `lpkstat 0`），关掉即回到"每次直接读包"的老行为。 */
void lpk_vfs_cache_set(bool on);
bool lpk_vfs_cache_get(void);
void lpk_vfs_cache_flush(void);

/* I/O 与缓存计数（串口 `lpkstat` 用：判断慢在 SD 还是 CPU） */
void lpk_vfs_stats(uint32_t *reads, uint32_t *bytes, uint32_t *us,
                   uint32_t *hits, uint32_t *loads, uint32_t *cache_bytes);
void lpk_vfs_stats_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* LPK_VFS_H */
