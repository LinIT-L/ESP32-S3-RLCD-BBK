/* os_file.c — 见 os_file.h 的说明
 *
 * ⚠ 本文件**故意只依赖 libc** (stdio/stat/string/unistd), 不 include 任何
 *   ESP-IDF 头 —— 这样主机测试可以直接把它和调用方一起编译, 逐条断言掉电语义。
 */
#include "os_file.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* 路径上限: 本机最长存档路径 ≈ /sdcard/data/saves/<平台>/<长中文名>.sav (<160),
 * 再留 ".old"/".tmp" 后缀余量。超长直接拒绝, 不做截断 (截断会写错文件)。 */
#define OS_FILE_PATH_MAX 256

static bool path_exists(const char *p)
{
    struct stat st;
    return (p && p[0] && stat(p, &st) == 0);
}

/* 存在则返回大小, 不存在返回 -1。用于分辨"文件在但是个空壳"。 */
static long path_size(const char *p)
{
    struct stat st;
    if (!p || !p[0] || stat(p, &st) != 0) return -1;
    return (long)st.st_size;
}

os_fs_rec_t os_file_recover(const char *final_path, const char *old_path)
{
    if (!final_path || !final_path[0] || !old_path || !old_path[0]) return OS_FS_REC_NONE;

    /* 先看有没有 .old —— 正常情况下没有, 到这里就结束 (只一次 stat, 不做任何写)。
     * 顺序是刻意的: 加载/扫描路径会频繁调用, 不能每次都去 stat 正式文件再决定。 */
    if (!path_exists(old_path)) return OS_FS_REC_NONE;

    /* ★★ 2026-11-22 (第4回合) 保守化: 正式文件存在时**不再删 .old** ★★
     *   旧版写着"正式文件在 ⇒ .old 是换完没来得及删的陈旧副本, 清掉省空间"。
     *   这个推断只在"新档确实完整落盘"时成立。而 SD 卡有写缓存、无掉电保护,
     *   f_close 报成功之后掉电仍可能让新档变成半截 —— 此时 .old 是**唯一完好的副本**,
     *   按旧逻辑删掉它就再也救不回来了 (加载侧 magic/长度校验失败 = 玩家眼里"存档没了")。
     *   所以改成: 分不清哪个更完整时一律**保留**, 不替用户做删除决定。
     *   空间成本接近零 —— 正常流程末尾就删了 .old; 只有掉电残留才会走到这里,
     *   而下一次 os_file_replace/os_file_write_atomic 开头就会 remove(old_path) 覆盖掉它。
     *   唯一例外: 正式文件是 0 字节空壳 (目录项写进去了、数据簇没写), 那它是确定无效的,
     *   这时用 .old 顶替。 */
    const long final_sz = path_size(final_path);
    if (final_sz > 0) return OS_FS_REC_NONE;      /* 保留 .old, 不动 */
    if (final_sz == 0) {
        /* 正式文件是空壳 ⇒ 确定无效, 用 .old 顶替 (FatFs rename 不覆盖, 必须先删空壳) */
        if (remove(final_path) == 0 && rename(old_path, final_path) == 0)
            return OS_FS_REC_RESTORED;
        return OS_FS_REC_FAILED;
    }
    /* 正式文件缺失 ⇒ 掉电恰好落在"旧档已挪走、新档未就位"之间。复位回去。 */
    return (rename(old_path, final_path) == 0) ? OS_FS_REC_RESTORED : OS_FS_REC_FAILED;
}

