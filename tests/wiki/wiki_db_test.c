/* wiki_db_test.c — 离线维基数据层主机测试
 *
 * 直接编译**生产源码** components/services/wiki/wiki_db.c + lz4_dec.c，
 * 用主机真实文件系统读 tests/wiki/data/testlib（由 gen_wiki_db.py 生成）。
 *
 * 编译：
 *   CC=clang
 *   $CC -I components/services/wiki -I tests/wiki/stub \
 *       tests/wiki/wiki_db_test.c components/services/wiki/wiki_db.c \
 *       components/services/wiki/lz4_dec.c -o /tmp/wt
 */
#include "wiki_db.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                    \
    if (cond) { g_pass++; printf("  PASS  "); }                   \
    else      { g_fail++; printf("  FAIL  "); }                   \
    printf(__VA_ARGS__); printf("\n");                            \
} while (0)

static const char *DBDIR = "tests/wiki/data/testlib";

/* 找一篇的 id（按标题精确匹配） */
static int find_id(const char *title) {
    for (int i = 0; i < wiki_count(); i++) {
        char t[WIKI_TITLE_MAX];
        if (wiki_title(i, t, sizeof(t)) && strcmp(t, title) == 0) return i;
    }
    return -1;
}

/* 统计一行在 400px 屏上占的宽度（3 字节=UTF-8 三字节序列算 16px，其余 8px） */
static int line_px(const char *s, int len) {
    int w = 0, i = 0;
    while (i < len) {
        if ((s[i] & 0xE0) == 0xE0 && i + 3 <= len) { w += 16; i += 3; }
        else { w += 8; i += 1; }
    }
    return w;
}

