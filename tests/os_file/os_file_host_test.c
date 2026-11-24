/* os_file_host_test.c — 主机测试: 验证 os_file.c 的替换/恢复语义
 *
 * 编译运行 (在项目根):
 *   cc -I components/libs/osutil/include \
 *      tests/os_file/os_file_host_test.c \
 *      components/libs/osutil/os_file.c -o /tmp/os_file_test && /tmp/os_file_test
 *
 * ⚠ 注意能力边界: 主机文件系统是 POSIX, rename **会覆盖已存在目标**;
 *   真机 FatFs 的 f_rename 目标存在时返回 FR_EXIST(ff.c:5243-5245)。
 *   所以本测试能证明"代码逻辑/掉电分支正确", **不能**证明"真机覆盖成功" ——
 *   后者靠 os_file_replace() 的"先挪旧再换新"顺序保证(不依赖 rename 覆盖能力)。
 *
 * ★ 2026-11-22 对齐第4回合语义: 本条原先断言 recover 会"删陈旧 .old / 提升 .tmp",
 *   那两条策略已被第4回合**主动废除**(见 os_file.c 里的两段 ★★ 说明):
 *     · 正式档非空时保留 .old (不替用户删可能是唯一完好副本的数据);
 *     · .tmp 一律不提升 (无法区分"写完"与"写一半", 提升会让 MD/SMS 读到半截存档)。
 *   因此用例 4/5 的期望值随之更改; 用例 1 的 "4 头 + 8 体" 是旧字符串长度笔误
 *   ("HELLO-1" 为 7 字节), 一并修正。
 *   更完整的分支/故障/阶段覆盖见同目录 os_file_fault_test.c。
 */
#include "os_file.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL %s\n", msg); g_fail++; } \
                              else printf("  ok   %s\n", msg); } while (0)

static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }

static void write_raw(const char *p, const char *s)
{
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite(s, 1, strlen(s), f);
    fclose(f);
}

static char *read_raw(const char *p, char *buf, size_t cap)
{
    FILE *f = fopen(p, "rb");
    if (!f) { buf[0] = 0; return buf; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

int main(void)
{
    const char *dir = "/tmp/osfile_test";
    char base[128], path[256], tmp[256], old[256];
    snprintf(base, sizeof(base), "%s", dir);
    (void)system("rm -rf /tmp/osfile_test && mkdir -p /tmp/osfile_test");
    snprintf(path, sizeof(path), "%s/save.dat", base);
    snprintf(tmp,  sizeof(tmp),  "%s/save.dat.tmp", base);
    snprintf(old,  sizeof(old),  "%s/save.dat.old", base);

    printf("== 1. 首次写入 (无旧文件) ==\n");
    {
        const char body[] = "HELLO-1";
        int rc = os_file_write_atomic(path, "HDR!", 4, body, sizeof(body) - 1);
        CHECK(rc == 0, "返回 0");
        CHECK(fsize(path) == 4 + 7, "长度 = 4 头 + 7 体");   /* "HELLO-1" 是 7 字节 */
        CHECK(!exists(tmp), ".tmp 已清理");
        CHECK(!exists(old), "无 .old");
        char buf[64];
        CHECK(strcmp(read_raw(path, buf, sizeof buf), "HDR!HELLO-1") == 0, "内容正确");
    }

    printf("== 2. 覆盖已存在文件 (真机最容易失败的一步) ==\n");
    {
        const char body[] = "HELLO-2!!";
        int rc = os_file_write_atomic(path, "HDR2", 4, body, sizeof(body) - 1);
        CHECK(rc == 0, "返回 0");
        char buf[64];
        CHECK(strcmp(read_raw(path, buf, sizeof buf), "HDR2HELLO-2!!") == 0, "内容已换新");
        CHECK(!exists(old), ".old 已清理");
    }

    printf("== 3. 掉电模拟 A: 旧已挪走、新未就位 (只剩 .old) ==\n");
    {
        /* 手动构造: 把正式文件当 .old, 正式文件缺失 */
        (void)system("mv /tmp/osfile_test/save.dat /tmp/osfile_test/save.dat.old");
        CHECK(!exists(path) && exists(old), "构造完成: 只有 .old");
        os_fs_rec_t r = os_file_recover_auto(path);
        CHECK(r == OS_FS_REC_RESTORED, "返回 RESTORED");
        CHECK(exists(path) && !exists(old), ".old 已复位为正式文件");
        char buf[64];
        CHECK(strcmp(read_raw(path, buf, sizeof buf), "HDR2HELLO-2!!") == 0, "恢复的是旧档内容");
    }

    printf("== 4. 掉电模拟 B: 换完了但 .old 没删 (两者都在) ==\n");
    {
        write_raw(old, "STALE-OLD");
        os_fs_rec_t r = os_file_recover_auto(path);
        /* 第4回合保守化: 分不清新旧谁更完整 ⇒ 一律保留, 返回 NONE */
        CHECK(r == OS_FS_REC_NONE, "返回 NONE (保留 .old, 不再判 CLEANED)");
        CHECK(exists(old), ".old 保留 (可能是唯一完好副本)");
        char buf[64];
        CHECK(strcmp(read_raw(path, buf, sizeof buf), "HDR2HELLO-2!!") == 0, "正式文件未被旧的顶掉");
    }

    printf("== 5. 掉电模拟 C: 首次写只落了 .tmp (无正式无 .old) ==\n");
    {
        (void)system("rm -f /tmp/osfile_test/*");
        write_raw(tmp, "ONLY-TMP");
        os_fs_rec_t r = os_file_recover_auto(path);
        /* 第4回合: 不提升 .tmp (无法区分"写完"与"写一半") */
        CHECK(r == OS_FS_REC_NONE, "返回 NONE (不提升 tmp)");
        CHECK(!exists(path) && exists(tmp), ".tmp 原样保留, 不变成正式存档");
        char buf[64];
        CHECK(strcmp(read_raw(tmp, buf, sizeof buf), "ONLY-TMP") == 0, ".tmp 内容未被改动");
    }

    printf("== 6. 干净状态: recover 不改任何东西 ==\n");
    {
        (void)system("rm -f /tmp/osfile_test/*");
        write_raw(path, "CLEAN");
        os_fs_rec_t r = os_file_recover_auto(path);
        CHECK(r == OS_FS_REC_NONE, "返回 NONE");
        char buf[64];
        CHECK(strcmp(read_raw(path, buf, sizeof buf), "CLEAN") == 0, "内容未变");
    }

    printf("== 7. 边界: 空/超长路径 ==\n");
    {
        CHECK(os_file_write_atomic(NULL, "a", 1, "b", 1) == -1, "path=NULL ⇒ -1");
        CHECK(os_file_write_atomic("", "a", 1, "b", 1) == -1, "path=\"\" ⇒ -1");
        char longp[600];
        memset(longp, 'x', sizeof longp - 1);
        longp[sizeof longp - 1] = 0;
        CHECK(os_file_write_atomic(longp, "a", 1, "b", 1) == -1, "超长路径 ⇒ -1 (不截断)");
        CHECK(os_file_recover_auto(NULL) == OS_FS_REC_NONE, "recover(NULL) ⇒ NONE");
    }

    printf("== 8. 只有 body, 无 hdr ==\n");
    {
        (void)system("rm -f /tmp/osfile_test/*");
        int rc = os_file_write_atomic(path, NULL, 0, "BODYONLY", 8);
        CHECK(rc == 0, "hdr=NULL 可写");
        CHECK(fsize(path) == 8, "长度 = body");
    }

    (void)system("rm -rf /tmp/osfile_test");
    printf("\n%s (fail=%d)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
