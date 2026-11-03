/* ============================================================
 * lpk_test.c — LPK 读取器主机测试
 *
 * 直接编译**生产源码** components/services/lpk/lpk1.c（零 ESP 依赖），
 * 并与 tools/pack_lpk.py 打出来的真实包交叉验证 —— 两边实现必须一致。
 *
 * 用法:
 *   lpk_test <good.lpk> <源目录> [<坏包目录>]
 *     坏包目录里的文件名形如 `-3_xxx.lpk`，前缀是要期望的 lpk_open 返回码。
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "lpk1.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static lpk_entry_t g_e[LPK_MAX_ENTRIES];

static unsigned char *slurp(const char *p, long *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)sz ? (size_t)sz : 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    if (n) *n = sz;
    return b;
}

/* ---------------- T1 路径守卫 ---------------- */
static void t_path(void) {
    printf("\nT1 路径守卫\n");
    const char *ok[] = { "ABC.MKF", "music/9.mp3", "a/b/c.dat", "中文/音乐.mp3", "a..b/c" };
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        CHECK(lpk_path_ok(ok[i]), "应放行: %s", ok[i]);
    const char *bad[] = { "", "/abs", "../x", "a/../b", "a\\b", "C:x", "a//b", "a/",
                          "./a", "a\nb", "https://x" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!lpk_path_ok(bad[i]), "应拒收: [%s]", bad[i]);
}

/* ---------------- T2 正常包 ---------------- */
static void t_good(const char *lpk, const char *srcdir) {
    printf("\nT2 正常包 (%s)\n", lpk);
    lpk_t h;
    int rc = lpk_open(&h, lpk, g_e, LPK_MAX_ENTRIES);
    CHECK(rc == LPK_OK, "打开失败 rc=%d", rc);
    if (rc != LPK_OK) return;
    printf("  条数=%u lua=%u B data_off=%u pkg_crc=%08x\n",
           (unsigned)h.count, (unsigned)h.lua_size, (unsigned)h.data_off, h.pkg_crc);
    CHECK(h.count >= 1, "资源条数应 >= 1");
    CHECK(h.loaded == (int)h.count, "表条数 %d != 头 %u", h.loaded, (unsigned)h.count);

    /* Lua 入口能读且 CRC 对 */
    if (h.lua_size) {
        char *lb = malloc(h.lua_size + 1);
        uint32_t n = 0;
        CHECK(lpk_read_lua(&h, lb, h.lua_size, &n) == LPK_OK, "读 Lua 入口失败");
        CHECK(n == h.lua_size, "Lua 长度 %u != %u", (unsigned)n, (unsigned)h.lua_size);
        CHECK(memcmp(lb, "local", 5) == 0, "Lua 内容不像 Lua: %.16s", lb);
        free(lb);
    }

    /* 逐条: 与源目录真实文件逐字节比对(至少前 4KB + 尾部 128B) */
    int bad = 0;
    for (int i = 0; i < h.loaded; i++) {
        char src[512];
        snprintf(src, sizeof(src), "%s/%s", srcdir, g_e[i].name);
        long sn = 0;
        unsigned char *s = slurp(src, &sn);
        if (!s) { printf("  (跳过, 源文件不在: %s)\n", src); continue; }
        if ((uint32_t)sn != g_e[i].size) { printf("  FAIL 大小不符 %s: %ld vs %u\n", g_e[i].name, sn, (unsigned)g_e[i].size); bad++; free(s); continue; }
        if (lpk_crc32(0, s, (uint32_t)sn) != g_e[i].crc32) { printf("  FAIL CRC 不符 %s\n", g_e[i].name); bad++; free(s); continue; }
        unsigned char buf[4096];
        uint32_t head = (uint32_t)sn < 4096 ? (uint32_t)sn : 4096;
        if (lpk_read(&h, &g_e[i], 0, buf, head) != LPK_OK || memcmp(buf, s, head) != 0) {
            printf("  FAIL 头部数据不符 %s\n", g_e[i].name); bad++; free(s); continue;
        }
        if (sn > 128) {
            uint32_t off = (uint32_t)sn - 128;
            if (lpk_read(&h, &g_e[i], off, buf, 128) != LPK_OK || memcmp(buf, s + off, 128) != 0) {
                printf("  FAIL 尾部数据不符 %s\n", g_e[i].name); bad++; free(s); continue;
            }
        }
        /* 随机 seek: 中间 64 字节 */
        if (sn > 1024) {
            uint32_t off = (uint32_t)(sn / 2);
            if (lpk_read(&h, &g_e[i], off, buf, 64) != LPK_OK || memcmp(buf, s + off, 64) != 0) {
                printf("  FAIL 随机 seek 不符 %s\n", g_e[i].name); bad++; free(s); continue;
            }
        }
        free(s);
    }
    CHECK(bad == 0, "%d 个资源与源文件不一致", bad);

    /* 边界: 读末尾 1 字节 OK; 越界 1 字节必须被拒 */
    int last = h.loaded - 1;
    unsigned char c;
    CHECK(lpk_read(&h, &g_e[last], g_e[last].size - 1, &c, 1) == LPK_OK, "读最后 1 字节应成功");
    CHECK(lpk_read(&h, &g_e[last], g_e[last].size, &c, 1) != LPK_OK, "读越界(EOF+1)必须失败");
    CHECK(lpk_read(&h, &g_e[last], 0, &c, g_e[last].size + 1) != LPK_OK, "长度超资源必须失败");

    /* 查表 */
    int idx = lpk_find(&h, g_e, g_e[0].name);
    CHECK(idx == 0, "lpk_find 首条应返回 0, 实际 %d", idx);
    CHECK(lpk_find(&h, g_e, "不存在的文件.xyz") == -1, "查不存在的资源应返回 -1");

    /* 整包 CRC */
    CHECK(lpk_verify_pkg(&h) == LPK_OK, "整包 CRC 校验失败");
    lpk_close(&h);
    CHECK(lpk_read(&h, &g_e[0], 0, &c, 1) != LPK_OK, "关闭后读必须失败");
}

/* ---------------- T3 坏包 ---------------- */
static void t_bad_dir(const char *dir) {
    printf("\nT3 坏包负向用例 (%s)\n", dir);
    DIR *d = opendir(dir);
    if (!d) { printf("  (目录不存在, 跳过)\n"); return; }
    struct dirent *e;
    int n = 0, bad = 0;
    while ((e = readdir(d)) != NULL) {
        const char *fn = e->d_name;
        if (fn[0] != '-' || !strstr(fn, ".lpk")) continue;
        int want = atoi(fn);
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", dir, fn);
        lpk_t h;
        int rc = lpk_open(&h, p, g_e, LPK_MAX_ENTRIES);
        if (rc != want) { printf("  FAIL %s: 期望 %d 实际 %d\n", fn, want, rc); bad++; }
        else { lpk_close(&h); g_pass++; }
        n++;
    }
    closedir(d);
    printf("  覆盖 %d 个坏包, %d 个不符合预期\n", n, bad);
    CHECK(n >= 6, "坏包用例太少(%d)", n);
}

int main(int argc, char **argv) {
    printf("=== LPK 读取器主机测试 ===\n");
    t_path();
    if (argc > 1) t_good(argv[1], argc > 2 ? argv[2] : ".");
    if (argc > 3) t_bad_dir(argv[3]);
    printf("\n==== %d PASS / %d FAIL ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
