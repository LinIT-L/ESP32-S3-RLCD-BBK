/**
 * lpk_mount_test.c — G2 读写分流（主机侧，用 POSIX 当"真实 FAT"）
 *
 * 用法: lpk_mount_test <pal.lpk> <后端根目录> <原始资源目录>
 *
 * 验证的核心命题只有一条：
 *   **资源只从 LPK 读，存档/配置只往真实 FAT 写，一次都不串。**
 * 特别是"盘上有同名文件也不能顶掉包内资源"——那是资源混用，最难查的一类 bug。
 *
 * 后端用 POSIX 实现（open/read/write/lseek/close/stat/mkdir/unlink/rename），
 * 与设备侧 VFS 适配层注入的是同一组语义 —— 所以这里能跑通，设备侧的逻辑就是同一套。
 * ⚠ 这**不是**真机 VFS 测试：ESP-IDF 的 VFS → FATFS 那一层没在这里跑。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "lpk_mount.h"

static int fails = 0;
/* ⚠ 条件只求值**一次**：早先版本把 (c) 写了三遍，带副作用的表达式
 * （lpk_m_write / lpk_m_open）会被执行两三次 —— 曾导致"写 100 B 结果文件 200 B"。 */
#define CHECK(c, ...) do { \
    int _ok = (c) ? 1 : 0; \
    printf(_ok ? "  PASS  " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); \
    if (!_ok) fails++; } while (0)

/* ---------------- POSIX 后端 ---------------- */
static int  b_open(void *c, const char *p, int flags)       { (void)c; return open(p, flags, 0666); }
static int  b_read(void *c, int fd, void *b, size_t n)      { (void)c; return (int)read(fd, b, n); }
static int g_wr_calls, g_wr_bytes;
static int  b_write(void *c, int fd, const void *b, size_t n){
    (void)c; g_wr_calls++; g_wr_bytes += (int)n; return (int)write(fd, b, n); }
static long b_lseek(void *c, int fd, long off, int wh)      { (void)c; return (long)lseek(fd, off, wh); }
static int  b_close(void *c, int fd)                        { (void)c; return close(fd); }
static int  b_stat(void *c, const char *p, lpk_m_stat_t *st) {
    (void)c; struct stat s;
    if (stat(p, &s) != 0) return -1;
    st->is_dir = S_ISDIR(s.st_mode) ? 1 : 0;
    st->size   = (unsigned long)s.st_size;
    return 0;
}
static int  b_mkdir(void *c, const char *p)                 { (void)c; return mkdir(p, 0777); }
static int  b_unlink(void *c, const char *p)                { (void)c; return unlink(p); }
static int  b_rename(void *c, const char *a, const char *b) { (void)c; return rename(a, b); }

static const lpk_backing_t BK = {
    .ctx = NULL, .open = b_open, .read = b_read, .write = b_write, .lseek = b_lseek,
    .close = b_close, .stat = b_stat, .mkdir = b_mkdir, .unlink = b_unlink, .rename = b_rename,
};

