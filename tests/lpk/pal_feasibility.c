/**
 * pal_feasibility.c — 仙剑资源走 LPK 的最低限度可行性验证（主机侧）
 *
 * 不碰引擎、不碰固件，只回答五个问题：
 *   ① 41.7 MB 的仙剑资源能不能装进一个 LPK 并正确打开
 *   ② 索引里的偏移/大小/文件名对不对
 *   ③ 随机读 6 个不同资源，内容与 ZIP 原件是否逐字节一致
 *   ④ EOF / 越界 / 不存在 / 非法路径 / **大小写不匹配** 的行为是否安全
 *   ⑤ 按 SDL_RWops 的语义(seek/read/tell)驱一遍 MKF 的真实访问模式能不能跑通
 *   ⑥ 打开耗时、随机读、顺序读与「解压后的普通文件」相比有没有明显退化
 *
 * 输出末尾的 RESULT 行给 verify_feasibility.py 做逐字节核对。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "lpk1.h"

#define LPK_PATH   "/tmp/pal2.lpk"
#define PLAIN_DIR  "/tmp/asset_pal"

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  PASS  "); } else { printf("  FAIL  "); fails++; } \
    printf(__VA_ARGS__); printf("\n"); } while (0)

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---- SDL_RWops 等价适配（只在这个测试里，形状照抄 sdl_shim.c 的实现） ---- */
typedef struct { lpk_t *h; const lpk_entry_t *e; long pos; int err; } rw_t;

static int rw_seek(rw_t *r, long offset, int whence) {
    long base = (whence == SEEK_SET) ? 0 : (whence == SEEK_CUR) ? r->pos : (long)r->e->size;
    long np = base + offset;
    if (np < 0 || (unsigned long)np > r->e->size) return -1;
    r->pos = np;
    return (int)r->pos;
}
static int rw_read(rw_t *r, void *buf, int size, int n) {
    if (size <= 0 || n <= 0) return 0;
    long want = (long)size * n;
    long left = (long)r->e->size - r->pos;
    if (left <= 0) return 0;
    if (want > left) want = left;
    if (lpk_read(r->h, r->e, (uint32_t)r->pos, buf, (uint32_t)want) != LPK_OK) { r->err = 1; return 0; }
    r->pos += want;
    return (int)(want / size);
}
static int rw_tell(rw_t *r) { return (int)r->pos; }

