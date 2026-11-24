/* os_file_fault_test.c — os_file.c 完整主机测试 (正常 / 故障注入 / 恢复矩阵 / 阶段中断)
 *
 * 测的是**生产源码** components/libs/osutil/os_file.c。
 * 生产代码通过 -include stub/fault_inject.h 把 libc 调用改道到 fi_*,
 * 从而在不修改生产代码的前提下注入失败。本文件用 FI_NO_REDIRECT 只取原型,
 * 自己的 fopen/rename 仍走真实 libc (否则没法准备与检查文件)。
 *
 * 编译见同目录 run_all.sh。
 *
 * ⚠ 能力边界 (每次都要如实说明):
 *   · 主机是 POSIX, rename 会**覆盖**已存在目标; 真机 FatFs 的 f_rename 返回
 *     FR_EXIST (ff.c:5243-5245)。所以这里能证明**逻辑分支**正确, 不能证明
 *     "真机上覆盖成功" —— 后者依赖 os_file_replace 的"先挪旧再换新"顺序
 *     (该顺序本身已在用例里被观测到: 先 rename(final,old) 再 rename(tmp,final))。
 *   · 断电是物理事件, 这里只验证"断电留下的磁盘状态"被如何解释, 不模拟掉电本身。
 */
#include "os_file.h"

#define FI_NO_REDIRECT 1
#include "stub/fault_inject.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BASE "/tmp/osfile_fault"

static int g_pass = 0, g_fail = 0;
static const char *g_sec = "-";

