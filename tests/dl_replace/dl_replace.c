/* =====================================================================
 * dl_replace.c — 落盘替换 / 掉电恢复 的主机侧验证
 *
 * ★ 关键区别（相对上一版）：**不再复制生产代码**。
 *   本程序 `#include "netc_fs_replace.h"`，并与
 *   `components/services/net_client/netc_fs_replace.c` **一起编译**。
 *   断言的就是固件里跑的同一份源码；改生产代码，测试立刻跟着变，
 *   不存在"测试副本漂移"。
 *
 *   为什么能这么做：netc_fs_replace.c 不 include 任何 ESP-IDF 头文件
 *   （只用了 stdio / sys/stat），主机 clang 可以直接编。
 *
 *   ⚠ 未覆盖的部分（诚实声明，不伪称全覆盖）：
 *     - am_store.c 里"遍历目录 + 按 *.lua.bak 过滤"的那层胶水（十几行）不在本测试里；
 *       本测试用 scan_dir() 复刻同一条过滤规则（见该函数注释）；
 *     - net_client.c 里的 HTTP/CRC 闸门依赖 ESP-IDF，主机编不了，只能靠
 *       代码审阅 + 模拟器回归，本轮不声称被覆盖。
 *
 * 与 FatFs 的关系：测试跑在 POSIX rename 上，但被测实现**从不依赖 rename 覆盖**
 *   （覆盖前一定先把旧文件改名腾位），所以两者语义差异不影响结论；
 *   FatFs 特有行为（目标存在返回 FR_EXIST）由"先腾位"这个设计本身规避。
 *
 * 编译运行（macOS）：
 *   CC=/Library/Developer/CommandLineTools/usr/bin/clang
 *   SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
 *   INC=components/services/net_client
 *   $CC -isysroot $SDK -O2 -std=c11 -Wall -Wextra -I $INC \
 *       tests/dl_replace/dl_replace.c $INC/netc_fs_replace.c -o /tmp/dl_replace
 *   /tmp/dl_replace
 * ===================================================================== */
#include "netc_fs_replace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

static int  g_pass, g_fail;
static char g_dir[128];