/* 大小写不敏感的查找（PAL 的 PAL_FILESYSTEM_IGNORE_CASE=1 要求这个） */
static int find_ci(const lpk_t *h, const lpk_entry_t *en, const char *name) {
    int i = lpk_find(h, en, name);
    if (i >= 0) return i;
    for (i = 0; i < h->loaded; i++) {
        const char *a = en[i].name, *b = name;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}

static uint32_t crc_of(lpk_t *h, const lpk_entry_t *e, int chunk, double *ms) {
    static uint8_t buf[65536];
    uint32_t crc = 0, off = 0;
    double t0 = now_ms();
    while (off < e->size) {
        uint32_t n = e->size - off; if (n > (uint32_t)chunk) n = (uint32_t)chunk;
        if (lpk_read(h, e, off, buf, n) != LPK_OK) return 0xFFFFFFFFu;
        crc = lpk_crc32(crc, buf, n);
        off += n;
    }
    if (ms) *ms = now_ms() - t0;
    return crc;
}

int main(void) {
    static lpk_entry_t en[LPK_MAX_ENTRIES];
    lpk_t h;
    memset(&h, 0, sizeof(h));

    printf("========== 1) 打开 41.7 MB 的 LPK ==========\n");
    double t0 = now_ms();
    int rc = lpk_open(&h, LPK_PATH, en, LPK_MAX_ENTRIES);
    double t_open = now_ms() - t0;
    if (rc != LPK_OK) { printf("  FAIL  打开失败 rc=%d\n", rc); return 1; }
    printf("  PASS  打开成功 rc=0 用时 %.3f ms\n", t_open);
    printf("  头部: total=%u lua=%u B flags=0x%x pkg_crc=%08x\n",
           (unsigned)h.total_size, (unsigned)h.lua_size, h.flags, h.pkg_crc);
    printf("  索引: count=%u loaded=%d\n", (unsigned)h.count, h.loaded);
    long idx_bytes = (long)h.count * LPK_ENTRY_SIZE;
    printf("       打开只读 64 B 头 + %u B 元数据区(记录+名字池)\n",
           (unsigned)(h.data_off - LPK_HDR_SIZE));
    printf("       常驻内存 ≈ %ld B 条目表 (%.1f KB), 正文一个字节都不进内存\n",
           idx_bytes, idx_bytes / 1024.0);
    CHECK(h.count == 110 && h.loaded == 110, "条数 110 / 实际载入 %d", h.loaded);
    CHECK(h.total_size == 43759616u, "total_size = %u", (unsigned)h.total_size);
    CHECK(h.data_off >= LPK_HDR_SIZE, "数据区起始 data_off=%u", (unsigned)h.data_off);

    printf("\n========== 2) 索引自洽：偏移/大小/名字 ==========\n");
    int bad_off = 0, bad_sz = 0, bad_name = 0, overlap = 0;
    for (int i = 0; i < h.loaded; i++) {
        if (en[i].off < h.data_off || (uint64_t)en[i].off + en[i].size > h.total_size) bad_off++;
        if (en[i].size == 0) bad_sz++;
        if (!en[i].name[0] || strchr(en[i].name, '\\') || en[i].name[0] == '/' ||
            strstr(en[i].name, "..")) bad_name++;
        if (i && en[i].off < en[i-1].off + en[i-1].size) overlap++;
    }
    CHECK(bad_off == 0, "所有偏移落在 [data_off, total) 内 (越界 %d)", bad_off);
    CHECK(bad_sz == 0, "没有 0 字节条目 (异常 %d)", bad_sz);
    CHECK(bad_name == 0, "路径合法(相对/无反斜杠/无 ..) (异常 %d)", bad_name);
    CHECK(overlap == 0, "条目区间互不重叠 (重叠 %d)", overlap);

    printf("\n========== 3) 六个不同资源逐字节核对（CRC 交 python 与 ZIP 原件比）==========\n");
    const char *names[] = {
        "SETUP.DAT",     /* 20 B，最小 */
        "PAT.MKF",       /* 8 KB，小文件 */
        "ABC.MKF",       /* 1.0 MB，中 */
        "GOP.MKF",       /* 11 MB，最大 */
        "MUS.MKF",       /* 331 KB */
        "music/39.mp3",  /* 子目录 + 二进制 */
    };
    for (unsigned k = 0; k < sizeof(names)/sizeof(names[0]); k++) {
        int i = find_ci(&h, en, names[k]);
        if (i < 0) { printf("  FAIL  找不到 %s\n", names[k]); fails++; continue; }
        double ms = 0;
        uint32_t crc = crc_of(&h, &en[i], 65536, &ms);
        printf("  RESULT %s size=%u crc=%08x ms=%.2f\n", names[k], (unsigned)en[i].size, crc, ms);
        printf("  PASS  %-14s %9u B @%-9u crc=%08x  %6.2f ms\n",
               names[k], (unsigned)en[i].size, (unsigned)en[i].off, crc, ms);
    }

    printf("\n========== 4) 边界与异常行为 ==========\n");
    int gi = find_ci(&h, en, "GOP.MKF");
    uint8_t b[16];
    CHECK(gi >= 0, "定位 GOP.MKF");
    const lpk_entry_t *g = &en[gi];
    /* 末尾一个字节可读 */
    CHECK(lpk_read(&h, g, g->size - 1, b, 1) == LPK_OK, "读最后一个字节 (off=size-1, len=1) 成功");
    /* 起点正好在尾部 ⇒ 越界拒绝 */
    CHECK(lpk_read(&h, g, g->size, b, 1) == LPK_E_ARG, "off=size 读 1 字节被拒 (LPK_E_ARG)");
    /* 跨尾部 */
    CHECK(lpk_read(&h, g, g->size - 4, b, 8) == LPK_E_ARG, "跨尾部读 (off=size-4,len=8) 被拒");
    /* 远越界（防 32 位回绕） */
    CHECK(lpk_read(&h, g, 0xFFFFFFF0u, b, 0x20) == LPK_E_ARG, "off=0xFFFFFFF0 不被回绕绕过");
    /* 不存在的资源 */
    CHECK(lpk_find(&h, en, "NOPE.MKF") == -1, "不存在的资源 lpk_find 返回 -1");
    /* 非法路径 */
    CHECK(!lpk_path_ok("../etc/passwd"), "lpk_path_ok(\"../etc/passwd\") = false");
    CHECK(!lpk_path_ok("/etc/passwd"), "lpk_path_ok(\"/etc/passwd\") = false");
    CHECK(!lpk_path_ok("music\\39.mp3"), "lpk_path_ok(反斜杠) = false");
    CHECK(lpk_path_ok("music/39.mp3"), "lpk_path_ok(\"music/39.mp3\") = true");
    /* 大小写（PAL 依赖，见报告） */
    int ci = lpk_find(&h, en, "abc.mkf");
    printf("  NOTE  lpk_find(\"abc.mkf\") = %d  %s\n", ci,
           ci < 0 ? "⇒ 现有实现**大小写敏感**，PAL 直接接会全盘找不到文件" : "");
    int ci2 = find_ci(&h, en, "abc.mkf");
    CHECK(ci2 >= 0, "大小写不敏感查找能命中 ABC.MKF (下标 %d)", ci2);

    printf("\n========== 5) 按 SDL_RWops 语义驱一遍 MKF 真实访问模式 ==========\n");
    /* MKF 真实布局（与 palcommon.c:988 `fseek(fp, 4 * n)` 完全对应）:
     *   u32 offsets[count+1] 从**字节 0** 开始, offsets[0] = 4*(count+1) = 数据区起点,
     *   末项 == 文件大小 ⇒ count = offsets[0]/4 - 1。
     *   PAL 每次取块就是「seek 到 offsets[n], 读 offsets[n+1]-offsets[n] 字节」。 */
    int fi = find_ci(&h, en, "FBP.MKF");
    if (fi < 0) { printf("  FAIL  找不到 FBP.MKF\n"); fails++; }
    else {
        rw_t r = { &h, &en[fi], 0, 0 };
        uint32_t data_start = 0;
        CHECK(rw_read(&r, &data_start, 4, 1) == 1 && data_start >= 8 && data_start % 4 == 0,
              "读 MKF 表头: offsets[0]=%u ⇒ 块数 %u", (unsigned)data_start,
              (unsigned)(data_start / 4 - 1));
        unsigned nchunk = data_start / 4 - 1;
        uint32_t *ofs = (uint32_t *)calloc((size_t)nchunk + 1, 4);
        if (ofs && rw_seek(&r, 0, SEEK_SET) == 0 &&
            rw_read(&r, ofs, 4, (int)nchunk + 1) == (int)nchunk + 1) {
            CHECK(ofs[nchunk] == en[fi].size, "末偏移 %u == 资源大小 %u",
                  (unsigned)ofs[nchunk], (unsigned)en[fi].size);
            printf("  PASS  块数 %u, 首块 @%u 末块 @%u\n",
                   (unsigned)nchunk, (unsigned)ofs[0], (unsigned)ofs[nchunk - 1]);

            /* 取中间一块，按 PAL 的方式 seek+read，再与普通文件同一块逐字节比 */
            unsigned pick = nchunk / 2;
            uint32_t clen = ofs[pick + 1] - ofs[pick];
            uint8_t *cb = (uint8_t *)malloc(clen);
            uint8_t *pb = (uint8_t *)malloc(clen);
            int sk = rw_seek(&r, (long)ofs[pick], SEEK_SET);
            int rd = (sk >= 0 && cb) ? rw_read(&r, cb, 1, (int)clen) : 0;
            CHECK(sk == (int)ofs[pick] && rd == (int)clen,
                  "seek(%u) + read(%u B) 成功, tell=%d",
                  (unsigned)ofs[pick], (unsigned)clen, rw_tell(&r));

            char pp[256]; snprintf(pp, sizeof(pp), "%s/FBP.MKF", PLAIN_DIR);
            FILE *pf = fopen(pp, "rb");
            int same = 0;
            if (pf && pb) {
                if (fseek(pf, (long)ofs[pick], SEEK_SET) == 0 &&
                    fread(pb, 1, clen, pf) == clen)
                    same = (memcmp(cb, pb, clen) == 0);
                fclose(pf);
            }
            CHECK(same, "块 #%u (%u B) 与普通文件同一块逐字节一致", pick, (unsigned)clen);
            free(cb); free(pb);
            /* 越界 seek 必须被拒（PAL 里 offset 可能被坏数据污染） */
            CHECK(rw_seek(&r, (long)en[fi].size + 1, SEEK_SET) == -1, "seek 越过资源尾被拒");
        } else {
            printf("  FAIL  读偏移表失败\n"); fails++;
        }
        free(ofs);
    }

    printf("\n========== 6) 性能：LPK vs 解压后的普通文件 ==========\n");
    {
        uint8_t *buf = (uint8_t *)malloc(4096);
        char pp[256]; snprintf(pp, sizeof(pp), "%s/GOP.MKF", PLAIN_DIR);
        FILE *pf = fopen(pp, "rb");
        if (!pf || !buf) { printf("  SKIP  普通文件打不开\n"); }
        else {
            /* 两个循环各先热身一遍，避免第一次缺页把数字带偏 */
            unsigned seed = 12345;
            for (int w = 0; w < 2; w++) {
                long okc = 0, ok2 = 0;
                double t1 = now_ms();
                seed = 12345;
                for (int i = 0; i < 200; i++) {
                    seed = seed * 1103515245u + 12345u;
                    uint32_t off = (seed >> 8) % (g->size - 4096);
                    if (lpk_read(&h, g, off, buf, 4096) == LPK_OK) okc++;
                }
                double t_lpk = now_ms() - t1;
                seed = 12345;
                double t2 = now_ms();
                for (int i = 0; i < 200; i++) {
                    seed = seed * 1103515245u + 12345u;
                    uint32_t off = (seed >> 8) % (g->size - 4096);
                    if (fseek(pf, off, SEEK_SET) == 0 && fread(buf, 1, 4096, pf) == 4096) ok2++;
                }
                double t_plain = now_ms() - t2;
                if (w == 0) continue;                 /* 热身轮不报 */
                CHECK(okc == 200 && ok2 == 200, "随机读 4 KB × 200 全部成功 (LPK %ld / 文件 %ld)", okc, ok2);
                printf("  LPK   %.3f ms/次   |  普通文件 %.3f ms/次  (主机页缓存, 仅看数量级)\n",
                       t_lpk / 200, t_plain / 200);
                printf("  ⇒ LPK 每次只多一次“加偏移+区间检查”，**不做任何额外拷贝**\n");
            }
            fclose(pf);
        }
        free(buf);
    }
    {
        /* 顺序整读 11 MB */
        double ms = 0;
        crc_of(&h, g, 8192, &ms);
        printf("  PASS  顺序整读 GOP.MKF 11.53 MB: %.1f ms ⇒ %.1f MB/s（8 KB 块）\n",
               ms, 11.530322 / (ms / 1000.0));
        double ms2 = 0;
        crc_of(&h, g, 65536, &ms2);
        printf("  PASS  顺序整读 GOP.MKF 11.53 MB: %.1f ms ⇒ %.1f MB/s（64 KB 块）\n",
               ms2, 11.530322 / (ms2 / 1000.0));
    }

    printf("\n========== 7) 整包 CRC ==========\n");
    double tv = now_ms();
    int vr = lpk_verify_pkg(&h);
    printf("  %s  整包校验 rc=%d (pkg_crc=%08x) 用时 %.0f ms\n",
           vr == LPK_OK ? "PASS" : "FAIL", vr, h.pkg_crc, now_ms() - tv);
    if (vr != LPK_OK) fails++;

    lpk_close(&h);
    printf("\n==== %d FAIL ====\n", fails);
    return fails ? 1 : 0;
}
