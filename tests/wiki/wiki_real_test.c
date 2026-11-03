/* wiki_real_test.c — 离线维基：真实 LZ4 压缩 + 安全加固验收
 *
 * 与 wiki_db_test.c 的分工：
 *   wiki_db_test.c   手工 literal-only LZ4 库（tests/wiki/data/testlib），验基本语义
 *   本文件           真实 lz4.block 压缩库（tests/wiki/data/reallib），验生产链路
 *
 * 编译（ASan 版本，P1 越界修复的验收就靠它）：
 *   CC=clang
 *   $CC -I components/services/wiki -I tests/wiki/stub \
 *       -fsanitize=address -g -O1 \
 *       tests/wiki/wiki_real_test.c components/services/wiki/wiki_db.c \
 *       components/services/wiki/lz4_dec.c tests/wiki/stub/esp_stub.c -o /tmp/wr
 *
 * 用法： /tmp/wr [库目录]     默认 tests/wiki/data/reallib
 */
#include "wiki_db.h"
#include "esp_heap_caps.h"      /* g_stub_psram_largest: 模拟 PSRAM 不足 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                    \
    if (cond) { g_pass++; printf("  PASS  "); }                   \
    else      { g_fail++; printf("  FAIL  "); }                   \
    printf(__VA_ARGS__); printf("\n");                            \
} while (0)

static const char *DBDIR = "tests/wiki/data/reallib";

static int find_id(const char *title) {
    for (int i = 0; i < wiki_count(); i++) {
        char t[WIKI_TITLE_MAX];
        if (wiki_title(i, t, sizeof(t)) && strcmp(t, title) == 0) return i;
    }
    return -1;
}

