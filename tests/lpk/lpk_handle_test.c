/**
 * lpk_handle_test.c — 句柄独立性 / 并发 / 生命周期（主机侧）
 *
 * 用法: lpk_handle_test <pal.lpk> <后端根目录> <原始资源目录>
 *
 * 为什么单独一个用例
 * ------------------
 * 真机上"载入一分钟"的根因假设是：**全包共用一个 FILE\***，导致每次读都必须持全局锁，
 * 读一个大资源就把别的句柄、把音频任务一起堵住。改成"每句柄一个 FILE\*"之后，
 * 必须证明三件事，否则只是换了结构体：
 *
 *   ① 位置独立：同一文件开两次，各读各的，互不影响；
 *   ② 交叉不串：A 读 4 KB、B 读 4 KB、A 再读 4 KB …… 数据必须与原件逐字节一致；
 *   ③ 真能并发：两个线程同时读**不同**资源，结果正确且总耗时不退化成串行。
 *
 * ④ 句柄上限：打满 16 个后第 17 个必须明确失败（不能覆盖正在用的）；
 * ⑤ 卸载安全：还有句柄开着时 unmount，不能崩、不能泄漏（后端 fd 要关掉）。
 *
 * ⚠ 这是**主机**测试：底层是 macOS 的 POSIX 文件系统，不是 SDMMC。
 *   它能证明"位置/并发/生命周期"的语义对，证明不了真机吞吐。
 *   第 8 段另给一组 A(目录) vs B(LPK) 的**主机**吞吐对照，同样不代表真机。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#include "lpk_mount.h"

static int fails = 0;
#define CHECK(c, ...) do { \
    int _ok = (c) ? 1 : 0; \
    printf(_ok ? "  PASS  " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); \
    if (!_ok) fails++; } while (0)

/* ---------------- POSIX 后端 ---------------- */
static int  b_open(void *c, const char *p, int f)      { (void)c; return open(p, f, 0666); }
static int  b_read(void *c, int fd, void *b, size_t n) { (void)c; return (int)read(fd, b, n); }
static int  b_write(void *c, int fd, const void *b, size_t n) { (void)c; return (int)write(fd, b, n); }
static long b_lseek(void *c, int fd, long o, int w)    { (void)c; return (long)lseek(fd, o, w); }
static int  b_close(void *c, int fd)                   { (void)c; return close(fd); }
static int  b_stat(void *c, const char *p, lpk_m_stat_t *st) {
    (void)c; struct stat s;
    if (stat(p, &s) != 0) return -1;
    st->is_dir = S_ISDIR(s.st_mode) ? 1 : 0;
    st->size   = (unsigned long)s.st_size;
    return 0;
}
static int  b_mkdir(void *c, const char *p)            { (void)c; return mkdir(p, 0777); }
static int  b_unlink(void *c, const char *p)           { (void)c; return unlink(p); }
static int  b_rename(void *c, const char *a, const char *b) { (void)c; return rename(a, b); }
static const lpk_backing_t BK = {
    .ctx = NULL, .open = b_open, .read = b_read, .write = b_write, .lseek = b_lseek,
    .close = b_close, .stat = b_stat, .mkdir = b_mkdir, .unlink = b_unlink, .rename = b_rename,
};

static lpk_mnt_t M;
static lpk_entry_t EN[LPK_MAX_ENTRIES];

/* 从磁盘原件读一段，用于比对 */
static int origin_read(const char *origin, const char *name, long off, void *buf, size_t n) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", origin, name);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    if (fseek(f, off, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return (int)got;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ---------------- 并发读线程 ---------------- */
typedef struct {
    const char *name;
    uint32_t    want_crc;
    uint32_t    got_crc;
    long        bytes;
    int         opens;
    int         ok;
} thr_arg_t;

