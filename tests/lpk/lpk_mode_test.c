/**
 * lpk_mode_test.c — 资源模式判定矩阵（主机侧）
 *
 * 用法: lpk_mode_test <dir_ok> <dir_incomplete> <dir_lpk> <dir_both> <dir_badlpk> <dir_empty>
 *
 * 判定是"选错就直接打不开游戏"的地方，所以每个组合都过一遍：
 *   安装记录(lpk/zip/无) × 目录完整性(全/缺) × 包可用性(好/坏/无) = 12 种
 * 重点看**回落**：安装记录说 lpk 但包坏了、且旧目录还在 ⇒ 必须回落到目录，
 * 绝不能既不认包也不认目录（那等于把用户存档一起废掉）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "lpk_mode.h"

static int fails = 0;
#define CHECK(c, ...) do { \
    int _ok = (c) ? 1 : 0; \
    printf(_ok ? "  PASS  " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); \
    if (!_ok) fails++; } while (0)

static const char *g_ok, *g_inc, *g_lpk, *g_both, *g_bad, *g_empty;

static lpk_res_mode_t decide(const char *dir, const char **why_out)
{
    static char why[160];
    lpk_entry_t *tab = NULL;
    int max = 0;

    /* 与设备侧同一套做法：按头里的条数分配索引表 */
    char p[300];
    snprintf(p, sizeof(p), "%s/pal.lpk", dir);
    lpk_t h;
    if (lpk_open(&h, p, NULL, 0) == LPK_OK) {
        max = (int)h.count;
        lpk_close(&h);
        if (max > 0) tab = (lpk_entry_t *)malloc((size_t)max * sizeof(lpk_entry_t));
    }
    lpk_res_mode_t m = lpk_mode_decide(dir, tab, tab ? max : 0, why, sizeof(why));
    free(tab);
    if (why_out) *why_out = why;
    return m;
}

static void expect(const char *tag, const char *dir, int rec, lpk_res_mode_t want)
{
    if (rec >= 0) CHECK(lpk_mode_record_write(dir, rec) == 0, "%s: 写安装记录=%s",
                        tag, rec ? "lpk" : "zip");
    const char *why = "";
    lpk_res_mode_t got = decide(dir, &why);
    CHECK(got == want, "%s → %s（期望 %s）| %s", tag, lpk_mode_str(got),
          lpk_mode_str(want), why);
    if (rec >= 0) {
        char p[300];
        snprintf(p, sizeof(p), "%s/%s", dir, LPK_MODE_RECORD);
        unlink(p);                       /* 每个用例都从"无记录"开始，互不影响 */
    }
}

int main(int argc, char **argv)
{
    if (argc < 7) {
        printf("用法: %s <dir_ok> <dir_incomplete> <dir_lpk> <dir_both> <dir_badlpk> <dir_empty>\n",
               argv[0]);
        return 2;
    }
    g_ok = argv[1]; g_inc = argv[2]; g_lpk = argv[3];
    g_both = argv[4]; g_bad = argv[5]; g_empty = argv[6];

    printf("========== M-1 必备清单 ==========\n");
    {
        int n = 0;
        const char *const *req = lpk_mode_required(&n);
        CHECK(n >= 9, "必备资源 %d 条（口径唯一，两模式共用）", n);
        CHECK(req && req[0] && strcmp(req[0], "ABC.MKF") == 0, "首条是 ABC.MKF（%s）",
              req ? req[0] : "(null)");
        char why[160] = {0};
        CHECK(lpk_mode_dir_ready(g_ok, why, sizeof(why)), "完整目录自检通过");
        CHECK(!lpk_mode_dir_ready(g_inc, why, sizeof(why)), "缺一条必需文件 → 自检失败（%s）", why);
    }

    printf("\n========== M-2 安装记录读写 ==========\n");
    {
        char p[300];
        snprintf(p, sizeof(p), "%s/%s", g_empty, LPK_MODE_RECORD);
        CHECK(lpk_mode_record_read(g_empty) == -1, "没有记录 → -1");
        CHECK(lpk_mode_record_write(g_empty, 1) == 0 && lpk_mode_record_read(g_empty) == 1,
              "写 lpk → 读回 1");
        CHECK(lpk_mode_record_write(g_empty, 0) == 0 && lpk_mode_record_read(g_empty) == 0,
              "写 zip → 读回 0");
        CHECK(lpk_mode_record_write(g_empty, 1) == 0, "再写 lpk");
        /* 手工塞垃圾：必须当"没有记录"处理，不能瞎猜 */
        FILE *f = fopen(p, "wb");
        if (f) { fwrite("garbage\n", 1, 8, f); fclose(f); }
        CHECK(lpk_mode_record_read(g_empty) == -1, "内容非法 → 视为无记录");
        unlink(p);
    }

    printf("\n========== M-3 包自检 ==========\n");
    {
        char why[160] = {0};
        char p[300];
        snprintf(p, sizeof(p), "%s/pal.lpk", g_lpk);
        CHECK(lpk_mode_lpk_ready(p, NULL, 0, why, sizeof(why)), "完好包通过自检");
        snprintf(p, sizeof(p), "%s/pal.lpk", g_bad);
        CHECK(!lpk_mode_lpk_ready(p, NULL, 0, why, sizeof(why)), "截断包被拒（%s）", why);
        CHECK(!lpk_mode_lpk_ready("/nonexistent/pal.lpk", NULL, 0, why, sizeof(why)),
              "不存在的包被拒（%s）", why);
    }

    printf("\n========== M-4 判定矩阵 ==========\n");
    expect("无记录 + 只有目录",            g_ok,    -1, LPK_RES_DIR);
    expect("无记录 + 只有包",              g_lpk,   -1, LPK_RES_LPK);
    expect("无记录 + 两者都有(目录优先)",  g_both,  -1, LPK_RES_DIR);
    expect("记录=lpk + 两者都有",          g_both,   1, LPK_RES_LPK);
    expect("记录=zip + 两者都有",          g_both,   0, LPK_RES_DIR);
    expect("记录=zip + 只有包(回落包)",    g_lpk,    0, LPK_RES_LPK);
    expect("记录=lpk + 只有目录(回落目录)", g_ok,    1, LPK_RES_DIR);
    expect("记录=lpk + 包坏 + 目录完整",   g_bad,    1, LPK_RES_DIR);
    expect("无记录 + 包坏 + 目录完整",     g_bad,   -1, LPK_RES_DIR);
    expect("空目录",                       g_empty, -1, LPK_RES_NONE);
    expect("空目录 + 记录=lpk",            g_empty,  1, LPK_RES_NONE);

    printf("\n==== %d FAIL ====\n", fails);
    return fails ? 1 : 0;
}
