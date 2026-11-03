/**
 * am_lpk.h — 商店「LPK 单文件资源包」的安装收尾
 *
 * 与 am_asset.c(ZIP 解包) 的分工
 * ------------------------------
 *   asset : 下 zip → 解压 109 个文件 → 写标记      （字体包仍走这条）
 *   lpk   : 下 1 个 .lpk → **校验 + 登记** → 写标记  （仙剑 / 英雄坛说）
 * 下载本身由 am_store_net.c 的下载任务做（含 crc/大小校验、.part 后替换）；
 * 本文件只做下载成功之后的三件事，全部**失败即拒绝**、不写半截状态：
 *   ① 用 lpk_mode_lpk_ready() 验包（头/索引/9 条必备资源，全部读一遍索引）；
 *   ② 写 `.installed.<主干>` 与 `.pkg.<主干>.lst` —— 与 zip 包**同一套标记**，
 *      所以"已安装"判定和卸载（am_asset_uninstall）一行代码都不用改；
 *   ③ 写 `.install_mode = lpk` —— 这是启动时 lpk_mode_decide() 的最权威依据。
 *
 * 为什么不在下载时就落到最终文件名：net_client 的契约是"先写 .part、校验通过
 * 才替换"，所以目标位置要么没有、要么是上一份完好的旧包 —— 正合我们需要的语义。
 */
#ifndef AM_LPK_H
#define AM_LPK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 从索引路径取"包主干"：pkgs/pal.lpk → "pal"（与 zip 包 .installed.<stem> 同名规则） */
void am_lpk_stem(const char *path, char *out, unsigned cap);

/* 下载成功后调用：验包 + 写安装标记 + 记 .install_mode=lpk。
 * 返回 false = 包不可用（已打日志），调用方应报"安装失败"并删除该文件。 */
bool am_lpk_finish(const char *lpk_path, const char *target_dir);

/* 同上，并记录"这份包来自商店索引的哪一版"（src_ver/src_build 可为 NULL）。
 * 新体系（包内带 manifest）会把它写进 registry 的 store_ver/store_build，
 * 供商店的更新判定使用；老格式（无 manifest，如仙剑 pal.lpk）忽略这两个参数。 */
bool am_lpk_finish_meta(const char *lpk_path, const char *target_dir,
                        const char *src_ver, const char *src_build);

/* ★ 2026-11-02: 同上，并记录**服务器文件的身份**（mtime/crc32/size）。
 *   更新判定只认 src_mtime（服务器上那个文件的修改时间，不是本地下载时间）；
 *   src_crc/src_size 用于"内容其实没变"的判定与老应用迁移核对。
 *   传 0 = 不知道来源，商店侧退回 size+CRC32 核对。 */
bool am_lpk_finish_meta2(const char *lpk_path, const char *target_dir,
                         const char *src_ver, const char *src_build,
                         uint32_t src_mtime, uint32_t src_crc, uint32_t src_size);

/* 卸载后调用：把这包的安装记录清掉（否则下次启动仍按 lpk 找，找不到再回落目录）。
 * 只删 `<target>/.install_mode`，不碰用户存档。 */
void am_lpk_clear_record(const char *target_dir);

#ifdef __cplusplus
}
#endif

#endif /* AM_LPK_H */