os_fs_rec_t os_file_recover_auto(const char *final_path)
{
    if (!final_path || !final_path[0]) return OS_FS_REC_NONE;

    char buf[OS_FILE_PATH_MAX];
    os_fs_rec_t r = OS_FS_REC_NONE;

    if (snprintf(buf, sizeof(buf), "%s" OS_FILE_OLD_SUFFIX, final_path) < (int)sizeof(buf))
        r = os_file_recover(final_path, buf);
    else
        return OS_FS_REC_NONE;      /* 路径长到拼不出 .old ⇒ 不可能存在, 直接返回 */

    if (r != OS_FS_REC_NONE) return r;          /* .old 已处理, 不再动 .tmp */

    /* ★★ 2026-11-22 (第4回合) 取消"把 .tmp 提升成正式文件" ★★
     *   旧版在这里 rename("<path>.tmp" → path), 理由写的是"至少留下一份数据"。
     *   但 .tmp 有两种来源, 而 recover 无法区分:
     *     ① 写完了、长度校验也过了, 只差最后那次 rename —— 提升是对的;
     *     ② 正写到一半就掉电 (fwrite 中途) —— 提升等于把**半截数据**变成正式存档。
     *   NES/GBC/gam4980 还能靠自己的 magic+长度校验把 ② 拒收, 但 MD/SMS 的
     *   gwenesis/smsplus 加载函数不校验长度, 拿到半截 savestate 会读到垃圾甚至崩。
     *   用"可能崩"换"可能救回首次存档"不划算, 所以这里**什么都不做**: .tmp 原样留着,
     *   由下一次写入路径开头的 remove(tmp) 清掉 (三条写入路径都有这一步)。
     *   损失仅限"首次存档写到一半掉电" —— 那本来也没有旧档可救, 结果相同。 */
    return OS_FS_REC_NONE;
}

bool os_file_replace(const char *tmp_path, const char *final_path, const char *old_path)
{
    if (!tmp_path || !tmp_path[0] || !final_path || !final_path[0] ||
        !old_path || !old_path[0]) return false;

    bool moved_old = false;

    if (path_exists(final_path)) {
        (void)remove(old_path);                        /* 清上次遗留的陈旧备份 */
        if (rename(final_path, old_path) != 0) {
            (void)remove(tmp_path);                    /* 旧文件挪不动 ⇒ 放弃本次替换 */
            return false;
        }
        moved_old = true;
    }
    if (rename(tmp_path, final_path) != 0) {
        if (moved_old) (void)rename(old_path, final_path);   /* 回滚到旧版本 */
        (void)remove(tmp_path);
        return false;
    }
    if (moved_old) (void)remove(old_path);             /* 新文件已就位, 才删旧版本 */
    return true;
}

int os_file_write_atomic(const char *path, const void *hdr, size_t hdr_len,
                         const void *body, size_t body_len)
{
    if (!path || !path[0]) return -1;
    if ((hdr_len && !hdr) || (body_len && !body)) return -1;

    char tmp[OS_FILE_PATH_MAX], old[OS_FILE_PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    if (snprintf(old, sizeof(old), "%s" OS_FILE_OLD_SUFFIX, path) >= (int)sizeof(old)) return -1;

    (void)remove(tmp);                       /* 上次残留 (半截文件, 不能接在后面写) */

    FILE *f = fopen(tmp, "wb");
    if (!f) return -2;

    bool ok = true;
    if (hdr_len  && fwrite(hdr,  1, hdr_len,  f) != hdr_len)  ok = false;
    if (ok && body_len && fwrite(body, 1, body_len, f) != body_len) ok = false;
    if (ok && fflush(f) != 0) ok = false;
    /* ⚠ 这里**故意不调 fsync()**: FatFs 的 f_close() 内部就是 f_sync(), 已经把
     *   文件数据 + FAT 目录项全部落盘并返回错误码; 再显式 fsync 一次只是把同样的
     *   工作做两遍。对"每 5 秒存一次档"的引擎, 多这一次 = 多十几毫秒持总线,
     *   换来的是游戏中周期性掉帧 —— 纯亏。
     *   (page_wb.c 那种退出时只写一次的场合保留 fsync 无妨, 但也没必要。) */
    if (fclose(f) != 0) ok = false;          /* 卡满/拔卡: fwrite 成功而 fclose 失败 */

    /* 长度校验: 挡住"fwrite 报成功但实际少写"(掉卡、FAT 链断裂)。 */
    const size_t want = hdr_len + body_len;
    if (ok) {
        struct stat st;
        if (stat(tmp, &st) != 0 || (size_t)st.st_size != want) ok = false;
    }
    if (!ok) { (void)remove(tmp); return -3; }

    if (!os_file_replace(tmp, path, old)) { (void)remove(tmp); return -4; }
    return 0;
}
