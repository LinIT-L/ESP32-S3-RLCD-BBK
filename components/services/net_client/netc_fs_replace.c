/* netc_fs_replace.c — 见 netc_fs_replace.h 的说明
 *
 * ⚠ 本文件**故意不 include 任何 ESP-IDF 头文件**: 它同时被固件和
 *   tests/dl_replace 的主机测试编译。要打日志请在调用方做(本文件只返回结果码),
 *   这样主机测试可以逐条断言, 而不必解析日志。
 */
#include "netc_fs_replace.h"

#include <stdio.h>
#include <sys/stat.h>

static bool path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

netc_rec_t netc_fs_recover(const char *final_path, const char *bak_path)
{
    if (!final_path || !final_path[0] || !bak_path || !bak_path[0]) return NETC_REC_NONE;

    /* 先看有没有 .bak —— 正常情况下没有, 到这里就结束(一次 stat, 不做任何写操作)。
     * 这个顺序是刻意的: 扫描/启动路径会频繁调用, 不能每次都去 stat 正式文件再决定。 */
    if (!path_exists(bak_path)) return NETC_REC_NONE;

    if (path_exists(final_path)) {
        /* 正式文件在 ⇒ .bak 是"替换成功后没来得及删"的陈旧副本, 清掉省空间。
         * ⚠ 绝不反向覆盖: 有用的正式文件优先, 不会被旧备份顶掉。 */
        return (remove(bak_path) == 0) ? NETC_REC_CLEANED : NETC_REC_FAILED;
    }
    return (rename(bak_path, final_path) == 0) ? NETC_REC_RESTORED : NETC_REC_FAILED;
}

bool netc_fs_replace(const char *part_path, const char *final_path, const char *bak_path)
{
    if (!part_path || !part_path[0] || !final_path || !final_path[0] ||
        !bak_path || !bak_path[0]) return false;

    bool moved_old = false;

    if (path_exists(final_path)) {
        (void)remove(bak_path);                       /* 清上次遗留的陈旧备份 */
        if (rename(final_path, bak_path) != 0) {
            (void)remove(part_path);                  /* 旧文件挪不动 ⇒ 放弃本次替换 */
            return false;
        }
        moved_old = true;
    }
    if (rename(part_path, final_path) != 0) {
        if (moved_old) (void)rename(bak_path, final_path);   /* 回滚到旧版本 */
        (void)remove(part_path);
        return false;
    }
    if (moved_old) (void)remove(bak_path);            /* 新文件已就位, 才删旧版本 */
    return true;
}