#define SEC(name) do { g_sec = (name); printf("\n== %s ==\n", (name)); } while (0)
#define CK(cond) do { \
        if (cond) { g_pass++; } \
        else { g_fail++; printf("  FAIL [%s] %s:%d  %s\n", g_sec, __func__, __LINE__, #cond); } \
    } while (0)

/* ---------------- 文件工具 (真实 libc) ---------------- */

static bool ex(const char *p) { struct stat st; return p && stat(p, &st) == 0; }
static long fsz(const char *p) { struct stat st; return (p && stat(p, &st) == 0) ? (long)st.st_size : -1; }

static void wf(const char *p, const void *d, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (!f) { printf("  !! 工具写入失败: %s\n", p); return; }
    if (n) (void)fwrite(d, 1, n, f);
    (void)fclose(f);
}

static void wfs(const char *p, const char *s) { wf(p, s, strlen(s)); }

/* 读回内容; 返回读到的字节数, 缓冲保证以 0 结尾 */
static long rf(const char *p, char *buf, size_t cap)
{
    if (cap == 0) return -1;
    FILE *f = fopen(p, "rb");
    if (!f) { buf[0] = 0; return -1; }
    size_t n = fread(buf, 1, cap - 1, f);
    (void)fclose(f);
    buf[n] = 0;
    return (long)n;
}

static bool content_is(const char *p, const char *want)
{
    char buf[8192];
    long n = rf(p, buf, sizeof buf);
    if (n < 0) return false;
    return strcmp(buf, want) == 0;
}

static void clean_dir(void)
{
    (void)system("rm -rf " BASE " && mkdir -p " BASE);
}

typedef struct { char f[320], t[320], o[320]; } paths_t;

static void mk_paths(paths_t *p, const char *name)
{
    snprintf(p->f, sizeof p->f, "%s/%s", BASE, name);
    snprintf(p->t, sizeof p->t, "%s/%s.tmp", BASE, name);
    snprintf(p->o, sizeof p->o, "%s/%s.old", BASE, name);
}

/* ==================================================================
 * 3.1 正常操作
 * ================================================================== */

static void t_normal(void)
{
    paths_t p;
    char buf[8192];

    SEC("3.1.1 新文件写入 (无旧文件)");
    clean_dir(); mk_paths(&p, "n1.dat");
    CK(os_file_write_atomic(p.f, "HDR!", 4, "BODY", 4) == 0);
    CK(fsz(p.f) == 8L);
    CK(!ex(p.t) && !ex(p.o));
    CK(content_is(p.f, "HDR!BODY"));

    SEC("3.1.2 已有文件替换");
    clean_dir(); mk_paths(&p, "n2.dat");
    wfs(p.f, "OLD-VERSION");
    CK(os_file_write_atomic(p.f, "H", 1, "NEW-VERSION", 11) == 0);
    CK(content_is(p.f, "HNEW-VERSION"));
    CK(fsz(p.f) == 12L);
    CK(!ex(p.t) && !ex(p.o));

    SEC("3.1.3 空文件 (hdr=0, body=0)");
    clean_dir(); mk_paths(&p, "n3.dat");
    CK(os_file_write_atomic(p.f, NULL, 0, NULL, 0) == 0);
    CK(fsz(p.f) == 0L);
    CK(ex(p.f) && !ex(p.t) && !ex(p.o));

    SEC("3.1.4 只有 hdr, 无 body");
    clean_dir(); mk_paths(&p, "n4.dat");
    CK(os_file_write_atomic(p.f, "ABC", 3, NULL, 0) == 0);
    CK(fsz(p.f) == 3L);
    CK(content_is(p.f, "ABC"));

    SEC("3.1.5 只有 body, 无 hdr");
    clean_dir(); mk_paths(&p, "n5.dat");
    CK(os_file_write_atomic(p.f, NULL, 0, "BODYONLY", 8) == 0);
    CK(fsz(p.f) == 8L);
    CK(content_is(p.f, "BODYONLY"));

    SEC("3.1.6 大文件 (4 MiB, 内容可校验)");
    clean_dir(); mk_paths(&p, "n6.dat");
    {
        const size_t BIG = 4u * 1024 * 1024;
        unsigned char *big = (unsigned char *)malloc(BIG);
        CK(big != NULL);
        if (big) {
            for (size_t i = 0; i < BIG; i++) big[i] = (unsigned char)((i * 31u + 7u) & 0xFF);
            CK(os_file_write_atomic(p.f, "BIG0", 4, big, BIG) == 0);
            CK(fsz(p.f) == (long)(BIG + 4));
            /* 抽取校验: 头部 4 字节 + 若干随机位置 */
            FILE *f = fopen(p.f, "rb");
            CK(f != NULL);
            if (f) {
                unsigned char h[4] = {0};
                CK(fread(h, 1, 4, f) == 4);
                CK(memcmp(h, "BIG0", 4) == 0);
                int spot_ok = 1;
                const size_t spots[] = {0, 1, 1023, 65535, 1048575, BIG - 1};
                for (size_t k = 0; k < sizeof(spots) / sizeof(spots[0]); k++) {
                    size_t off = 4 + spots[k];
                    if (off >= BIG) continue;
                    if (fseek(f, (long)off, SEEK_SET) != 0) { spot_ok = 0; break; }
                    int c = fgetc(f);
                    unsigned char want = (unsigned char)((spots[k] * 31u + 7u) & 0xFF);
                    if (c != (int)want) { spot_ok = 0; break; }
                }
                CK(spot_ok);
                (void)fclose(f);
            }
            free(big);
        }
    }

    SEC("3.1.7 多次覆盖 (20 轮, 每轮内容不同)");
    clean_dir(); mk_paths(&p, "n7.dat");
    {
        int all_ok = 1;
        for (int i = 0; i < 20; i++) {
            char want[64];
            int wl = snprintf(want, sizeof want, "ROUND-%02d-PAYLOAD", i);
            if (os_file_write_atomic(p.f, NULL, 0, want, (size_t)wl) != 0) { all_ok = 0; break; }
            if (!content_is(p.f, want)) { all_ok = 0; break; }
            if (ex(p.t) || ex(p.o)) { all_ok = 0; break; }
        }
        CK(all_ok);
    }

    SEC("3.1.8 连续替换 (直接调 os_file_replace)");
    clean_dir(); mk_paths(&p, "n8.dat");
    wfs(p.f, "V0");
    wfs(p.t, "V1");
    CK(os_file_replace(p.t, p.f, p.o));
    CK(content_is(p.f, "V1"));
    CK(!ex(p.t));
    CK(!ex(p.o));                       /* 成功后才删 .old */

    SEC("3.1.9 同一文件重复 recover (10 次, 幂等)");
    clean_dir(); mk_paths(&p, "n9.dat");
    wfs(p.f, "STABLE");
    {
        int idem = 1;
        for (int i = 0; i < 10; i++)
            if (os_file_recover_auto(p.f) != OS_FS_REC_NONE) { idem = 0; break; }
        CK(idem);
        CK(content_is(p.f, "STABLE"));
        CK(!ex(p.t) && !ex(p.o));
    }

    SEC("3.1.10 文件不存在 → recover 不造文件");
    clean_dir(); mk_paths(&p, "ghost.dat");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(!ex(p.f) && !ex(p.t) && !ex(p.o));

    SEC("3.1.11 目录不存在 → 写入失败且不留垃圾");
    clean_dir();
    {
        char bad[320];
        snprintf(bad, sizeof bad, "%s/no_such_dir/x.dat", BASE);
        int rc = os_file_write_atomic(bad, "a", 1, "b", 1);
        CK(rc < 0);                      /* fopen 失败 ⇒ -2 */
        CK(!ex(bad));
    }

    SEC("3.1.12 路径长度边界 (OS_FILE_PATH_MAX=256)");
    clean_dir();
    {
        /* BASE 长度 + 1 + 文件名长度 = 总长;  ".tmp"/".old" 后缀 4 字节。
         * 总长 <= 251 才能拼下后缀 ⇒ 成功; 252 ⇒ snprintf 截断判定 ⇒ -1。 */
        const size_t baselen = strlen(BASE) + 1;
        size_t ok_name = 251 - baselen;
        size_t ng_name = 252 - baselen;

        char *nm = (char *)malloc(ok_name + 1);
        if (nm) {
            memset(nm, 'a', ok_name); nm[ok_name] = 0;
            char full[400];
            snprintf(full, sizeof full, "%s/%s", BASE, nm);
            CK(strlen(full) == 251);
            CK(os_file_write_atomic(full, NULL, 0, "L", 1) == 0);
            CK(fsz(full) == 1L);
            free(nm);
        }

        nm = (char *)malloc(ng_name + 1);
        if (nm) {
            memset(nm, 'a', ng_name); nm[ng_name] = 0;
            char full[400];
            snprintf(full, sizeof full, "%s/%s", BASE, nm);
            CK(strlen(full) == 252);
            CK(os_file_write_atomic(full, NULL, 0, "L", 1) == -1);   /* 拒绝, 不截断 */
            free(nm);
        }
    }

    SEC("3.1.13 非法参数");
    clean_dir();
    CK(os_file_write_atomic(NULL, "a", 1, "b", 1) == -1);
    CK(os_file_write_atomic("", "a", 1, "b", 1) == -1);
    CK(os_file_write_atomic("/tmp/x", NULL, 5, NULL, 0) == -1);   /* 长度>0 但指针为 NULL */
    CK(os_file_write_atomic("/tmp/x", NULL, 0, NULL, 5) == -1);   /* 同上, body */
    /* ⚠ 契约边界: "指针非 NULL 但长度超过实际缓冲区"**不在防御范围** ——
     *   C API 拿不到缓冲区真实大小, 传错长度就是越界读, 属调用方责任。
     *   这里不构造该场景 (ASan 会直接报错), 只显式记录边界。 */
    CK(os_file_recover_auto(NULL) == OS_FS_REC_NONE);
    CK(os_file_recover_auto("") == OS_FS_REC_NONE);

    (void)buf;
}

/* ==================================================================
 * 3.2 故障注入
 * ================================================================== */

static void t_fault(void)
{
    paths_t p;

    /* 每个用例: 干净目录 + 一份旧档, 便于断言"旧档未被破坏" */
#define FRESH(name) do { fi_reset(); clean_dir(); mk_paths(&p, (name)); wfs(p.f, "OLD-GOOD"); } while (0)

    SEC("3.2.1 临时文件创建失败 (fopen)");
    FRESH("f1.dat");
    fi_fail_always("fopen");
    CK(os_file_write_atomic(p.f, "N", 1, "NEW", 3) == -2);
    CK(content_is(p.f, "OLD-GOOD"));
    CK(!ex(p.t) && !ex(p.o));

    SEC("3.2.2 写 hdr 短写 (fwrite)");
    FRESH("f2.dat");
    fi_set_short_write(2);
    CK(os_file_write_atomic(p.f, "HDRX", 4, "BODY", 4) == -3);
    CK(content_is(p.f, "OLD-GOOD"));
    CK(!ex(p.t) && !ex(p.o));

    SEC("3.2.3 写 body 短写 (无 hdr)");
    FRESH("f3.dat");
    fi_set_short_write(3);
    CK(os_file_write_atomic(p.f, NULL, 0, "BODYBODY", 8) == -3);
    CK(content_is(p.f, "OLD-GOOD"));
    CK(!ex(p.t));

    SEC("3.2.4 fflush 失败");
    FRESH("f4.dat");
    fi_fail_always("fflush");
    CK(os_file_write_atomic(p.f, NULL, 0, "NEW", 3) == -3);
    CK(content_is(p.f, "OLD-GOOD"));
    CK(!ex(p.t));

    SEC("3.2.5 fclose 失败 (卡满/拔卡典型)");
    FRESH("f5.dat");
    fi_fail_always("fclose");
    CK(os_file_write_atomic(p.f, NULL, 0, "NEW", 3) == -3);
    CK(content_is(p.f, "OLD-GOOD"));
    CK(!ex(p.t));
    CK(!ex(p.o));

    SEC("3.2.6 rename(tmp→final) 失败 → 旧档回滚");
    FRESH("f6.dat");
    /* rename 第 1 次是 final→old, 第 2 次是 tmp→final。
     * 用 fail_once: 只打掉第 2 次, 让第 3 次(回滚 old→final)能成功。 */
    fi_fail_once("rename", 2);
    CK(os_file_write_atomic(p.f, NULL, 0, "NEW", 3) == -4);
    CK(content_is(p.f, "OLD-GOOD"));    /* 回滚成功 */
    CK(!ex(p.t));
    CK(!ex(p.o));                       /* 回滚时 old 已被 rename 回 final */

    SEC("3.2.6b 回滚本身也失败 (最坏情况) → 数据退到 .old, 仍可恢复");
    FRESH("f6b.dat");
    fi_fail_at("rename", 2);            /* 第 2、3 次 rename 全失败 */
    CK(os_file_write_atomic(p.f, NULL, 0, "NEW", 3) == -4);
    CK(!ex(p.f));                       /* 正式档缺失 */
    CK(!ex(p.t));                       /* tmp 已清 */
    CK(ex(p.o) && content_is(p.o, "OLD-GOOD"));   /* 旧档退守 .old —— 数据没丢 */
    fi_reset();
    /* 关键: 下一个启动周期的 recover 必须能把它救回来 */
    CK(os_file_recover_auto(p.f) == OS_FS_REC_RESTORED);
    CK(content_is(p.f, "OLD-GOOD"));

    SEC("3.2.7 rename(final→old) 失败 → 放弃本次替换, final 不动");
    FRESH("f7.dat");
    fi_fail_at("rename", 1);
    CK(os_file_write_atomic(p.f, NULL, 0, "NEW", 3) == -4);
    CK(content_is(p.f, "OLD-GOOD"));
    CK(!ex(p.t));
    CK(!ex(p.o));

    SEC("3.2.8 remove 陈旧 .old 失败 → 不影响最终成功");
    FRESH("f8.dat");
    /* remove 调用序: ① remove(tmp) ② replace 内 remove(old)。
     * 让第 2 次失败 = "清陈旧 .old" 失败。POSIX rename 可覆盖, 最终仍成功。 */
    fi_fail_at("remove", 2);
    CK(os_file_write_atomic(p.f, NULL, 0, "NEW", 3) == 0);
    CK(content_is(p.f, "NEW"));
    /* ⚠ 这里主机通过, 但真机 FatFs 会因 rename 目标存在而 FR_EXIST ⇒ 本用例
     *   证明不了真机行为。真机上该分支由"陈旧 .old 已被前一次成功流程删掉"
     *   保证不发生; 只有掉电残留才会留下 .old, 那种情况由 recover 先处理。 */
    fi_reset();

    SEC("3.2.9 正式文件不存在时的 replace (无旧档可留)");
    FRESH("f9.dat");
    (void)remove(p.f);                  /* 变成本次首次落盘 */
    wfs(p.t, "FIRST");
    CK(os_file_replace(p.t, p.f, p.o));
    CK(content_is(p.f, "FIRST"));
    CK(!ex(p.t) && !ex(p.o));

    SEC("3.2.10 首次落盘 rename 失败 → tmp 清理, final 仍未创建");
    FRESH("fa.dat");
    (void)remove(p.f);
    fi_fail_at("rename", 1);
    CK(os_file_write_atomic(p.f, NULL, 0, "NEW", 3) == -4);
    CK(!ex(p.f));
    CK(!ex(p.t));
    CK(!ex(p.o));

    SEC("3.2.11 失败后再次写入 → 必须能成功 (不能留下永久坏死状态)");
    FRESH("fb.dat");
    fi_fail_once("rename", 2);
    CK(os_file_write_atomic(p.f, NULL, 0, "BAD", 3) == -4);
    fi_reset();
    CK(os_file_write_atomic(p.f, NULL, 0, "RETRY-OK", 8) == 0);
    CK(content_is(p.f, "RETRY-OK"));
    CK(!ex(p.t) && !ex(p.o));

    SEC("3.2.12 空洞文件/半截 tmp 不阻断下一次写入");
    FRESH("fc.dat");
    wfs(p.t, "HALF");                   /* 上次掉电留下的 .tmp */
    fi_reset();
    CK(os_file_write_atomic(p.f, NULL, 0, "FULL-DATA", 9) == 0);
    CK(content_is(p.f, "FULL-DATA"));
    CK(!ex(p.t));

    SEC("3.2.13 正式档缺失 + 陈旧 .old, 不先 recover 直接写 → 观察 .old 残留");
    /* ★ 记录一个**真实但轻微**的行为 (非崩溃/非丢数据, 由本测试暴露):
     *   os_file_replace 只在 "final 存在" 分支里 remove(old_path)。
     *   若 final 缺失(掉电) 而 .old 还在, 直接写入会:
     *     rename(tmp→final) 成功, 但陈旧的 .old 不会被清理 ⇒ 残留一份旧档。
     *   之后 recover 走"正式文件非空 ⇒ 保留 .old"的保守分支, 于是它长期留着,
     *   直到该文件下一次"覆盖已存在正式档"的写入才被清掉。
     *   代价: 一个存档的空间 (几十 KB ~ 几百 KB); 不影响正确性。
     *   规避: 调用方在写入前先 os_file_recover_auto() (svc_backup 读取路径已这么做)。
     *   本用例**记录现状**, 不判定为失败 —— 若将来决定在 replace 里补清理, 把
     *   下面的 CK(ex(p.o)) 改成 CK(!ex(p.o)) 即为回归门禁。 */
    FRESH("fd.dat");
    (void)remove(p.f);                  /* 模拟掉电: 正式档没了, .old 还在 */
    wfs(p.o, "OLD-GOOD");
    fi_reset();
    CK(os_file_write_atomic(p.f, NULL, 0, "NEWEST", 6) == 0);
    CK(content_is(p.f, "NEWEST"));
    CK(ex(p.o) && content_is(p.o, "OLD-GOOD"));   /* ← 现状: 残留 */
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, "NEWEST"));                /* 保守分支不误伤新档 */
    CK(ex(p.o) && content_is(p.o, "OLD-GOOD"));   /* recover 也不清 (设计如此) */