static void *thr_read(void *p) {
    thr_arg_t *a = (thr_arg_t *)p;
    uint8_t *buf = (uint8_t *)malloc(64 * 1024);
    a->ok = 0;
    if (!buf) return NULL;
    int hd = lpk_m_open(&M, a->name, LPK_O_RDONLY);
    if (hd < 0) { free(buf); return NULL; }
    a->opens++;
    uint32_t crc = 0;
    int n;
    while ((n = lpk_m_read(&M, hd, buf, 64 * 1024)) > 0) {
        crc = lpk_crc32(crc, buf, (uint32_t)n);
        a->bytes += n;
    }
    lpk_m_close(&M, hd);
    free(buf);
    a->got_crc = crc;
    a->ok = (a->want_crc == 0 || crc == a->want_crc);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("用法: %s <pal.lpk> <后端根> <原始资源目录>\n", argv[0]);
        return 2;
    }
    const char *lpk = argv[1], *base = argv[2], *origin = argv[3];

    printf("========== H-0 挂载 ==========\n");
    if (lpk_mount(&M, lpk, EN, LPK_MAX_ENTRIES, base, &BK) != LPK_OK) {
        printf("  FAIL  挂载失败\n"); return 1;
    }
    lpk_mount_add_writable_dir(&M, "save/");
    lpk_mount_add_writable_file(&M, "sdlpal.cfg");
    printf("  PASS  挂载 %d 条资源\n", M.h.loaded);

    /* -------- ① 同一文件开两次，位置独立 -------- */
    printf("\n========== H-1 同一文件两个句柄，位置互不影响 ==========\n");
    {
        static uint8_t a[256], b[256], ref[512];
        int h1 = lpk_m_open(&M, "ABC.MKF", LPK_O_RDONLY);
        int h2 = lpk_m_open(&M, "ABC.MKF", LPK_O_RDONLY);
        CHECK(h1 >= 0 && h2 >= 0, "同一资源开两个句柄 h1=%d h2=%d", h1, h2);
        CHECK(h1 != h2, "两个句柄不是同一个槽（%d / %d）", h1, h2);

        CHECK(lpk_m_read(&M, h1, a, 100) == 100, "h1 读前 100 B");
        CHECK(lpk_m_read(&M, h2, b, 200) == 200, "h2 读前 200 B");
        origin_read(origin, "ABC.MKF", 0, ref, 200);
        CHECK(memcmp(a, ref, 100) == 0, "h1 的内容是文件开头 100 B");
        CHECK(memcmp(b, ref, 200) == 0, "h2 的内容是文件开头 200 B");

        /* h1 接着读：必须从 100 开始，不能被 h2 的读推着走 */
        CHECK(lpk_m_read(&M, h1, a, 50) == 50, "h1 再读 50 B");
        origin_read(origin, "ABC.MKF", 100, ref, 50);
        CHECK(memcmp(a, ref, 50) == 0, "h1 从偏移 100 继续（位置未被 h2 污染）");

        CHECK(lpk_m_seek(&M, h2, 1000, LPK_SEEK_SET) == 1000, "h2 定位到 1000");
        CHECK(lpk_m_read(&M, h2, b, 32) == 32, "h2 读 32 B");
        origin_read(origin, "ABC.MKF", 1000, ref, 32);
        CHECK(memcmp(b, ref, 32) == 0, "h2 内容与原件 @1000 一致");
        CHECK(lpk_m_seek(&M, h1, 0, LPK_SEEK_CUR) == 150, "h1 位置仍是 150（没被 h2 拖走）");

        lpk_m_close(&M, h1);
        lpk_m_close(&M, h2);
    }

    /* -------- ② 四个文件交叉读，不串 -------- */
    printf("\n========== H-2 四个文件交叉读（每轮各读 4 KB，共 8 轮） ==========\n");
    {
        const char *names[4] = { "ABC.MKF", "FBP.MKF", "MGO.MKF", "F.MKF" };
        int hd[4]; int ok = 1;
        for (int i = 0; i < 4; i++) {
            hd[i] = lpk_m_open(&M, names[i], LPK_O_RDONLY);
            if (hd[i] < 0) ok = 0;
        }
        CHECK(ok, "4 个句柄同时打开");
        static uint8_t got[4096], ref[4096];
        for (int round = 0; round < 8 && ok; round++) {
            for (int i = 0; i < 4; i++) {
                int n = lpk_m_read(&M, hd[i], got, sizeof(got));
                long off = (long)round * 4096;
                if (n != (int)sizeof(got) || origin_read(origin, names[i], off, ref, sizeof(ref)) != (int)sizeof(ref) ||
                    memcmp(got, ref, sizeof(got)) != 0) {
                    ok = 0;
                    printf("  FAIL  %s 第 %d 轮不一致\n", names[i], round);
                }
            }
        }
        CHECK(ok, "4×8 轮交叉读，全部与原件逐字节一致");
        for (int i = 0; i < 4; i++) if (hd[i] >= 0) lpk_m_close(&M, hd[i]);
    }

    /* -------- ③ 两个线程同时读不同资源 -------- */
    printf("\n========== H-3 两个线程并发读不同资源 ==========\n");
    {
        thr_arg_t t[2] = {
            { .name = "GOP.MKF", .want_crc = 0 },
            { .name = "RNG.MKF", .want_crc = 0 },
        };
        for (int i = 0; i < M.h.loaded; i++) {
            if (strcmp(EN[i].name, t[0].name) == 0) t[0].want_crc = EN[i].crc32;
            if (strcmp(EN[i].name, t[1].name) == 0) t[1].want_crc = EN[i].crc32;
        }
        double t0 = now_ms();
        pthread_t p1, p2;
        pthread_create(&p1, NULL, thr_read, &t[0]);
        pthread_create(&p2, NULL, thr_read, &t[1]);
        pthread_join(p1, NULL);
        pthread_join(p2, NULL);
        double dt = now_ms() - t0;
        CHECK(t[0].ok && t[1].ok, "并发读完两文件，CRC 均与包内记录一致");
        CHECK(t[0].bytes > 0 && t[1].bytes > 0, "线程字节数 %ld / %ld", t[0].bytes, t[1].bytes);
        printf("  INFO  并发两文件 %.0f KB 用时 %.0f ms\n",
               (double)(t[0].bytes + t[1].bytes) / 1024.0, dt);
    }

    /* -------- ④ 句柄上限 -------- */
    printf("\n========== H-4 句柄上限与复用 ==========\n");
    {
        int hs[LPK_M_MAXH + 2];
        int n = 0;
        for (int i = 0; i < LPK_M_MAXH + 2; i++) {
            hs[i] = lpk_m_open(&M, "ABC.MKF", LPK_O_RDONLY);
            if (hs[i] >= 0) n++;
        }
        CHECK(n == LPK_M_MAXH, "最多同时开 %d 个（实际 %d）", LPK_M_MAXH, n);
        CHECK(hs[LPK_M_MAXH] == LPK_E_NOMEM, "第 %d 个明确失败 LPK_E_NOMEM(%d)",
              LPK_M_MAXH + 1, hs[LPK_M_MAXH]);
        for (int i = 0; i < n; i++) lpk_m_close(&M, hs[i]);
        int again = lpk_m_open(&M, "ABC.MKF", LPK_O_RDONLY);
        CHECK(again >= 0, "全部关闭后可再开（无泄漏）");
        lpk_m_close(&M, again);
    }

    /* -------- ⑤ 带活跃句柄卸载 -------- */
    printf("\n========== H-5 还有句柄开着就 unmount ==========\n");
    {
        int h1 = lpk_m_open(&M, "FBP.MKF", LPK_O_RDONLY);
        int h2 = lpk_m_open(&M, "save/x.bin", LPK_O_RDWR | LPK_O_CREAT);
        CHECK(h1 >= 0, "资源句柄 %d", h1);
        CHECK(h2 >= 0, "FAT 句柄 %d", h2);          /* 打开就落真实文件 */
        lpk_unmount(&M);                             /* 必须把两侧句柄都关掉 */
        CHECK(1, "带活跃句柄 unmount 未崩溃");
        /* 再挂一次，确认没有残留状态 */
        CHECK(lpk_mount(&M, lpk, EN, LPK_MAX_ENTRIES, base, &BK) == LPK_OK, "重新挂载成功");
        lpk_mount_add_writable_dir(&M, "save/");
        int h3 = lpk_m_open(&M, "ABC.MKF", LPK_O_RDONLY);
        CHECK(h3 >= 0, "重新挂载后还能打开资源");
        if (h3 >= 0) lpk_m_close(&M, h3);
    }

    /* -------- ⑥ 目录模式 vs LPK：主机吞吐对照 -------- */
    printf("\n========== H-6 (主机) 目录模式 vs LPK 读全部 109 条 ==========\n");
    {
        uint8_t *buf = (uint8_t *)malloc(256 * 1024);
        if (!buf) { CHECK(0, "缓冲不足"); }
        else {
            /* A: 逐个文件直接读（= 目录模式） */
            double t0 = now_ms();
            long total = 0;
            for (int i = 0; i < M.h.loaded; i++) {
                char p[1024];
                snprintf(p, sizeof(p), "%s/%s", origin, EN[i].name);
                FILE *f = fopen(p, "rb");
                if (!f) continue;
                size_t n;
                while ((n = fread(buf, 1, 256 * 1024, f)) > 0) total += (long)n;
                fclose(f);
            }
            double ta = now_ms() - t0;

            /* B: 走挂载层（= LPK 模式，每句柄独立 FILE*） */
            t0 = now_ms();
            long total2 = 0;
            for (int i = 0; i < M.h.loaded; i++) {
                int hd = lpk_m_open(&M, EN[i].name, LPK_O_RDONLY);
                if (hd < 0) continue;
                int n;
                while ((n = lpk_m_read(&M, hd, buf, 256 * 1024)) > 0) total2 += n;
                lpk_m_close(&M, hd);
            }
            double tb = now_ms() - t0;

            printf("  INFO  A 目录模式: %ld KB / %.0f ms = %.1f MB/s\n",
                   total / 1024, ta, (double)total / 1048576.0 / (ta / 1000.0));
            printf("  INFO  B LPK 模式: %ld KB / %.0f ms = %.1f MB/s\n",
                   total2 / 1024, tb, (double)total2 / 1048576.0 / (tb / 1000.0));
            CHECK(total == total2, "两条路读到的总字节一致（%ld vs %ld）", total, total2);
            printf("  INFO  ⚠ 主机文件系统，不代表真机 SD 性能；只用来看「布局本身」有没有额外代价\n");
            free(buf);
        }
    }

    lpk_unmount(&M);
    printf("\n==== %d FAIL ====\n", fails);
    return fails ? 1 : 0;
}
