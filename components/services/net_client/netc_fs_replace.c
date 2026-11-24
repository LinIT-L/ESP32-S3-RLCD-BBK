/* netc_fs_replace.c — 见 netc_fs_replace.h 的说明
 *
 * ★★ 2026-11-22 (第3回合) 改成对 os_file.c 的薄转发。
 *   原因: os_file.c 与本模块解决的是**同一个问题**(FatFs 的 f_rename 不覆盖已存在目标
 *   ⇒ 必须"先腾位再换名 + 可回滚 + 掉电恢复"), 而本回合给模拟器存档(nes/gbc/md/sms)、
 *   收藏备份、白板都接上了它。两份实现并存的话, 修一处漏一处是必然的。
 *   现在只有 os_file.c 一份实现, 本文件只保留 netc 自己的命名与枚举。
 *
 * ⚠ 仍**不 include 任何 ESP-IDF 头文件**(os_file.c 同样只用 libc),
 *   所以 tests/dl_replace 依旧能主机编译被测源码 ——
 *   编译命令多带一个文件 os_file.c (见 tests/dl_replace/dl_replace.c 文件头的说明)。
 *
 * 语义对应关系(枚举值逐位相同, 直接强转安全):
 *   NETC_REC_NONE/RESTORED/CLEANED/FAILED  ↔  OS_FS_REC_NONE/RESTORED/CLEANED/FAILED
 *
 * 为什么不用 os_file_recover_auto(): 它拼的后缀是 ".old", 而 netc 的下载中间件
 *   历史命名是 ".part" + 替换备份 ".bak" —— 自动版会找不到 netc 的备份文件。
 *   本模块只做显式路径的 recover, 由调用方给准路径。
 */
#include "netc_fs_replace.h"
#include "os_file.h"

netc_rec_t netc_fs_recover(const char *final_path, const char *bak_path)
{
    return (netc_rec_t)os_file_recover(final_path, bak_path);
}

bool netc_fs_replace(const char *part_path, const char *final_path, const char *bak_path)
{
    return os_file_replace(part_path, final_path, bak_path);
}