/* 把磁盘上的文件整体读出来（用于核对"写到了真实 FAT"） */
static long slurp(const char *path, unsigned char *buf, long cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 4) { printf("用法: %s <pal.lpk> <后端根> <原始资源目录>\n", argv[0]); return 2; }
    const char *lpk = argv[1], *base = argv[2], *origin = argv[3];

    static lpk_entry_t en[LPK_MAX_ENTRIES];
    static lpk_mnt_t m;

    printf("========== G2-0 挂载 ==========\n");
    int rc = lpk_mount(&m, lpk, en, LPK_MAX_ENTRIES, base, &BK);
    if (rc != LPK_OK) { printf("  FAIL  lpk_mount rc=%d\n", rc); return 1; }
    printf("  PASS  挂载 %s（%d 条资源），可写旁路根 %s\n", lpk, m.h.loaded, base);
    CHECK(lpk_mount_add_writable_dir(&m, "save/") == LPK_OK, "登记可写目录 save/");
    CHECK(lpk_mount_add_writable_file(&m, "sdlpal.cfg") == LPK_OK, "登记可写文件 sdlpal.cfg");
    CHECK(lpk_m_is_writable(&m, "save/0.rpg"), "save/0.rpg → 走 FAT");
    CHECK(lpk_m_is_writable(&m, "sdlpal.cfg"), "sdlpal.cfg → 走 FAT");
    CHECK(!lpk_m_is_writable(&m, "ABC.MKF"), "ABC.MKF → 走 LPK");
    CHECK(!lpk_m_is_writable(&m, "saveX/a"), "saveX/a 不是 save/ 子树 → 走 LPK");

    printf("\n========== G2-1 资源读取：精确 + 大小写 + 逐字节 ==========\n");
    {
        /* 流式逐块比对：不假设文件小于缓冲区（run_all 的 ABC.MKF 是 1 MB） */
        struct { const char *q; const char *orig; const char *label; } cases[3];
        char p1[256], p2[256], cand[256];
        snprintf(p1, sizeof(p1), "%s/ABC.MKF", origin);
        cases[0].q = "ABC.MKF";     cases[0].orig = p1; cases[0].label = "大小写一致";
        cases[1].q = "abc.mkf";     cases[1].orig = p1; cases[1].label = "全小写（PAL 实际用法）";
        /* 子目录资源：测试包可能是 music/1.mp3 或 music/9.mp3，取存在的那个 */
        snprintf(cand, sizeof(cand), "%s/music/1.mp3", origin);
        if (access(cand, R_OK) != 0) snprintf(cand, sizeof(cand), "%s/music/9.mp3", origin);
        snprintf(p2, sizeof(p2), "%s", cand);
        cases[2].q = "Music/9.Mp3"; cases[2].orig = p2; cases[2].label = "子目录 + 混合大小写";

        for (unsigned i = 0; i < 3; i++) {
            int hd = lpk_m_open(&m, cases[i].q, LPK_O_RDONLY);
            int same = -99;
            long total = 0;
            if (hd >= 0) {
                FILE *f = fopen(cases[i].orig, "rb");
                unsigned char a[8192], b[8192];
                same = 0;
                if (!f) same = -1;
                else {
                    for (;;) {
                        int na = lpk_m_read(&m, hd, a, sizeof(a));
                        size_t nb = fread(b, 1, sizeof(b), f);
                        if (na < 0) { same = -2; break; }
                        if ((size_t)na != nb || (na && memcmp(a, b, (size_t)na) != 0)) { same = -3; break; }
                        total += na;
                        if (na == 0) break;
                    }
                    fclose(f);
                }
            }
            lpk_m_close(&m, hd);
            CHECK(hd >= 0 && same == 0,
                  "%-22s (%s): hd=%d, 逐块比对 %ld B, 结果 %d(0=一致)",
                  cases[i].q, cases[i].label, hd, total, same);
        }
    }

    printf("\n========== G2-2 存档写入 → 真实 FAT ==========\n");
    {
        unsigned char pat[100];
        for (int i = 0; i < 100; i++) pat[i] = (unsigned char)(i * 7 + 3);
        int hd = lpk_m_open(&m, "save/0.rpg", LPK_O_WRONLY | LPK_O_CREAT | LPK_O_TRUNC);
        CHECK(hd >= 0, "新建 save/0.rpg（save/ 目录自动创建）hd=%d", hd);
        g_wr_calls = g_wr_bytes = 0;
        CHECK(lpk_m_write(&m, hd, pat, sizeof(pat)) == (int)sizeof(pat), "写入 100 B (b_write 调用 %d 次, 共 %d B)", g_wr_calls, g_wr_bytes);
        lpk_m_close(&m, hd);

        char full[256]; snprintf(full, sizeof(full), "%s/save/0.rpg", base);
        unsigned char back[128];
        long n = slurp(full, back, sizeof(back));
        struct stat dbg; stat(full, &dbg);
        CHECK(n == 100 && memcmp(back, pat, 100) == 0,
              "落到 %s：读到 %ld B / 磁盘 %lld B", full, n, (long long)dbg.st_size);
    }

    printf("\n========== G2-3 读回来必须来自 FAT（不是包里的）==========\n");
    {
        int hd = lpk_m_open(&m, "save/0.rpg", LPK_O_RDONLY);
        unsigned char got[128];
        int n = hd >= 0 ? lpk_m_read(&m, hd, got, sizeof(got)) : -1;
        lpk_m_close(&m, hd);
        CHECK(n == 100 && (unsigned char)(0 * 7 + 3) == got[0] && got[99] == (unsigned char)(99 * 7 + 3),
              "读回 %d B，首尾字节与写入一致（说明走的是 FAT，而包里没有这个文件）", n);
    }

    printf("\n========== G2-4 存档覆盖（同一路径写第二次）==========\n");
    {
        unsigned char p2[50];
        memset(p2, 0xAB, sizeof(p2));
        int hd = lpk_m_open(&m, "save/0.rpg", LPK_O_WRONLY | LPK_O_CREAT | LPK_O_TRUNC);
        int w = hd >= 0 ? lpk_m_write(&m, hd, p2, sizeof(p2)) : -1;
        lpk_m_close(&m, hd);
        char full[256]; snprintf(full, sizeof(full), "%s/save/0.rpg", base);
        unsigned char back[128];
        long n = slurp(full, back, sizeof(back));
        CHECK(w == 50 && n == 50 && back[0] == 0xAB && back[49] == 0xAB,
              "覆盖后磁盘上 %ld B，全 0xAB（没有追加残留）", n);
    }

    printf("\n========== G2-5 配置：首次创建 / 读回 ==========\n");
    {
        lpk_m_stat_t st;
        CHECK(lpk_m_stat(&m, "sdlpal.cfg", &st) == LPK_E_NOENT,
              "首次 stat 返回 NOENT（不是 0 字节假文件）");
        const char *cfg = "GamePath=/lpk\nSavePath=/lpk/save\n";
        int hd = lpk_m_open(&m, "sdlpal.cfg", LPK_O_WRONLY | LPK_O_CREAT | LPK_O_TRUNC);
        int w = hd >= 0 ? lpk_m_write(&m, hd, cfg, strlen(cfg)) : -1;
        lpk_m_close(&m, hd);
        CHECK(w == (int)strlen(cfg), "写入配置 %d B", w);

        CHECK(lpk_m_stat(&m, "sdlpal.cfg", &st) == LPK_OK && !st.is_dir && st.size == strlen(cfg),
              "stat 现在返回 OK，大小 %lu", st.size);

        hd = lpk_m_open(&m, "sdlpal.cfg", LPK_O_RDONLY);
        char rd[128] = {0};
        int n = hd >= 0 ? lpk_m_read(&m, hd, rd, sizeof(rd) - 1) : -1;
        lpk_m_close(&m, hd);
        CHECK(n == (int)strlen(cfg) && strcmp(rd, cfg) == 0, "读回配置内容一致");
    }

    printf("\n========== G2-6 资源区禁止写入（EROFS，且不改包）==========\n");
    {
        struct stat s1; stat(lpk, &s1);
        CHECK(lpk_m_open(&m, "ABC.MKF", LPK_O_WRONLY) == LPK_E_ROFS, "O_WRONLY 打开资源 → LPK_E_ROFS");
        CHECK(lpk_m_open(&m, "ABC.MKF", LPK_O_RDWR)   == LPK_E_ROFS, "O_RDWR  打开资源 → LPK_E_ROFS");
        CHECK(lpk_m_open(&m, "ABC.MKF", LPK_O_RDONLY | LPK_O_TRUNC) == LPK_E_ROFS,
              "O_TRUNC 打开资源 → LPK_E_ROFS");
        CHECK(lpk_m_unlink(&m, "ABC.MKF") == LPK_E_ROFS, "删除资源 → LPK_E_ROFS");
        CHECK(lpk_m_rename(&m, "ABC.MKF", "save/x") == LPK_E_ROFS, "把资源改名进 save/ → LPK_E_ROFS");
        CHECK(lpk_m_mkdir(&m, "abc") == LPK_E_ROFS, "在资源区建目录 → LPK_E_ROFS");
        struct stat s2; stat(lpk, &s2);
        CHECK(s1.st_size == s2.st_size, "包体大小未变（%lld B）", (long long)s2.st_size);

        /* 读一遍确认内容没被动过 */
        int hd = lpk_m_open(&m, "ABC.MKF", LPK_O_RDONLY);
        unsigned char a[64], b[64];
        char p1[256]; snprintf(p1, sizeof(p1), "%s/ABC.MKF", origin);
        int n = hd >= 0 ? lpk_m_read(&m, hd, a, 64) : -1;
        lpk_m_close(&m, hd);
        slurp(p1, b, sizeof(b));
        CHECK(n == 64 && memcmp(a, b, 64) == 0, "拒绝写入后资源内容仍与原始一致");
    }

    printf("\n========== G2-7 ⛔ 反混用：盘上有同名文件也不许顶掉包里资源 ==========\n");
    {
        char full[256]; snprintf(full, sizeof(full), "%s/NOPE.MKF", base);
        FILE *f = fopen(full, "wb");
        if (f) { fwrite("FAT 上的冒牌货", 1, 12, f); fclose(f); }
        int hd = lpk_m_open(&m, "NOPE.MKF", LPK_O_RDONLY);
        CHECK(hd == LPK_E_NOENT, "包内没有 NOPE.MKF ⇒ LPK_E_NOENT（**没有**去读盘上那个）hd=%d", hd);
        lpk_m_stat_t st;
        CHECK(lpk_m_stat(&m, "NOPE.MKF", &st) == LPK_E_NOENT, "stat 同样 NOENT");
        unlink(full);
    }

    printf("\n========== G2-8 目录 / EOF / 越界 / 非法路径 ==========\n");
    {
        lpk_m_stat_t st;
        CHECK(lpk_m_stat(&m, "save", &st) == LPK_OK && st.is_dir, "stat(\"save\") 是目录");
        CHECK(lpk_m_mkdir(&m, "save") == LPK_OK, "mkdir(\"save\") 已存在也不报错");

        int hd = lpk_m_open(&m, "SETUP.DAT", LPK_O_RDONLY);
        if (hd < 0) {   /* 测试包可能没有 SETUP.DAT，退回用 ABC.MKF 尾部 */
            hd = lpk_m_open(&m, "ABC.MKF", LPK_O_RDONLY);
        }
        long sz = (long)m.hd[hd].size;
        CHECK(lpk_m_seek(&m, hd, 0, LPK_SEEK_END) == sz, "lseek(SEEK_END) = %ld", sz);
        unsigned char tmp[8];
        CHECK(lpk_m_read(&m, hd, tmp, 4) == 0, "EOF 处读返回 0（与 fread 一致）");
        CHECK(lpk_m_seek(&m, hd, sz + 1, LPK_SEEK_SET) == LPK_E_ARG, "seek 越过尾部 → LPK_E_ARG");
        CHECK(lpk_m_seek(&m, hd, -1, LPK_SEEK_SET) == LPK_E_ARG, "seek 到负偏移 → LPK_E_ARG");
        CHECK(lpk_m_seek(&m, hd, 4, LPK_SEEK_SET) == 4, "seek(4) 成功");
        CHECK(lpk_m_read(&m, hd, tmp, 4) == 4, "读 4 B 成功");
        CHECK(lpk_m_read(&m, hd, tmp, 4) == 4, "再读 4 B（位置推进）");
        lpk_m_close(&m, hd);
        CHECK(lpk_m_read(&m, hd, tmp, 4) == LPK_E_ARG, "已关闭的句柄再用 → LPK_E_ARG");

        CHECK(lpk_m_open(&m, "../etc/passwd", LPK_O_RDONLY) == LPK_E_PATH, "穿越路径 → LPK_E_PATH");
        CHECK(lpk_m_open(&m, "/etc/passwd", LPK_O_RDONLY) == LPK_E_PATH, "绝对路径 → LPK_E_PATH");
        CHECK(lpk_m_open(&m, "save/../../x", LPK_O_RDONLY) == LPK_E_PATH, "save/../../x → LPK_E_PATH");
    }

    printf("\n========== G2-9 删除 / 改名（只在可写区）==========\n");
    {
        CHECK(lpk_m_rename(&m, "save/0.rpg", "save/1.rpg") == LPK_OK, "save/0.rpg → save/1.rpg");
        char a[256], b[256];
        snprintf(a, sizeof(a), "%s/save/0.rpg", base);
        snprintf(b, sizeof(b), "%s/save/1.rpg", base);
        unsigned char buf[128];
        CHECK(slurp(a, buf, sizeof(buf)) < 0 && slurp(b, buf, sizeof(buf)) > 0, "旧名没了、新名在");
        CHECK(lpk_m_unlink(&m, "save/1.rpg") == LPK_OK, "删除存档");
        CHECK(slurp(b, buf, sizeof(buf)) < 0, "磁盘上确实删掉了");
    }

    printf("\n========== G2-10 句柄上限 ==========\n");
    {
        int hs[LPK_M_MAXH + 2], opened = 0;
        for (int i = 0; i < LPK_M_MAXH + 2; i++) {
            hs[i] = lpk_m_open(&m, "ABC.MKF", LPK_O_RDONLY);
            if (hs[i] >= 0) opened++;
        }
        CHECK(opened == LPK_M_MAXH, "最多同时开 %d 个句柄（第 %d 个被拒 %d）",
              LPK_M_MAXH, LPK_M_MAXH + 1, hs[LPK_M_MAXH]);
        for (int i = 0; i < LPK_M_MAXH; i++) lpk_m_close(&m, hs[i]);
        int again = lpk_m_open(&m, "ABC.MKF", LPK_O_RDONLY);
        CHECK(again >= 0, "全部关闭后能再开（句柄没泄漏）");
        lpk_m_close(&m, again);
    }

    printf("\n========== G2-11 seek / 跨块 / 重复关闭 语义 ==========\n");
    {
        /* 仙剑的 MKF 读取完全靠 fseek：位置错一次就是整屏乱图，
         * 所以 SET/CUR/END 三种基准 + 越界拒绝都要单独钉住。 */
        int hd = lpk_m_open(&m, "ABC.MKF", LPK_O_RDONLY);
        CHECK(hd >= 0, "打开 ABC.MKF");
        if (hd >= 0) {
            lpk_m_stat_t st;
            CHECK(lpk_m_stat(&m, "ABC.MKF", &st) == LPK_OK, "取到资源大小 %lu", st.size);
            long sz = (long)st.size;

            CHECK(lpk_m_seek(&m, hd, 0, LPK_SEEK_SET) == 0, "SEEK_SET 到 0");
            CHECK(lpk_m_seek(&m, hd, 100, LPK_SEEK_CUR) == 100, "SEEK_CUR +100 → 100");
            CHECK(lpk_m_seek(&m, hd, 0, LPK_SEEK_END) == sz, "SEEK_END → %ld", sz);
            CHECK(lpk_m_seek(&m, hd, -1, LPK_SEEK_END) == sz - 1, "SEEK_END -1 → %ld", sz - 1);
            CHECK(lpk_m_read(&m, hd, &st, 1) == 1, "结尾前读最后 1 字节成功");
            CHECK(lpk_m_read(&m, hd, &st, 1) == 0, "再读 = EOF(返回 0，不是错误)");

            /* 越界 seek 必须被拒，且**不能改变当前位置** */
            CHECK(lpk_m_seek(&m, hd, -1, LPK_SEEK_SET) < 0, "负偏移 → 拒绝");
            CHECK(lpk_m_seek(&m, hd, sz + 1, LPK_SEEK_SET) < 0, "超过文件尾 → 拒绝");
            CHECK(lpk_m_seek(&m, hd, 0, LPK_SEEK_END) == sz, "越界被拒后位置没被改坏");

            /* 跨块：从中间开始连续读 256 KB（远大于单次缓冲），长度必须正好到 EOF */
            long start = sz / 2;
            CHECK(lpk_m_seek(&m, hd, start, LPK_SEEK_SET) == start, "定位到中间 %ld", start);
            static unsigned char cb[4096];
            long total = 0;
            int n;
            while ((n = lpk_m_read(&m, hd, cb, sizeof(cb))) > 0) total += n;
            CHECK(total == sz - start, "跨块连续读到 EOF: %ld / %ld", total, sz - start);

            CHECK(lpk_m_close(&m, hd) == LPK_OK, "关闭");
            CHECK(lpk_m_close(&m, hd) == LPK_E_ARG, "重复关闭 → LPK_E_ARG（不二次释放）");
            CHECK(lpk_m_read(&m, hd, cb, 4) == LPK_E_ARG, "关闭后读 → LPK_E_ARG");
            CHECK(lpk_m_seek(&m, hd, 0, LPK_SEEK_SET) < 0, "关闭后 seek → 失败");
        }
        CHECK(lpk_m_close(&m, 9999) == LPK_E_ARG, "不存在的句柄 → LPK_E_ARG");
        static unsigned char dummy[4];
        CHECK(lpk_m_read(&m, -1, dummy, 4) == LPK_E_ARG, "负句柄 → LPK_E_ARG");
    }

    lpk_unmount(&m);
    printf("\n==== %d FAIL ====\n", fails);
    return fails ? 1 : 0;
}