#define CHECK(cond, fmt, ...) do {                                        \
    if (cond) { g_pass++; printf("  PASS  " fmt "\n", ##__VA_ARGS__); }   \
    else      { g_fail++; printf("  FAIL  " fmt "\n", ##__VA_ARGS__); }   \
} while (0)

/* ------------------------------ 脚手架 ------------------------------ */
static void pathof(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", g_dir, name);
}

static void bakof(char *out, size_t cap, const char *base)
{
    snprintf(out, cap, "%s/%s.bak", g_dir, base);
}

static void write_file(const char *p, const char *content)
{
    FILE *f = fopen(p, "wb");
    if (!f) { printf("  !! 无法创建 %s\n", p); return; }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static long size_of(const char *p)
{
    struct stat st;
    return (stat(p, &st) == 0) ? (long)st.st_size : -1;
}

static bool file_has(const char *p, const char *expect)
{
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return strcmp(buf, expect) == 0;
}

static void reset_dir(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "chmod -R u+w '%s' 2>/dev/null; rm -rf '%s'", g_dir, g_dir);
    if (system(cmd) != 0) { /* 清理失败不影响断言 */ }
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", g_dir);
    if (system(cmd) != 0) { }
}

/* =========================================================================
 * 扫描阶段恢复：复刻 am_store.c 里 am_store_recover_baks() 的过滤规则
 * （"*.lua.bak" 且路径里无 '/' → 用正式名调恢复）。
 * 规则本身只是字符串判断；真正的状态机 netc_fs_recover() 来自生产源码。
 * 返回处理过的个数（RESTORED / CLEANED / FAILED 都算）。
 * ========================================================================= */
static int scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t len = strlen(nm);
        if (len <= 8 || strcasecmp(nm + len - 8, ".lua.bak") != 0) continue;
        if (strchr(nm, '/')) continue;
        char fin[256], bak[256];
        snprintf(fin, sizeof(fin), "%s/%.*s", dir, (int)len - 4, nm);
        snprintf(bak, sizeof(bak), "%s/%s", dir, nm);
        if (netc_fs_recover(fin, bak) != NETC_REC_NONE) n++;
    }
    closedir(d);
    return n;
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/lintos_dltest");
    printf("=== 落盘替换 / 掉电恢复 · 主机测试（直接编译生产源码）===\n");
    printf("被测实现: components/services/net_client/netc_fs_replace.c\n");
    printf("语义前提: 目标存在时 rename 不可覆盖(FatFs ff.c:5244) —— 本实现从不依赖覆盖\n\n");

    char part[160], fin[160], bak[160];

    /* ---------- T1 首次下载（目标不存在） ---------- */
    printf("T1 首次下载: 目标不存在\n");
    reset_dir();
    pathof(part, sizeof(part), "pkg.lua.part");
    pathof(fin,  sizeof(fin),  "pkg.lua");
    bakof(bak,   sizeof(bak),  "pkg.lua");
    write_file(part, "NEW-v2");
    bool ok = netc_fs_replace(part, fin, bak);
    CHECK(ok, "替换返回成功");
    CHECK(file_has(fin, "NEW-v2"), "最终文件 = 新内容");
    CHECK(size_of(part) < 0, ".part 已不存在");
    CHECK(size_of(bak) < 0, ".bak 未产生");
    printf("\n");

    /* ---------- T2 覆盖更新（目标已存在） ---------- */
    printf("T2 覆盖更新: 目标已存在(旧版 OLD-v1)\n");
    reset_dir();
    write_file(fin, "OLD-v1");
    write_file(part, "NEW-v2");
    ok = netc_fs_replace(part, fin, bak);
    CHECK(ok, "替换成功(FatFs 直接 rename 到已存在目标会失败)");
    CHECK(file_has(fin, "NEW-v2"), "最终文件 = 新内容(旧版被替换)");
    CHECK(size_of(bak) < 0, "成功后 .bak 已清理");
    CHECK(size_of(part) < 0, ".part 已清理");
    printf("\n");

    /* ---------- T3 旧文件挪不走（只读目录 ⇒ FR_DENIED） ---------- */
    printf("T3 旧文件挪不动(目录只读): 必须保留旧版\n");
    reset_dir();
    write_file(fin, "OLD-v1");
    write_file(part, "NEW-v2");
    if (chmod(g_dir, 0555) != 0) printf("  (提示: chmod 失败, 本用例结论可能无效)\n");
    ok = netc_fs_replace(part, fin, bak);
    chmod(g_dir, 0755);
    CHECK(!ok, "替换返回失败");
    CHECK(file_has(fin, "OLD-v1"), "★ 旧版本内容完好 (没有被抹掉)");
    CHECK(size_of(part) >= 0, ".part 仍在(只读卷删不掉, 与 FatFs FR_DENIED 一致)");
    printf("\n");

    /* ---------- T4 掉电窗口：旧版已改名成 .bak，新版未就位 ---------- */
    printf("T4 掉电窗口: 正式文件缺失 + .bak 完整\n");
    reset_dir();
    write_file(bak, "OLD-v1");                  /* 模拟"已挪成 .bak" */
    CHECK(size_of(fin) < 0, "此刻正式文件确实缺失");
    netc_rec_t r = netc_fs_recover(fin, bak);
    CHECK(r == NETC_REC_RESTORED, "恢复结果 = RESTORED (实际 %d)", (int)r);
    CHECK(file_has(fin, "OLD-v1"), "★ 恢复后正式文件 = 旧版内容");
    CHECK(size_of(bak) < 0, ".bak 已被改名消耗");
    printf("\n");

    /* ---------- T5 正式文件优先：final 与 .bak 并存 ---------- */
    printf("T5 正式文件优先: final 与 .bak 同时存在\n");
    reset_dir();
    write_file(fin, "NEW-v2");
    write_file(bak, "OLD-v1");
    r = netc_fs_recover(fin, bak);
    CHECK(r == NETC_REC_CLEANED, "结果 = CLEANED (实际 %d)", (int)r);
    CHECK(file_has(fin, "NEW-v2"), "★ 正式文件保持新版, 未被旧备份覆盖");
    CHECK(size_of(bak) < 0, "陈旧 .bak 被清理");
    printf("\n");

    /* ---------- T6 下载失败：只有 .part，正式文件不动 ---------- */
    printf("T6 下载/校验失败: 只有半截 .part\n");
    reset_dir();
    write_file(fin, "OLD-v1");
    write_file(part, "TRUNCATED");
    /* 失败路径在 netc_task 里: else { remove(part_path); } —— 不经过 replace */
    remove(part);
    CHECK(file_has(fin, "OLD-v1"), "★ 旧版本完好");
    CHECK(size_of(part) < 0, ".part 已删除");
    r = netc_fs_recover(fin, bak);
    CHECK(r == NETC_REC_NONE, "恢复逻辑不碰正式文件 (NONE, 实际 %d)", (int)r);
    printf("\n");

    /* ---------- T7 扫描阶段恢复：目录里有 xxx.lua.bak，正式文件缺失 ---------- */
    printf("T7 扫描阶段恢复: 目录里有 pkg.lua.bak, pkg.lua 缺失\n");
    reset_dir();
    write_file(bak, "OLD-v1");
    int n = scan_dir(g_dir);
    CHECK(n == 1, "扫描处理了 1 个 .bak (实际 %d)", n);
    CHECK(file_has(fin, "OLD-v1"), "★ 扫描后正式文件已复位");
    CHECK(size_of(bak) < 0, ".bak 消失(改名消耗)");
    printf("\n");

    /* ---------- T8 扫描幂等：已恢复后再扫一次不应做任何事 ---------- */
    printf("T8 扫描幂等: 已恢复后再扫一次\n");
    n = scan_dir(g_dir);
    CHECK(n == 0, "第二次扫描无动作 (实际 %d)", n);
    CHECK(file_has(fin, "OLD-v1"), "正式文件内容不变");
    printf("\n");

    /* ---------- T9 孤立 .part：既非 .bak 也不该被当包 ---------- */
    printf("T9 孤立 .part: 不得当成可安装包, 也不得删除\n");
    reset_dir();
    pathof(part, sizeof(part), "orphan.lua.part");
    write_file(part, "HALF");
    n = scan_dir(g_dir);                        /* 扫描只认 *.lua.bak */
    CHECK(n == 0, "扫描未处理任何 .bak (实际 %d)", n);
    CHECK(size_of(part) >= 0, "★ .part 原样保留(可能正在下载, 删了会毁掉别人的传输)");
    {
        const char *nm = "orphan.lua.part";
        size_t l = strlen(nm);
        CHECK(l > 4 && strcasecmp(nm + l - 4, ".lua") != 0,
              "打包过滤规则: 结尾不是 .lua ⇒ 不会被列为可安装包");
    }
    printf("\n");

    /* ---------- T10 非 *.lua.bak 的备份：扫描不碰 ---------- */
    printf("T10 非 *.lua.bak: 扫描不处理\n");
    reset_dir();
    write_file(bak, "OTHER");                   /* pkg.lua.bak */
    char other[160];
    pathof(other, sizeof(other), "note.txt.bak");
    write_file(other, "TXT");
    n = scan_dir(g_dir);
    CHECK(n == 1, "只处理了 pkg.lua.bak 这一个 (实际 %d)", n);
    CHECK(size_of(other) >= 0, "note.txt.bak 未被处理(不在规则内)");
    printf("\n");

    /* ---------- T11 空参数 / 无 .bak 的快速返回 ---------- */
    printf("T11 边界: 无 .bak 时快速返回, 空参数安全\n");
    reset_dir();
    write_file(fin, "NEW-v2");
    r = netc_fs_recover(fin, bak);              /* bak 不存在 */
    CHECK(r == NETC_REC_NONE, "无 .bak ⇒ NONE (实际 %d)", (int)r);
    CHECK(netc_fs_recover(NULL, bak) == NETC_REC_NONE, "final=NULL ⇒ NONE");
    CHECK(netc_fs_recover(fin, NULL) == NETC_REC_NONE, "bak=NULL ⇒ NONE");
    CHECK(!netc_fs_replace(NULL, fin, bak), "replace(part=NULL) ⇒ false");
    CHECK(file_has(fin, "NEW-v2"), "非法参数不会破坏正式文件");
    printf("\n");

    printf("================ 结果: %d PASS / %d FAIL ================\n", g_pass, g_fail);
    reset_dir();
    return g_fail ? 1 : 0;
}