#undef FRESH
}

/* ==================================================================
 * 3.3 恢复矩阵
 * ================================================================== */

static void t_recover(void)
{
    paths_t p;
    clean_dir();

    /* 统一内容: 正式=FORMAL, .old=OLDCOPY, .tmp=TMPCOPY */

    SEC("3.3.1 只有正式文件");
    clean_dir(); mk_paths(&p, "r1.dat");
    wfs(p.f, "FORMAL");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, "FORMAL"));
    CK(!ex(p.t) && !ex(p.o));

    SEC("3.3.2 只有 .tmp  (第4回合定: 不提升)");
    clean_dir(); mk_paths(&p, "r2.dat");
    wfs(p.t, "TMPCOPY");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(!ex(p.f));                        /* 不得把半截数据变成正式存档 */
    CK(ex(p.t));
    CK(content_is(p.t, "TMPCOPY"));

    SEC("3.3.3 只有 .old → 复位");
    clean_dir(); mk_paths(&p, "r3.dat");
    wfs(p.o, "OLDCOPY");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_RESTORED);
    CK(content_is(p.f, "OLDCOPY"));
    CK(!ex(p.o));

    SEC("3.3.4 正式 + .tmp  (两者都不动)");
    clean_dir(); mk_paths(&p, "r4.dat");
    wfs(p.f, "FORMAL"); wfs(p.t, "TMPCOPY");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, "FORMAL"));
    CK(ex(p.t) && content_is(p.t, "TMPCOPY"));

    SEC("3.3.5 正式 + .old  (保守: 保留 .old, 不动正式)");
    clean_dir(); mk_paths(&p, "r5.dat");
    wfs(p.f, "FORMAL"); wfs(p.o, "OLDCOPY");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, "FORMAL"));
    CK(ex(p.o) && content_is(p.o, "OLDCOPY"));   /* 唯一完好副本不得被删 */

    SEC("3.3.6 正式 + .tmp + .old  (三者都在)");
    clean_dir(); mk_paths(&p, "r6.dat");
    wfs(p.f, "FORMAL"); wfs(p.t, "TMPCOPY"); wfs(p.o, "OLDCOPY");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, "FORMAL"));
    CK(content_is(p.t, "TMPCOPY"));
    CK(content_is(p.o, "OLDCOPY"));

    SEC("3.3.7 正式为空 + .old → 用 .old 顶替");
    clean_dir(); mk_paths(&p, "r7.dat");
    wfs(p.f, ""); wfs(p.o, "OLDCOPY");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_RESTORED);
    CK(content_is(p.f, "OLDCOPY"));
    CK(!ex(p.o));

    SEC("3.3.8 正式损坏(非空) + .old → 保守不动 [已知代价]");
    clean_dir(); mk_paths(&p, "r8.dat");
    wfs(p.f, "\xDE\xAD\xBE\xEF-garbage");
    wfs(p.o, "OLDCOPY");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    /* ★ 这是保守策略的**已知代价**: 非空但内容损坏的正式文件不会被 .old 覆盖。
     *   理由是 recover 无法判断"谁更完整", 而覆盖会毁掉可能有效的新档。
     *   上层靠各自 magic/长度校验拒收损坏档 (见各引擎加载函数)。 */
    CK(ex(p.f) && ex(p.o));

    SEC("3.3.9 .tmp 为空");
    clean_dir(); mk_paths(&p, "r9.dat");
    wfs(p.t, "");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(!ex(p.f) && ex(p.t));

    SEC("3.3.10 .old 为空 + 正式缺失 → 复位(得到 0 字节) [记录行为]");
    clean_dir(); mk_paths(&p, "ra.dat");
    wfs(p.o, "");
    os_fs_rec_t r = os_file_recover_auto(p.f);
    CK(r == OS_FS_REC_RESTORED);
    /* ★ 记录: rename 不检查 .old 大小, 0 字节 .old 会被复位成 0 字节正式文件。
     *   实际危害有限 —— .old 只由 "rename(final→old)" 产生, 其内容恒等于当时的
     *   正式文件; 若 .old 为 0 字节, 说明掉电前正式文件本身就是空壳。
     *   上层加载函数仍会用长度/magic 拒绝。此处如实记录而非判定为缺陷。 */
    CK(fsz(p.f) == 0L);

    SEC("3.3.11 多次 recover (恢复后重复调用)");
    clean_dir(); mk_paths(&p, "rb.dat");
    wfs(p.o, "OLDCOPY");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_RESTORED);
    {
        int idem = 1;
        for (int i = 0; i < 6; i++)
            if (os_file_recover_auto(p.f) != OS_FS_REC_NONE) { idem = 0; break; }
        CK(idem);
        CK(content_is(p.f, "OLDCOPY"));
    }

    SEC("3.3.12 recover 之后再次写入");
    clean_dir(); mk_paths(&p, "rc.dat");
    wfs(p.o, "OLDCOPY");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_RESTORED);
    CK(os_file_write_atomic(p.f, NULL, 0, "NEWER", 5) == 0);
    CK(content_is(p.f, "NEWER"));
    CK(!ex(p.t) && !ex(p.o));

    SEC("3.3.13 recover 之后再次替换");
    clean_dir(); mk_paths(&p, "rd.dat");
    wfs(p.o, "OLDCOPY");
    CK(os_file_recover_auto(p.f) == OS_FS_REC_RESTORED);
    wfs(p.t, "REPLACED");
    CK(os_file_replace(p.t, p.f, p.o));
    CK(content_is(p.f, "REPLACED"));
    CK(!ex(p.o));

    SEC("3.3.14 显式路径版 recover 的入参守卫");
    clean_dir(); mk_paths(&p, "re.dat");
    wfs(p.o, "OLDCOPY");
    CK(os_file_recover(NULL, p.o) == OS_FS_REC_NONE);
    CK(os_file_recover("", p.o) == OS_FS_REC_NONE);
    CK(os_file_recover(p.f, NULL) == OS_FS_REC_NONE);
    CK(os_file_recover(p.f, "") == OS_FS_REC_NONE);
    CK(ex(p.o));                         /* 入参非法时不得动文件 */
}