int main(void) {
    printf("========== 离线维基 数据层主机测试 ==========\n\n");

    /* ---------- T1 打开 ---------- */
    printf("T1 打开库\n");
    int n = wiki_open(DBDIR);
    CHECK(n == 31, "返回篇数 = 31 (实际 %d)", n);
    CHECK(wiki_is_open(), "is_open = true");
    CHECK(wiki_count() == 31, "count = 31");
    CHECK(strcmp(wiki_name(), "testlib") == 0, "库名 = testlib (实际 %s)", wiki_name());

    /* ---------- T2 中文前缀搜索 ---------- */
    printf("\nT2 中文前缀搜索\n");
    wiki_hit_t hits[50];
    int k = wiki_search("北", hits, 50);
    CHECK(k >= 1, "搜「北」命中 %d 条", k);
    int ok = 0;
    for (int i = 0; i < k; i++) if (strcmp(hits[i].title, "北京") == 0) ok = 1;
    CHECK(ok, "命中里有「北京」");

    k = wiki_search("长", hits, 50);
    CHECK(k >= 4, "搜「长」命中 %d 条（长安/长城/长文测试/长江/长城与运河）", k);
    for (int i = 0; i < k; i++) printf("        命中: %s\n", hits[i].title);

    k = wiki_search("长城", hits, 50);
    printf("        搜「长城」→ %d 条\n", k);
    CHECK(k == 2, "「长城」精确前缀命中 2 条（长城 / 长城与运河）");

    /* ---------- T3 无结果 ---------- */
    printf("\nT3 无结果与边界\n");
    CHECK(wiki_search("不存在的东西", hits, 50) == 0, "无结果返回 0");
    CHECK(wiki_search("", hits, 50) == 0, "空查询返回 0");
    CHECK(wiki_search(NULL, hits, 50) == 0, "NULL 查询返回 0");
    CHECK(wiki_search("北", NULL, 50) == 0, "NULL 输出返回 0");
    CHECK(wiki_search("北", hits, 0) == 0, "max=0 返回 0");

    /* ---------- T4 超长查询（越界读风险点） ---------- */
    printf("\nT4 超长查询（对应审计 P2：memcmp 可能越界）\n");
    {
        char big[400];
        memset(big, 'Z', sizeof(big) - 1);
        big[sizeof(big) - 1] = 0;
        int r = wiki_search(big, hits, 50);   /* 期望 0 且不崩 */
        CHECK(r == 0, "400 字节查询返回 0 且未崩溃");
    }

    /* ---------- T5 拼音搜索 ---------- */
    printf("\nT5 拼音搜索\n");
    k = wiki_search_py("bj", 1, hits, 50);
    ok = 0;
    for (int i = 0; i < k; i++) if (strcmp(hits[i].title, "北京") == 0) ok = 1;
    CHECK(k >= 1 && ok, "首字母 bj → %d 条，含北京", k);

    k = wiki_search_py("beijing", 0, hits, 50);
    ok = 0;
    for (int i = 0; i < k; i++) if (strcmp(hits[i].title, "北京") == 0) ok = 1;
    CHECK(k >= 1 && ok, "全拼 beijing → %d 条，含北京", k);

    k = wiki_search_py("chang", 0, hits, 50);
    CHECK(k >= 4, "前缀 chang → %d 条", k);

    k = wiki_search_py("zzzz", 0, hits, 50);
    CHECK(k == 0, "不存在的拼音返回 0");

    /* ---------- T6 顺序浏览 ---------- */
    printf("\nT6 顺序取标题\n");
    wiki_hit_t rows[32];
    int got = wiki_titles(0, 10, rows);
    CHECK(got == 10, "titles(0,10) 取到 %d 条", got);
    CHECK(rows[0].id == 0 && rows[9].id == 9, "id 连续 0..9");
    int sorted = 1;
    for (int i = 1; i < got; i++)
        if (strcmp(rows[i - 1].title, rows[i].title) > 0) sorted = 0;
    CHECK(sorted, "返回顺序按标题升序（与索引一致）");
    for (int i = 0; i < got; i++) printf("        %2d. %s\n", rows[i].id, rows[i].title);
    CHECK(wiki_titles(28, 10, rows) == 3, "末尾越界自动截断（28..31 → 3 条）");
    CHECK(wiki_titles(999, 10, rows) == 0, "from 越界返回 0");

    /* ---------- T7 载入普通文章 ---------- */
    printf("\nT7 载入文章\n");
    int idbj = find_id("北京");
    CHECK(idbj >= 0, "找到「北京」 id=%d", idbj);
    int lines = wiki_load(idbj);
    CHECK(lines > 0, "load 返回行数 %d", lines);
    CHECK(wiki_lines() == lines, "wiki_lines 与返回值一致");

    int has_beijing = 0;
    for (int i = 0; i < wiki_lines(); i++) {
        int len = 0;
        const char *s = wiki_line(i, &len);
        if (len > 0 && strstr(s, "首都") && (size_t)len >= strlen("首都")) has_beijing = 1;
    }
    CHECK(has_beijing, "正文里能取到「首都」");

    /* HTML 标签必须被剥掉（注意：&lt; 解码出的 '<' 属于正文，不该算标签残留） */
    int no_tag = 1;
    for (int i = 0; i < wiki_lines(); i++) {
        int len = 0;
        const char *s = wiki_line(i, &len);
        for (int j = 0; j + 2 < len; j++) {
            if (s[j] != '<') continue;
            /* 只看 "<字母" 或 "</" 这种真正的标签起始 */
            if (s[j + 1] == '/' || (s[j + 1] >= 'a' && s[j + 1] <= 'z') ||
                (s[j + 1] >= 'A' && s[j + 1] <= 'Z')) no_tag = 0;
        }
    }
    CHECK(no_tag, "所有行都不含 HTML 标签（< 开头的标签已剥离）");

    /* 行宽不超过 384px */
    int maxpx = 0, over = 0;
    for (int i = 0; i < wiki_lines(); i++) {
        int len = 0;
        const char *s = wiki_line(i, &len);
        int px = line_px(s, len);
        if (px > maxpx) maxpx = px;
        if (px > 384) over++;
    }
    CHECK(over == 0, "所有行宽 <= 384px（实测最宽 %dpx）", maxpx);

    /* ---------- T8 实体与特殊字符 ---------- */
    printf("\nT8 HTML 实体\n");
    int idq = find_id("气候变化");
    CHECK(idq >= 0, "找到「气候变化」");
    wiki_load(idq);
    int lit_amp = 0, lit_lt = 0, has_amp_char = 0, has_lt_char = 0;
    for (int i = 0; i < wiki_lines(); i++) {
        int len = 0;
        const char *s = wiki_line(i, &len);
        for (int j = 0; j < len; j++) {
            if (s[j] == '&' && j + 5 <= len && memcmp(s + j, "&amp;", 5) == 0) lit_amp = 1;
            if (s[j] == '&' && j + 4 <= len && memcmp(s + j, "&lt;",  4) == 0) lit_lt = 1;
            if (s[j] == '&') has_amp_char = 1;   /* 解码后应留下单个 '&' */
            if (s[j] == '<') has_lt_char = 1;    /* &lt; 解码后应留下 '<' */
        }
    }
    CHECK(!lit_amp, "&amp; 字面不再残留（已解码成 &）");
    CHECK(!lit_lt, "&lt; 字面不再残留（已解码成 <）");
    CHECK(has_amp_char, "正文里确实出现了解码后的 & 字符");
    CHECK(has_lt_char, "正文里确实出现了解码后的 < 字符");

    /* ---------- T9 长文 ---------- */
    printf("\nT9 长文（400 段）\n");
    int idl = find_id("长文测试");
    CHECK(idl >= 0, "找到「长文测试」");
    lines = wiki_load(idl);
    CHECK(lines > 300, "长文行数 %d（> 300）", lines);
    maxpx = 0; over = 0;
    for (int i = 0; i < wiki_lines(); i++) {
        int len = 0;
        const char *s = wiki_line(i, &len);
        int px = line_px(s, len);
        if (px > maxpx) maxpx = px;
        if (px > 384) over++;
    }
    CHECK(over == 0, "长文所有行宽 <= 384px（最宽 %dpx）", maxpx);
    /* 末尾数据要能读到 */
    {
        int len = 0;
        const char *s = wiki_line(wiki_lines() - 1, &len);
        printf("        最后一行: %.*s\n", len > 60 ? 60 : len, s);
        CHECK(len > 0, "最后一行非空（末尾数据可读）");
    }
    /* 顺序完整性：抽查第 1/200/400 段都在 */
    int seg1 = 0, seg200 = 0, seg400 = 0;
    for (int i = 0; i < wiki_lines(); i++) {
        int len = 0;
        const char *s = wiki_line(i, &len);
        char tmp[64]; int c = len < 63 ? len : 63;
        memcpy(tmp, s, (size_t)c); tmp[c] = 0;
        if (strstr(tmp, "第 001 段")) seg1 = 1;
        if (strstr(tmp, "第 200 段")) seg200 = 1;
        if (strstr(tmp, "第 400 段")) seg400 = 1;
    }
    CHECK(seg1 && seg200 && seg400, "首/中/末段都能读到（1=%d 200=%d 400=%d）", seg1, seg200, seg400);

    /* ---------- T10 损坏条目与越界 ---------- */
    printf("\nT10 异常路径\n");
    int idbad = find_id("坏条目");
    CHECK(idbad >= 0, "找到「坏条目」 id=%d", idbad);
    int rc = wiki_load(idbad);
    CHECK(rc == -3, "损坏条目返回 -3（实际 %d）", rc);
    CHECK(wiki_load(-1) == -2, "id=-1 返回 -2");
    CHECK(wiki_load(9999) == -2, "id 越界返回 -2");

    /* ---------- T11 行越界 ---------- */
    printf("\nT11 行访问边界\n");
    {
        int len = 99;
        const char *s = wiki_line(-1, &len);
        CHECK(len == 0 && s && s[0] == 0, "负行号返回空串且 len=0");
        len = 99;
        wiki_line(999999, &len);
        CHECK(len == 0, "超大行号返回 len=0");
    }

    /* ---------- T12 关闭与重开 ---------- */
    printf("\nT12 关闭 / 重开\n");
    wiki_close();
    CHECK(!wiki_is_open(), "close 后 is_open = false");
    CHECK(wiki_count() == 0, "close 后 count = 0");
    CHECK(wiki_load(0) == -1, "close 后 load 返回 -1");
    CHECK(wiki_search("北", hits, 50) == 0, "close 后搜索返回 0");

    n = wiki_open(DBDIR);
    CHECK(n == 31, "重新打开成功（%d 篇）", n);

    /* 不存在目录 */
    CHECK(wiki_open("/nonexistent/wiki/dir") == 0, "不存在的目录返回 0");
    CHECK(!wiki_is_open(), "失败后 is_open = false");

    wiki_close();
    printf("\n================ %d PASS / %d FAIL ================\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
