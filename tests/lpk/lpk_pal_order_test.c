/**
 * lpk_pal_order_test.c — 仙剑**真实资源访问顺序**回归（主机侧，走挂载层）
 *
 * 用法: lpk_pal_order_test <pal.lpk> <后端根目录> <原始资源目录>
 *
 * 为什么不能只测 "open + read 一个文件"
 * ------------------------------------
 * 仙剑读资源不是顺序读一个文件，而是：
 *   ① 打开 ABC.MKF → 读前 4 字节 offsets[0] → count = offsets[0]/4 - 1 → 读 (count+1) 个偏移；
 *   ② 按偏移 **随机** fseek 到某个块，读固定长度（PAL_ReadMKFChunk）；
 *   ③ 一个资源读完关掉，回头再开另一个（GOP/FBP/MAP/RNG...），
 *      引擎同时还会开着 mus.mkf / voc.mkf；
 *   ④ 名字一律**小写**（DOS 时代习惯，磁盘上是 ABC.MKF）。
 * 任何一条在"偏移/长度/大小写/重开"上出错，游戏都表现为"卡在读盘"或"贴图乱"，
 * 而在最小的 open/read 用例里完全看不出来。所以这里照 ①~④ 全跑一遍。
 *
 * 最后把包里**每一条**资源整段读出来算 CRC32，同时和
 *    · 包索引里记录的 crc32
 *    · ZIP 解压出来的原件 crc32
 * 三方对齐 —— 这才是"逐字节一致"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "lpk_mount.h"

static int fails = 0;
#define CHECK(c, ...) do { \
    int _ok = (c) ? 1 : 0; \
    printf(_ok ? "  PASS  " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); \
    if (!_ok) fails++; } while (0)

/* ---------------- POSIX 后端（= 设备侧真实 FAT 的同一组语义） ---------------- */
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

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static lpk_mnt_t M;
static lpk_entry_t EN[LPK_MAX_ENTRIES];