/* ==================================================================
 * 3.4 十阶段中断模拟
 *
 * 模拟 os_file_write_atomic（旧档 OLD-DATA-123 → 新档 NEW-DATA-456）在
 * 各阶段被打断后留下的磁盘状态, 再跑 recover_auto 检查解释是否正确。
 * ================================================================== */

#define OLD_S "OLD-DATA-123"
#define NEW_S "NEW-DATA-456"

static void stage_setup(paths_t *p, const char *name, int stage)
{
    clean_dir();
    mk_paths(p, name);
    switch (stage) {
    case 1:  /* 创建临时文件之前 */
        wfs(p->f, OLD_S);
        break;
    case 2:  /* 临时文件创建之后 (0 字节) */
        wfs(p->f, OLD_S); wfs(p->t, "");
        break;
    case 3:  /* 写入过程中 (半截) */
        wfs(p->f, OLD_S); wfs(p->t, "NEW-DA");
        break;
    case 4:  /* 写入完成 */
    case 5:  /* fclose 之后 */
    case 6:  /* 原文件备份之前 */
        wfs(p->f, OLD_S); wfs(p->t, NEW_S);
        break;
    case 7:  /* 原文件移至 .old 之后 */
    case 8:  /* 临时文件替换正式之前 */
        wfs(p->o, OLD_S); wfs(p->t, NEW_S);
        break;
    case 9:  /* 替换之后 */
    case 10: /* .old 清理之前 */
        wfs(p->f, NEW_S); wfs(p->o, OLD_S);
        break;
    default:
        break;
    }
}

