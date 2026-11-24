/* ============================================================
 * am_manifest_test.c — 清单解析 / 路径守卫 主机测试
 *
 * 直接编译**生产源码** components/apps/pages/am_manifest.c（零 ESP 依赖），
 * 不写测试副本，避免"测试过了、固件里那版不对"。
 *
 * 数据来源: tools/mk_asset_manifest.py 用真实 41.7 MB 仙剑资源包生成的清单。
 * 编译/运行见 tests/asset/run_manifest_test.sh
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "am_manifest.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static char *read_all(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t rd = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[rd] = 0;
    if (out_len) *out_len = (int)rd;
    return b;
}

/* ---------------- T1 路径守卫 ---------------- */
static void t_path_guard(void) {
    printf("\nT1 路径守卫\n");
    const char *ok[] = {
        "ABC.MKF", "sound/bgm/x.mkf", "a/b/c/d.txt", "中文文件.bin",
        "data/第1章/剧情.txt", "a-b_c.d~e", "a..b", "x..y/z"   /* 段里含 .. 但整段不是 .. */
    };
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        CHECK(am_mf_path_ok(ok[i]), "应放行却拒了: %s", ok[i]);

    const char *bad[] = {
        "", "/etc/passwd", "../x", "a/../../b", "a/..", "..",
        "a\\b", "C:x", "a//b", "a/", "./a", "a/./b",
        "a\nb", "a\tb", "https://evil/x",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!am_mf_path_ok(bad[i]), "应拒收却放行: [%s]", bad[i]);

    /* 超长(>= 96 字节) 必须拒 —— 否则设备端会截断成另一个名字 */
    char longp[200];
    memset(longp, 'a', sizeof(longp));
    longp[sizeof(longp) - 1] = 0;
    CHECK(!am_mf_path_ok(longp), "超长路径应拒收");
}

/* ---------------- T2 拼接 ---------------- */
static void t_join(void) {
    printf("\nT2 路径拼接\n");
    char buf[320];
    CHECK(am_mf_join(buf, sizeof(buf), "/sdcard/apps/app/pal", "ABC.MKF") &&
          strcmp(buf, "/sdcard/apps/app/pal/ABC.MKF") == 0, "普通拼接: %s", buf);
    CHECK(am_mf_join(buf, sizeof(buf), "/sdcard/apps/app/pal/", "a/b.mkf") &&
          strcmp(buf, "/sdcard/apps/app/pal/a/b.mkf") == 0, "尾斜杠拼接: %s", buf);
    CHECK(!am_mf_join(buf, sizeof(buf), "/sdcard/apps/app/pal", "../evil"),
          "穿越必须被拒");
    CHECK(!am_mf_join(buf, 10, "/sdcard/apps/app/pal", "ABC.MKF"),
          "缓冲不够必须返回 false(不能截断)");
}

/* ---------------- T3 URL 编码 ---------------- */
static void t_url(void) {
    printf("\nT3 URL 编码\n");
    char o[400];
    am_mf_url_encode("a/b.MKF", o, sizeof(o));
    CHECK(strcmp(o, "a/b.MKF") == 0, "ASCII 不应被编码: %s", o);
    am_mf_url_encode("中文.bin", o, sizeof(o));
    CHECK(strncmp(o, "%E4%B8%AD%E6%96%87.bin", 18) == 0, "中文应被 %%XX 编码: %s", o);
    am_mf_url_encode("a b", o, sizeof(o));
    CHECK(strcmp(o, "a%20b") == 0, "空格应编码: %s", o);
}