/* 把整篇正文拼起来（每行之间补空格），用于内容断言 */
static char *grab_text(size_t *out_len) {
    size_t cap = 1 << 20, n = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (int i = 0; i < wiki_lines(); i++) {
        int len = 0;
        const char *s = wiki_line(i, &len);
        if (n + (size_t)len + 2 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        memcpy(buf + n, s, (size_t)len);
        n += (size_t)len;
        buf[n++] = ' ';
    }
    buf[n] = 0;
    if (out_len) *out_len = n;
    return buf;
}

static int line_px(const char *s, int len) {
    int w = 0, i = 0;
    while (i < len) {
        if ((s[i] & 0xE0) == 0xE0 && i + 3 <= len) { w += 16; i += 3; }
        else { w += 8; i += 1; }
    }
    return w;
}

int main(int argc, char **argv) {
    if (argc > 1) DBDIR = argv[1];

    printf("========== 离线维基 真实 LZ4 + 安全加固测试 ==========\n");
    printf("库目录: %s\n\n", DBDIR);

    /* ---------- T1 打开 ---------- */
    printf("T1 打开真实压缩库\n");
    int n = wiki_open(DBDIR);
    CHECK(n == 14, "返回篇数 = 14（实际 %d）", n);
    CHECK(wiki_is_open(), "is_open = true");

    wiki_hit_t hits[64];

    /* ---------- T2 真实 LZ4 解压正确性 ---------- */
    printf("\nT2 真实 LZ4（含 match/offset）解压\n");
    int id = find_id("北京");
    CHECK(id >= 0, "找到「北京」");
    {
        int lines = wiki_load(id);
        CHECK(lines > 0, "load 返回到 %d 行", lines);
        size_t tl = 0;
        char *txt = grab_text(&tl);
        CHECK(txt && strstr(txt, "首都"), "解压后能读到「首都」");
        CHECK(txt && strstr(txt, "政治"), "解压后能读到「政治」");
        free(txt);
    }

    /* ---------- T3 高重复文本（压缩率极高，全 match） ---------- */
    printf("\nT3 高重复率文本（LZ4 match 密集）\n");
    id = find_id("高重复测试");
    CHECK(id >= 0, "找到「高重复测试」");
    {
        int lines = wiki_load(id);
        CHECK(lines > 0, "load 返回 %d 行", lines);
        size_t tl = 0;
        char *txt = grab_text(&tl);
        /* 原文 300 遍 × 13 字 ≈ 3900 字，去标签后应远大于 3000 字节 */
        CHECK(tl > 3000, "解压后正文长度 %zu 字节（> 3000）", tl);
        CHECK(txt && strstr(txt, "重复的内容"), "内容正确（重复段存在）");
        free(txt);
    }

    /* ---------- T4 长文：首/中/末段 ---------- */
    printf("\nT4 长文 500 段\n");
    id = find_id("长文");
    CHECK(id >= 0, "找到「长文」");
    {
        int lines = wiki_load(id);
        CHECK(lines > 400, "行数 %d（> 400）", lines);
        size_t tl = 0;
        char *txt = grab_text(&tl);
        CHECK(txt && strstr(txt, "第 001 段"), "首段可读");
        CHECK(txt && strstr(txt, "第 250 段"), "中段可读");
        CHECK(txt && strstr(txt, "第 500 段"), "末段可读");

        int over = 0, maxpx = 0;
        for (int i = 0; i < wiki_lines(); i++) {
            int len = 0;
            const char *s = wiki_line(i, &len);
            int px = line_px(s, len);
            if (px > maxpx) maxpx = px;
            if (px > 384) over++;
        }
        CHECK(over == 0, "所有行 <= 384px（最宽 %dpx）", maxpx);
        free(txt);
    }

    /* ---------- T5 HTML 实体 ---------- */
    printf("\nT5 实体解码\n");
    id = find_id("实体");
    CHECK(id >= 0, "找到「实体」");
    {
        wiki_load(id);
        size_t tl = 0;
        char *txt = grab_text(&tl);
        CHECK(txt && !strstr(txt, "&amp;"), "&amp; 已解码");
        CHECK(txt && !strstr(txt, "&lt;"),  "&lt; 已解码");
        CHECK(txt && strstr(txt, "中文"),   "十六进制实体 &#x4E2D;&#x6587; 解成「中文」");
        free(txt);
    }

    /* ---------- T6 边界尺寸 ---------- */
    printf("\nT6 边界尺寸（64KB / 1MB）\n");
    id = find_id("边界64K");
    CHECK(id >= 0, "找到「边界64K」");
    if (id >= 0) {
        int lines = wiki_load(id);
        CHECK(lines > 100, "64KB 正文 → %d 行", lines);
    }
    id = find_id("边界1M");
    CHECK(id >= 0, "找到「边界1M」");
    if (id >= 0) {
        int lines = wiki_load(id);
        CHECK(lines > 1000, "1MB 正文 → %d 行", lines);
        int len = 0;
        const char *last = wiki_line(wiki_lines() - 1, &len);
        CHECK(len > 0, "1MB 文章最后一行非空（末尾可读）");
        (void)last;
    }

    /* ---------- T7 空段 ---------- */
    printf("\nT7 空正文\n");
    id = find_id("空段");
    CHECK(id >= 0, "找到「空段」");
    if (id >= 0) {
        int lines = wiki_load(id);
        CHECK(lines == 0, "只有空 <p> 时返回 0 行（实际 %d）", lines);
    }

    /* ---------- T8 拼音（多音字 + 句柄复用） ---------- */
    printf("\nT8 拼音搜索与多音字\n");
    {
        struct { const char *q; int ini; const char *want; } cases[] = {
            {"cq",      1, "重庆"}, {"chongqing", 0, "重庆"},
            {"cs",      1, "长沙"}, {"changsha",  0, "长沙"},
            {"ca",      1, "长安"}, {"changan",   0, "长安"},
            {"xm",      1, "厦门"}, {"xiamen",    0, "厦门"},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            int k = wiki_search_py(cases[i].q, cases[i].ini, hits, 64);
            int ok = 0;
            for (int j = 0; j < k; j++) if (strcmp(hits[j].title, cases[i].want) == 0) ok = 1;
            CHECK(ok, "search_py(\"%s\", ini=%d) 命中「%s」", cases[i].q, cases[i].ini, cases[i].want);
        }
    }

    /* ---------- T9 超长查询（P1 越界修复） ---------- */
    printf("\nT9 超长查询（P1：栈越界读）\n");
    {
        char *q = malloc(4097);
        memset(q, 'A', 4096);
        q[4096] = 0;

        CHECK(wiki_search(q, hits, 64) == 0, "4096 字节查询返回 0");

        memset(q, 'Z', 200); q[200] = 0;
        CHECK(wiki_search(q, hits, 64) == 0, "200 字节查询返回 0");

        /* 64 字节（正好越过 63 上限一个字节）：标题最长 63，必然无匹配 */
        memset(q, 'B', 64); q[64] = 0;
        CHECK(wiki_search(q, hits, 64) == 0, "64 字节查询返回 0 且不越界");

        /* 63 字节：仍在合法范围，正常走二分 */
        memset(q, 'C', 63); q[63] = 0;
        CHECK(wiki_search(q, hits, 64) == 0, "63 字节查询返回 0（无此标题，但不越界）");

        /* UTF-8 多字节：中文 22 字 = 66 字节，> 63 ⇒ 走"无匹配"分支，不切字 */
        strcpy(q, "这是一个很长的中文查询用来测试多字节边界情况的词条名称呀");
        size_t ql = strlen(q);
        CHECK(wiki_search(q, hits, 64) == 0, "多字节长查询（%zu 字节）返回 0", ql);

        free(q);
    }

    /* ---------- T10 坏帧 ---------- */
    printf("\nT10 损坏帧检测\n");
    id = find_id("坏魔术");
    CHECK(id >= 0, "找到「坏魔术」");
    if (id >= 0) {
        int rc = wiki_load(id);
        CHECK(rc == -5, "坏 magic 返回 -5（实际 %d）", rc);
    }
    id = find_id("坏截断");
    CHECK(id >= 0, "找到「坏截断」");
    if (id >= 0) {
        int rc = wiki_load(id);
        CHECK(rc == -7 || rc == -6, "压缩块不完整返回 -7/-6（实际 %d）", rc);
    }
    id = find_id("坏长度");
    CHECK(id >= 0, "找到「坏长度」");
    if (id >= 0) {
        int rc = wiki_load(id);
        CHECK(rc == -5, "ulen 与索引不符返回 -5（实际 %d）", rc);
    }

    /* 坏帧之后必须还能正常读好文章（缓冲复用不能把状态带坏） */
    id = find_id("北京");
    CHECK(wiki_load(id) > 0, "坏帧失败后仍能正常载入「北京」");

    /* ---------- T11 缓冲复用：大→小→大 ---------- */
    printf("\nT11 正文缓冲复用（1MB → 小篇 → 1MB）\n");
    {
        int idbig = find_id("边界1M");
        int idsmall = find_id("北京");
        int l1 = wiki_load(idbig);
        int l2 = wiki_load(idsmall);
        int l3 = wiki_load(idbig);
        CHECK(l1 > 1000 && l2 > 0 && l3 == l1, "三次切换行数稳定（%d → %d → %d）", l1, l2, l3);

        size_t tl = 0;
        char *txt = grab_text(&tl);
        CHECK(txt && strstr(txt, "一兆字节边界测试"), "切换回来后内容仍然正确");
        free(txt);

        /* 小篇内容不能被 1MB 篇的残留污染 */
        wiki_load(idsmall);
        txt = grab_text(&tl);
        CHECK(txt && strstr(txt, "首都"), "小篇内容正确（无上一轮的残留）");
        CHECK(txt && !strstr(txt, "一兆字节"), "小篇里不含大篇的文字");
        free(txt);
    }

    /* ---------- T12 PSRAM 不足 ---------- */
    printf("\nT12 PSRAM 不足路径\n");
    {
        int idbig = find_id("边界1M");
        size_t save = g_stub_psram_largest;
        g_stub_psram_largest = 4096;                /* 只给 4KB，1MB 文章必然装不下 */
        int rc = wiki_load(idbig);
        CHECK(rc == -8, "PSRAM 不足返回 -8（实际 %d）", rc);
        g_stub_psram_largest = save;

        rc = wiki_load(idbig);
        CHECK(rc > 1000, "恢复 PSRAM 后同一篇能正常载入（%d 行）", rc);
    }

    /* ---------- T13 连续 200 次搜索 / 20 篇连读 ---------- */
    printf("\nT13 压力：200 次搜索 + 20 篇连读\n");
    {
        const char *qs[] = {"bj", "beijing", "cs", "c", "z", "zzzz", "", "chang"};
        int total_hits = 0;
        for (int i = 0; i < 200; i++) {
            const char *q = qs[i % (int)(sizeof(qs) / sizeof(qs[0]))];
            total_hits += wiki_search_py(q, (i & 1), hits, 64);
            /* 标题搜索混着来，两条路径都要压到 */
            char tq[8] = {0};
            if (i % 3 == 0) { tq[0] = (char)('A' + (i % 26)); wiki_search(tq, hits, 64); }
        }
        CHECK(1, "200 次混合搜索完成（累计命中 %d 条，无崩溃/泄漏）", total_hits);

        /* 20 篇连读：3 篇是故意做坏的帧，必须失败；其余必须全部成功。
         * 断言要按「已知坏帧」排除，不能要求 20/20 —— 那等于要求坏帧能读。 */
        int bad_ids[8], nbad = 0;
        const char *badnames[] = {"坏魔术", "坏截断", "坏长度"};
        for (size_t i = 0; i < 3; i++) {
            int bid = find_id(badnames[i]);
            if (bid >= 0) bad_ids[nbad++] = bid;
        }
        int loads = 0, good_total = 0;
        for (int i = 0; i < 20; i++) {
            int rid = i % wiki_count();
            int is_bad = 0;
            for (int b = 0; b < nbad; b++) if (bad_ids[b] == rid) is_bad = 1;
            int rc = wiki_load(rid);
            if (is_bad) {
                if (rc < 0) loads++;                /* 坏帧：失败才算对 */
            } else {
                good_total++;
                if (rc >= 0) loads++;               /* 好文章：必须 >= 0 行 */
            }
        }
        CHECK(loads == 20, "20 篇连续载入结果全符合预期（%d/20，其中坏帧 %d 篇按预期失败）",
              loads, nbad);
        (void)good_total;
    }

    /* ---------- T14 关闭 / 重开 ---------- */
    printf("\nT14 关闭与重开（句柄全部释放）\n");
    wiki_close();
    CHECK(!wiki_is_open(), "close 后 is_open = false");
    CHECK(wiki_load(0) == -1, "close 后 load 返回 -1");
    CHECK(wiki_search_py("bj", 1, hits, 64) == 0, "close 后拼音搜索返回 0");
    n = wiki_open(DBDIR);
    CHECK(n == 14, "重新打开成功（%d 篇）", n);
    CHECK(wiki_search_py("bj", 1, hits, 64) >= 1, "重开后拼音搜索恢复");
    wiki_close();

    printf("\n================ %d PASS / %d FAIL ================\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