static void t_stages(void)
{
    paths_t p;

    SEC("3.4.1 阶段1 创建 tmp 之前 → 保持旧档");
    stage_setup(&p, "s1.dat", 1);
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, OLD_S));
    CK(!ex(p.t) && !ex(p.o));

    SEC("3.4.2 阶段2 tmp 已建(0字节) → 不提升, 旧档不动");
    stage_setup(&p, "s2.dat", 2);
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, OLD_S));
    CK(ex(p.t) && fsz(p.t) == 0L);

    SEC("3.4.3 阶段3 写入中(半截) → 绝不把半截变正式");
    stage_setup(&p, "s3.dat", 3);
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, OLD_S));
    CK(ex(p.t) && content_is(p.t, "NEW-DA"));

    SEC("3.4.4 阶段4 写入完成 → 旧档仍是正式, tmp 保留");
    stage_setup(&p, "s4.dat", 4);
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, OLD_S));
    CK(content_is(p.t, NEW_S));

    SEC("3.4.5 阶段5 fclose 之后 → 同上");
    stage_setup(&p, "s5.dat", 5);
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, OLD_S));
    CK(content_is(p.t, NEW_S));

    SEC("3.4.6 阶段6 备份原文件之前 → 同上");
    stage_setup(&p, "s6.dat", 6);
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, OLD_S));
    CK(content_is(p.t, NEW_S));

    SEC("3.4.7 阶段7 原文件已挪 .old → 旧档复位, 新档留在 tmp");
    stage_setup(&p, "s7.dat", 7);
    CK(os_file_recover_auto(p.f) == OS_FS_REC_RESTORED);
    CK(content_is(p.f, OLD_S));
    CK(!ex(p.o));
    CK(content_is(p.t, NEW_S));         /* 不提升, 但也绝不删 */

    SEC("3.4.8 阶段8 tmp 就位之前 → 同阶段7");
    stage_setup(&p, "s8.dat", 8);
    CK(os_file_recover_auto(p.f) == OS_FS_REC_RESTORED);
    CK(content_is(p.f, OLD_S));
    CK(content_is(p.t, NEW_S));

    SEC("3.4.9 阶段9 替换之后 → 新档生效, .old 保留");
    stage_setup(&p, "s9.dat", 9);
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, NEW_S));
    CK(content_is(p.o, OLD_S));         /* 保守: 不替用户删 */

    SEC("3.4.10 阶段10 .old 清理之前 → 同上");
    stage_setup(&p, "sa.dat", 10);
    CK(os_file_recover_auto(p.f) == OS_FS_REC_NONE);
    CK(content_is(p.f, NEW_S));
    CK(content_is(p.o, OLD_S));

    SEC("3.4.11 每个阶段之后都能正常再写一次 (不可恢复的残留不阻断)");
    for (int st = 1; st <= 10; st++) {
        char name[32];
        snprintf(name, sizeof name, "sb%d.dat", st);
        stage_setup(&p, name, st);
        (void)os_file_recover_auto(p.f);
        int rc = os_file_write_atomic(p.f, NULL, 0, "AFTER-STAGE-OK", 14);
        if (rc != 0 || !content_is(p.f, "AFTER-STAGE-OK")) {
            g_fail++;
            printf("  FAIL [3.4.11] 阶段 %d 之后重新写入失败 (rc=%d)\n", st, rc);
        } else {
            g_pass++;
        }
    }

    SEC("3.4.12 关键不变式: 任何阶段都不出现「既无正式档也无 .old」的丢档状态");
    for (int st = 1; st <= 10; st++) {
        char name[32];
        snprintf(name, sizeof name, "sc%d.dat", st);
        stage_setup(&p, name, st);
        (void)os_file_recover_auto(p.f);
        /* 阶段 1-6 有正式档; 7-10 由 .old 复位。恢复后正式档必须存在。 */
        if (!ex(p.f)) {
            g_fail++;
            printf("  FAIL [3.4.12] 阶段 %d 恢复后正式档缺失\n", st);
        } else {
            g_pass++;
        }
    }
}

int main(void)
{
    printf("os_file 主机测试 (生产源码 components/libs/osutil/os_file.c)\n");

    t_normal();
    t_fault();
    t_recover();
    t_stages();

    printf("\n---------------------------------------------\n");
    printf("PASS=%d  FAIL=%d  合计=%d\n", g_pass, g_fail, g_pass + g_fail);
    printf("%s\n", g_fail ? "结果: FAILED" : "结果: ALL PASS");

    clean_dir();
    return g_fail ? 1 : 0;
}