/* ---------------- T4 真实清单 ---------------- */
static void t_real(const char *path) {
    printf("\nT4 真实清单 (%s)\n", path);
    int len = 0;
    char *txt = read_all(path, &len);
    if (!txt) { CHECK(0, "读不到清单文件: %s", path); return; }
    printf("  清单 %d B\n", len);

    am_mf_head_t h;
    CHECK(am_mf_head_parse(txt, &h) == 0, "头部解析失败");
    printf("  fmt=%d count=%d total=%u ver=%d pkg=%s\n",
           h.fmt, h.nfiles, (unsigned)h.total_bytes, h.ver, h.pkg);
    CHECK(h.fmt == 1, "fmt 应为 1");
    CHECK(h.nfiles > 50, "文件数应 > 50 (实际 %d)", h.nfiles);
    CHECK(h.total_bytes > 40000000u, "总字节应 > 40 MB");
    CHECK(strcmp(h.pkg, "pal") == 0, "pkg 应为 pal: %s", h.pkg);

    am_mf_file_t *arr = calloc((size_t)h.nfiles, sizeof(am_mf_file_t));
    int n = am_mf_files_parse(txt, arr, h.nfiles);
    CHECK(n == h.nfiles, "文件表条数 %d != 头声明 %d", n, h.nfiles);
    CHECK(am_mf_verify_total(arr, n, &h), "总字节不自洽");

    /* 抽查首/中/末三条，并逐条再跑一次路径守卫 */
    int badpath = 0;
    unsigned long long sum = 0;
    for (int i = 0; i < n; i++) {
        if (!am_mf_path_ok(arr[i].path)) badpath++;
        if (arr[i].size == 0 || arr[i].crc32 == 0) badpath++;
        sum += arr[i].size;
    }
    CHECK(badpath == 0, "有 %d 条路径/size/crc 不合法", badpath);
    CHECK(sum == (unsigned long long)h.total_bytes, "逐条加总 %llu != total %u",
          sum, (unsigned)h.total_bytes);
    printf("  首: %s (%u B crc=%08x)\n", arr[0].path, (unsigned)arr[0].size, arr[0].crc32);
    printf("  中: %s (%u B)\n", arr[n / 2].path, (unsigned)arr[n / 2].size);
    printf("  末: %s (%u B)\n", arr[n - 1].path, (unsigned)arr[n - 1].size);
    CHECK(strcmp(arr[0].path, "ABC.MKF") == 0, "首条应为 ABC.MKF: %s", arr[0].path);

    /* 与 zip 里最大的那个文件对得上（GOP.MKF 11 MB） */
    int big = 0;
    for (int i = 0; i < n; i++) if (arr[i].size > arr[big].size) big = i;
    CHECK(arr[big].size > 10u * 1024 * 1024, "最大文件应 > 10 MB: %s %u",
          arr[big].path, (unsigned)arr[big].size);
    printf("  最大: %s (%u B)\n", arr[big].path, (unsigned)arr[big].size);

    /* 每条都能拼出合法目标路径 */
    int joinbad = 0;
    char full[320];
    for (int i = 0; i < n; i++)
        if (!am_mf_join(full, sizeof(full), "/sdcard/apps/app/pal", arr[i].path)) joinbad++;
    CHECK(joinbad == 0, "%d 条拼不出目标路径", joinbad);

    free(arr);
    free(txt);
}