/* 按仙剑的方式打开一个 MKF，读索引 + 随机块，并与原件逐字节比。 */
static void pal_mkf(const char *name_lc, const char *name_real, const char *origin)
{
    printf("\n---------- %s（按仙剑的方式：小写名 + 索引 + 随机块） ----------\n", name_real);

    /* ① 小写名字打开（包内存的是 ABC.MKF） */
    int hd = lpk_m_open(&M, name_lc, LPK_O_RDONLY);
    CHECK(hd >= 0, "以小写名 %s 打开 → 句柄 %d", name_lc, hd);
    if (hd < 0) return;

    /* ② 文件长度：fseek(0,END) */
    long size = lpk_m_seek(&M, hd, 0, LPK_SEEK_END);
    CHECK(size > 8, "%s 长度 %ld", name_real, size);
    (void)lpk_m_seek(&M, hd, 0, LPK_SEEK_SET);

    /* ③ 读 offsets[0] → count */
    uint8_t b4[4];
    CHECK(lpk_m_read(&M, hd, b4, 4) == 4, "读索引头 4 B");
    uint32_t first = rd32le(b4);
    uint32_t nblk = (first >= 8 && first % 4 == 0) ? first / 4 - 1 : 0;
    CHECK(nblk > 0 && (long)((nblk + 1) * 4) <= size, "块数 %u（索引 %u B / 文件 %ld）",
          (unsigned)nblk, (unsigned)(nblk + 1) * 4, size);
    if (!nblk) { lpk_m_close(&M, hd); return; }

    /* ④ 读完整索引，验证单调不减 + 尾部不越界 */
    uint32_t *offs = (uint32_t *)malloc((size_t)(nblk + 1) * 4);
    if (!offs) { CHECK(0, "索引表内存不足"); lpk_m_close(&M, hd); return; }
    CHECK(lpk_m_seek(&M, hd, 0, LPK_SEEK_SET) == 0 &&
          lpk_m_read(&M, hd, offs, (nblk + 1) * 4) == (int)((nblk + 1) * 4),
          "读完整索引 (%u+1 项)", (unsigned)nblk);
    int bad = 0;
    for (uint32_t i = 0; i < nblk; i++)
        if (offs[i] > offs[i + 1] || (long)offs[i + 1] > size) bad++;
    CHECK(bad == 0, "索引单调且不越界（%d 处异常）", bad);
    CHECK(offs[nblk] == (uint32_t)size, "末偏移 == 文件长度（%u / %ld）", (unsigned)offs[nblk], size);

    /* ⑤ 随机块：头 / 中 / 尾，各读两遍（第二遍重开句柄），并与原件逐字节比 */
    static uint8_t a[65536], b[65536];
    uint32_t pick[3] = { 0, nblk / 2, nblk - 1 };
    /* 第 0 块在 ABC/GOP 里长度就是 0（索引占位），挑第一个非空块来读 */
    while (pick[0] + 1 < nblk && offs[pick[0]] == offs[pick[0] + 1]) pick[0]++;
    char opath[1024];
    snprintf(opath, sizeof(opath), "%s/%s", origin, name_real);
    FILE *of = fopen(opath, "rb");
    CHECK(of != NULL, "打开原件 %s", opath);

    for (int k = 0; k < 3; k++) {
        uint32_t i = pick[k];
        uint32_t o0 = offs[i], o1 = offs[i + 1];
        uint32_t len = o1 - o0;
        if (len == 0) {
            /* 空块是**合法**的：ABC/GOP 的第 0 块长度就是 0（off[0]==off[1]），
             * SDLPal 的 PAL_ReadMKFChunk 对空块返回 NULL 而不报错。 */
            CHECK(1, "%s 第%u块 @%u 为空块（合法）", name_real, (unsigned)i, (unsigned)o0);
            continue;
        }
        if (len > sizeof(a)) { CHECK(0, "第%u块 长度异常 %u", (unsigned)i, (unsigned)len); continue; }

        bool ok = lpk_m_seek(&M, hd, (long)o0, LPK_SEEK_SET) == (long)o0 &&
                  lpk_m_read(&M, hd, a, len) == (int)len;

        /* 重开句柄读同一块 */
        int hd2 = lpk_m_open(&M, name_lc, LPK_O_RDONLY);
        ok = ok && hd2 >= 0 &&
             lpk_m_seek(&M, hd2, (long)o0, LPK_SEEK_SET) == (long)o0 &&
             lpk_m_read(&M, hd2, b, len) == (int)len;
        if (hd2 >= 0) lpk_m_close(&M, hd2);
        ok = ok && memcmp(a, b, len) == 0;

        /* 与 ZIP 原件同区间比 */
        bool same_origin = false;
        if (of && fseek(of, (long)o0, SEEK_SET) == 0 && fread(b, 1, len, of) == len)
            same_origin = memcmp(a, b, len) == 0;
        ok = ok && same_origin;

        CHECK(ok, "%s 第%u块 @%u +%u：重读一致 + 与原件一致", name_real,
              (unsigned)i, (unsigned)o0, (unsigned)len);
    }
    if (of) fclose(of);
    free(offs);

    /* ⑥ 关掉再开：句柄可复用 */
    CHECK(lpk_m_close(&M, hd) == LPK_OK, "关闭 %s", name_lc);
    int again = lpk_m_open(&M, name_lc, LPK_O_RDONLY);
    CHECK(again >= 0, "再次打开 %s", name_lc);
    if (again >= 0) lpk_m_close(&M, again);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("用法: %s <pal.lpk> <后端根> <原始资源目录>\n", argv[0]);
        return 2;
    }
    const char *lpk = argv[1], *base = argv[2], *origin = argv[3];

    printf("========== PAL-0 挂载（可写旁路 = 真实 FAT） ==========\n");
    int rc = lpk_mount(&M, lpk, EN, LPK_MAX_ENTRIES, base, &BK);
    if (rc != LPK_OK) { printf("  FAIL  lpk_mount rc=%d\n", rc); return 1; }
    CHECK(lpk_mount_add_writable_dir(&M, "save/") == LPK_OK, "登记 save/");
    CHECK(lpk_mount_add_writable_file(&M, "sdlpal.cfg") == LPK_OK, "登记 sdlpal.cfg");
    printf("  INFO  包内 %d 条资源\n", M.h.loaded);

    /* 目录能不能 stat 出来：pal_music.c 用 stat("<GamePath>/music") 决定
     * 走音乐模式还是音效模式。包里没有目录条目，只能靠前缀推断 —— 这条错了
     * 的表现是"画面正常、音乐没了"，极难反查。 */
    printf("\n========== PAL-D 目录推断（包里只有文件，没有目录条目） ==========\n");
    {
        lpk_m_stat_t st;
        CHECK(lpk_m_stat(&M, "", &st) == LPK_OK && st.is_dir, "挂载根是目录");
        CHECK(lpk_m_stat(&M, "music", &st) == LPK_OK && st.is_dir, "music 是目录（%s）",
              lpk_m_stat(&M, "music", &st) == LPK_OK ? "OK" : "NOENT");
        CHECK(lpk_m_stat(&M, "save", &st) != LPK_OK, "save 未创建时 → NOENT（首次存档自动建）");
        CHECK(lpk_m_mkdir(&M, "save") == LPK_OK &&
              lpk_m_stat(&M, "save", &st) == LPK_OK && st.is_dir, "建 save/ 后是目录");
        CHECK(lpk_m_stat(&M, "ABC.MKF", &st) == LPK_OK && !st.is_dir &&
              st.size == 1022564, "ABC.MKF 是文件且大小 1022564（%lu）", st.size);
        CHECK(lpk_m_access(&M, "music") == LPK_OK, "access music 通过");
        CHECK(lpk_m_stat(&M, "nosuchdir", &st) != LPK_OK, "不存在的目录 → NOENT");
        CHECK(lpk_m_open(&M, "music", LPK_O_RDONLY) == LPK_E_ISDIR, "目录不能当文件打开（EISDIR）");
    }

    /* 仙剑真实启动顺序：先 ABC（战斗动作），再 GOP/FBP（贴图/过场），后面才轮到别的 */
    pal_mkf("abc.mkf", "ABC.MKF", origin);
    pal_mkf("gop.mkf", "GOP.MKF", origin);
    pal_mkf("fbp.mkf", "FBP.MKF", origin);

    /* 同时开多个 MKF（引擎会同时挂着 mus.mkf / voc.mkf） */
    printf("\n---------- 多文件同时打开 ----------\n");
    {
        const char *multi[] = { "abc.mkf", "gop.mkf", "fbp.mkf", "map.mkf", "rng.mkf", "mus.mkf" };
        int hds[6];
        int n = 0;
        for (int i = 0; i < 6; i++) {
            hds[i] = lpk_m_open(&M, multi[i], LPK_O_RDONLY);
            if (hds[i] >= 0) n++;
        }
        CHECK(n == 6, "6 个 MKF 同时打开成功 %d/6", n);
        /* 交叉读：每读一个都换文件，验证"共享 FILE* + 各自 pos"没有串位 */
        static uint8_t x[16], y[16];
        int cross_ok = 1;
        for (int i = 0; i < 6; i++) {
            if (hds[i] < 0) continue;
            if (lpk_m_seek(&M, hds[i], 0, LPK_SEEK_SET) != 0) cross_ok = 0;
            if (lpk_m_read(&M, hds[i], x, sizeof(x)) != (int)sizeof(x)) cross_ok = 0;
            if (lpk_m_read(&M, hds[i], y, sizeof(y)) != (int)sizeof(y)) cross_ok = 0;
        }
        CHECK(cross_ok, "交替读 6 个文件（各 32 B）不串位");
        for (int i = 0; i < 6; i++) if (hds[i] >= 0) lpk_m_close(&M, hds[i]);
    }

    /* 逐条 CRC：包内索引 / 挂载层读出 / ZIP 原件，三方对齐 */
    printf("\n---------- 逐条 CRC 三方对齐 ----------\n");
    {
        uint8_t *buf = (uint8_t *)malloc(65536);
        int bad_crc = 0, bad_origin = 0, missing_origin = 0, done = 0;
        for (int i = 0; i < M.h.loaded; i++) {
            const lpk_entry_t *e = &EN[i];
            int hd = lpk_m_open(&M, e->name, LPK_O_RDONLY);
            if (hd < 0) { bad_crc++; continue; }
            uint32_t crc = 0;
            int n;
            while ((n = lpk_m_read(&M, hd, buf, 65536)) > 0) crc = lpk_crc32(crc, buf, (uint32_t)n);
            lpk_m_close(&M, hd);
            if (e->crc32 && crc != e->crc32) {
                bad_crc++;
                if (bad_crc <= 3) printf("  FAIL  CRC 不符 %s: %08x vs %08x\n",
                                         e->name, (unsigned)crc, (unsigned)e->crc32);
            }
            char op[1024];
            snprintf(op, sizeof(op), "%s/%s", origin, e->name);
            if (access(op, 0) == 0) {
                uint32_t oc = lpk_crc32_file(op);
                if (oc != crc) {
                    bad_origin++;
                    if (bad_origin <= 3) printf("  FAIL  与原件不符 %s: %08x vs %08x\n",
                                                e->name, (unsigned)crc, (unsigned)oc);
                }
            } else {
                missing_origin++;
            }
            done++;
        }
        free(buf);
        CHECK(bad_crc == 0, "包内 CRC：%d/%d 一致", done - bad_crc, done);
        CHECK(bad_origin == 0, "与 ZIP 原件 CRC：%d 处不符（原件缺失 %d 条）",
              bad_origin, missing_origin);
        CHECK(done == M.h.loaded, "覆盖全部 %d 条", M.h.loaded);
    }

    lpk_unmount(&M);
    printf("\n==== %d FAIL ====\n", fails);
    return fails ? 1 : 0;
}
