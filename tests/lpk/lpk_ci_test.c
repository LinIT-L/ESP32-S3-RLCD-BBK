/**
 * lpk_ci_test.c — G1 大小写兼容 + 重名歧义 + 非法路径（纯内存，不需要磁盘）
 *
 * 为什么单独测：SDLPal 全是小写文件名（"abc.mkf"）而磁盘上是 ABC.MKF，
 * 一旦查找区分大小写，游戏开局就 TerminateOnError —— 这不是"偶发找不到资源"，
 * 是**必崩**。所以这条要有独立、可重复的用例。
 */
#include <stdio.h>
#include <string.h>

#include "lpk1.h"

static int fails = 0;
/* ⚠ 条件只求值**一次**：早先版本把 (c) 写了三遍，带副作用的表达式
 * （lpk_m_write / lpk_m_open）会被执行两三次 —— 曾导致"写 100 B 结果文件 200 B"。 */
#define CHECK(c, ...) do { \
    int _ok = (c) ? 1 : 0; \
    printf(_ok ? "  PASS  " : "  FAIL  "); printf(__VA_ARGS__); printf("\n"); \
    if (!_ok) fails++; } while (0)

static lpk_entry_t en[8];
static lpk_t h;

static void mk(int i, const char *name, uint32_t size, uint32_t off) {
    snprintf(en[i].name, sizeof(en[i].name), "%s", name);
    en[i].size = size;
    en[i].off = off;
    en[i].crc32 = 0;
}

int main(void) {
    memset(&h, 0, sizeof(h));
    memset(en, 0, sizeof(en));
    mk(0, "ABC.MKF", 1000, 4096);
    mk(1, "music/39.mp3", 500, 8192);
    mk(2, "SETUP.DAT", 20, 16384);
    h.loaded = 3;
    h.count = 3;

    printf("========== G1-1 精确匹配（原有行为不变）==========\n");
    CHECK(lpk_find(&h, en, "ABC.MKF") == 0, "lpk_find(\"ABC.MKF\") = %d", lpk_find(&h, en, "ABC.MKF"));
    CHECK(lpk_find(&h, en, "abc.mkf") == -1, "lpk_find 仍**区分**大小写（契约不变）");
    CHECK(lpk_find_ci(&h, en, "ABC.MKF") == 0, "lpk_find_ci 精确命中也走下标 0");

    printf("\n========== G1-2 大小写混合 ==========\n");
    struct { const char *q; int want; } cases[] = {
        { "abc.mkf",        0 },
        { "Abc.Mkf",        0 },
        { "aBc.mKf",        0 },
        { "MUSIC/39.MP3",   1 },
        { "Music/39.Mp3",   1 },
        { "setup.dat",      2 },
        { "SETUP.DAT",      2 },
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        int got = lpk_find_ci(&h, en, cases[i].q);
        CHECK(got == cases[i].want, "lpk_find_ci(\"%-12s\") = %d (期望 %d)",
              cases[i].q, got, cases[i].want);
    }

    printf("\n========== G1-3 未命中 / 空查询 ==========\n");
    CHECK(lpk_find_ci(&h, en, "NOPE.MKF") == -1, "不存在的资源返回 -1");
    CHECK(lpk_find_ci(&h, en, "AB") == -1, "前缀不算命中（不是模糊搜索）");
    CHECK(lpk_find_ci(&h, en, "ABC.MKF.bak") == -1, "更长的名字不算命中");
    CHECK(lpk_find_ci(&h, en, "") == -1, "空串返回 -1");
    CHECK(lpk_find_ci(&h, en, NULL) == -1, "NULL 返回 -1");

    printf("\n========== G1-4 重名歧义（仅大小写不同）==========\n");
    /* 打包器已禁止这种包；读取端必须再拦一次，不能随便挑一个。
     * 注意：查询串与两条都只差大小写时才是歧义；精确命中优先。 */
    lpk_entry_t amb[3];
    memset(amb, 0, sizeof(amb));
    snprintf(amb[0].name, sizeof(amb[0].name), "DATA.MKF");
    snprintf(amb[1].name, sizeof(amb[1].name), "data.mkf");
    amb[0].size = amb[1].size = 10;
    h.loaded = 2;
    CHECK(lpk_find_ci(&h, amb, "DATA.MKF") == 0, "精确命中 DATA.MKF → 0（不报歧义）");
    CHECK(lpk_find_ci(&h, amb, "data.mkf") == 1, "精确命中 data.mkf → 1（不报歧义）");
    CHECK(lpk_find_ci(&h, amb, "Data.Mkf") == LPK_E_AMBIG,
          "混合大小写查询命中两条 → LPK_E_AMBIG(%d)", LPK_E_AMBIG);

    printf("\n========== G1-5 非法路径（与 lpk_path_ok 一致）==========\n");
    struct { const char *p; int ok; } paths[] = {
        { "ABC.MKF",        1 },
        { "music/39.mp3",   1 },
        { "音乐/词条.dat",   1 },     /* UTF-8 多字节必须放行 */
        { "../etc/passwd",  0 },
        { "music/../../x",  0 },
        { "/etc/passwd",    0 },
        { "music\\39.mp3",  0 },
        { "C:/x",           0 },
        { "",               0 },
        { NULL,             0 },
    };
    for (unsigned i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        bool got = lpk_path_ok(paths[i].p);
        CHECK(got == (bool)paths[i].ok, "lpk_path_ok(%-16s) = %d",
              paths[i].p ? paths[i].p : "(NULL)", (int)got);
    }

    printf("\n==== %d FAIL ====\n", fails);
    return fails ? 1 : 0;
}