/* ---------------- T5 畸形清单必须被拒 ---------------- */
static void t_bad(void) {
    printf("\nT5 畸形清单拒收\n");

    struct { const char *txt; int want; const char *why; } cases[] = {
        { "{\"fmt\":1,\"count\":2,\"total\":10,\"files\":["
          "{\"path\":\"../x\",\"size\":5,\"crc32\":\"00000001\"},"
          "{\"path\":\"b\",\"size\":5,\"crc32\":\"00000002\"}]}", -4, "含 .. 穿越" },
        { "{\"fmt\":1,\"count\":2,\"total\":10,\"files\":["
          "{\"path\":\"/etc/passwd\",\"size\":5,\"crc32\":\"00000001\"},"
          "{\"path\":\"b\",\"size\":5,\"crc32\":\"00000002\"}]}", -4, "绝对路径" },
        { "{\"fmt\":1,\"count\":1,\"total\":10,\"files\":["
          "{\"path\":\"a\",\"size\":0,\"crc32\":\"00000001\"}]}", -6, "size=0" },
        { "{\"fmt\":1,\"count\":1,\"total\":10,\"files\":["
          "{\"path\":\"a\",\"size\":10}]}", -3, "缺 crc32" },
        { "{\"fmt\":1,\"count\":1,\"total\":10,\"files\":["
          "{\"path\":\"a\\\\b\",\"size\":10,\"crc32\":\"00000001\"}]}", -4, "反斜杠" },
        { "{\"fmt\":2,\"count\":1,\"total\":10,\"files\":["
          "{\"path\":\"a\",\"size\":10,\"crc32\":\"00000001\"}]}", -1, "fmt=2 不支持" },
        { "{\"fmt\":1,\"count\":9000,\"total\":10,\"files\":["
          "{\"path\":\"a\",\"size\":10,\"crc32\":\"00000001\"}]}", -1, "count 超设备上限(4096)" },
        { "", -1, "空文本" },
        { "not json at all", -1, "根本不是 JSON" },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        am_mf_head_t h;
        int hr = am_mf_head_parse(cases[i].txt, &h);
        if (hr != 0) {
            /* 头部就拒了 —— 符合"整份清单作废"的契约 */
            CHECK(cases[i].want < 0, "%s: 头部拒收但期望 %d", cases[i].why, cases[i].want);
            continue;
        }
        am_mf_file_t a[8];
        int r = am_mf_files_parse(cases[i].txt, a, 8);
        CHECK(r == cases[i].want, "%s: 期望 rc=%d 实际 %d",
              cases[i].why, cases[i].want, r);
    }

    /* count 与 files[] 实际条数不一致 ⇒ 总字节校验必须失败 */
    am_mf_head_t h2;
    const char *mismatch =
        "{\"fmt\":1,\"count\":3,\"total\":20,\"files\":["
        "{\"path\":\"a\",\"size\":10,\"crc32\":\"00000001\"},"
        "{\"path\":\"b\",\"size\":10,\"crc32\":\"00000002\"}]}";
    CHECK(am_mf_head_parse(mismatch, &h2) == 0, "头部应能解析");
    am_mf_file_t a2[8];
    int n2 = am_mf_files_parse(mismatch, a2, 8);
    CHECK(n2 == 2, "实际 2 条");
    CHECK(!am_mf_verify_total(a2, n2, &h2), "count 对不上时 verify_total 必须为 false");

    /* total 与实际加总不一致 */
    const char *badsum =
        "{\"fmt\":1,\"count\":2,\"total\":99,\"files\":["
        "{\"path\":\"a\",\"size\":10,\"crc32\":\"00000001\"},"
        "{\"path\":\"b\",\"size\":10,\"crc32\":\"00000002\"}]}";
    am_mf_head_t h3;
    CHECK(am_mf_head_parse(badsum, &h3) == 0, "头部应能解析");
    am_mf_file_t a3[8];
    int n3 = am_mf_files_parse(badsum, a3, 8);
    CHECK(!am_mf_verify_total(a3, n3, &h3), "total 不符时必须为 false");
}

/* ---------------- T6 缓冲上限 ---------------- */
static void t_limit(void) {
    printf("\nT6 条数上限\n");
    am_mf_file_t a[2];
    const char *t3 =
        "{\"fmt\":1,\"count\":3,\"total\":30,\"files\":["
        "{\"path\":\"a\",\"size\":10,\"crc32\":\"00000001\"},"
        "{\"path\":\"b\",\"size\":10,\"crc32\":\"00000002\"},"
        "{\"path\":\"c\",\"size\":10,\"crc32\":\"00000003\"}]}";
    int r = am_mf_files_parse(t3, a, 2);
    CHECK(r == -5, "只给 2 个槽却有 3 条, 应返回 -5 (实际 %d)", r);
}

int main(int argc, char **argv) {
    const char *mf = argc > 1 ? argv[1] : "tests/asset/data/pal_manifest.json";
    printf("=== am_manifest 主机测试 ===\n");
    t_path_guard();
    t_join();
    t_url();
    t_real(mf);
    t_bad();
    t_limit();
    printf("\n==== %d PASS / %d FAIL ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
