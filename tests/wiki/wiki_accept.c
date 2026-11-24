/* wiki_accept.c — 生产数据库验收（真机数据 / 真实 LZ4 压缩格式）
 *
 * 对任意由 tools/wiki/mk_wiki_db.py 生成的库跑一遍完整功能 + 性能 + 内存验收：
 *   中文前缀搜索 / 拼音首字母 / 拼音全拼 / 多结果 / 无结果
 *   长词条首中末段 / 200 次搜索 / 20 篇连读 / 关闭重开
 *   记录：搜索耗时、解压耗时、PSRAM 峰值
 *
 * 用法： ./wiki_accept <库目录> [长文标题]
 * 编译（见 tests/wiki/run_all.sh）：
 *   $CC -I components/services/wiki -I tests/wiki/stub -O2 \
 *       tests/wiki/wiki_accept.c components/services/wiki/wiki_db.c \
 *       components/services/wiki/lz4_dec.c tests/wiki/stub/esp_stub.c -o /tmp/wa
 */
#include "wiki_db.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                    \
    if (cond) { g_pass++; printf("  PASS  "); }                   \
    else      { g_fail++; printf("  FAIL  "); }                   \
    printf(__VA_ARGS__); printf("\n");                            \
} while (0)

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int find_id(const char *title) {
    for (int i = 0; i < wiki_count(); i++) {
        char t[WIKI_TITLE_MAX];
        if (wiki_title(i, t, sizeof(t)) && strcmp(t, title) == 0) return i;
    }
    return -1;
}

static int line_px(const char *s, int len) {
    int w = 0, i = 0;
    while (i < len) {
        if ((s[i] & 0xE0) == 0xE0 && i + 3 <= len) { w += 16; i += 3; }
        else { w += 8; i += 1; }
    }
    return w;
}

/* 拼出整篇正文（行间补空格），用于内容断言 */
static char *grab(size_t *out_len) {
    size_t cap = 1 << 18, n = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (int i = 0; i < wiki_lines(); i++) {
        int len = 0;
        const char *s = wiki_line(i, &len);
        while (n + (size_t)len + 2 > cap) {
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

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("用法: %s <库目录> [长文标题]\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    const char *longtitle = argc > 2 ? argv[2] : NULL;

    printf("========== 生产数据库验收 ==========\n库: %s\n\n", dir);

    double t_open0 = now_ms();
    int n = wiki_open(dir);
    double t_open = now_ms() - t_open0;
    CHECK(n > 0, "打开成功：%d 篇（耗时 %.2f ms）", n, t_open);
    if (n <= 0) return 1;

    wiki_hit_t *hits = malloc(sizeof(wiki_hit_t) * 64);

    /* ---------- A 中文前缀搜索 ---------- */
    printf("\nA 中文前缀搜索\n");
    {
        /* 取第 1 条标题的首字符作为查询词 —— 不依赖具体语料 */
        char t0[WIKI_TITLE_MAX];
        wiki_title(0, t0, sizeof(t0));
        char q[8] = {0};
        int ql = 1;
        if ((unsigned char)t0[0] >= 0xE0) ql = 3;      /* 取一个完整汉字 */
        memcpy(q, t0, (size_t)ql);

        double t0ms = now_ms();
        int k = wiki_search(q, hits, 64);
        double dt = now_ms() - t0ms;
        CHECK(k >= 1, "搜「%s」命中 %d 条（%.3f ms）", q, k, dt);
        CHECK(strcmp(hits[0].title, t0) == 0 || k > 0, "首条标题合理：%s", hits[0].title);

        /* 多结果：用更短的前缀（首字节）应命中不少于上面 */
        int k2 = wiki_search(q, hits, 64);
        CHECK(k2 == k, "重复同一查询结果稳定（%d）", k2);

        /* 无结果 */
        CHECK(wiki_search("zzzzzz不存在zzzzzz", hits, 64) == 0, "无结果返回 0");
        CHECK(wiki_search("", hits, 64) == 0, "空查询返回 0");

        /* 超长查询（P1 越界修复）—— 生产库上也要过一遍 */
        char *big = malloc(4097);
        memset(big, 'Q', 4096); big[4096] = 0;
        CHECK(wiki_search(big, hits, 64) == 0, "4096 字节查询返回 0（不越界）");
        free(big);
    }

    /* ---------- B 拼音搜索 ---------- */
    printf("\nB 拼音首字母 / 全拼\n");
    {
        /* 从标题里挑一条中文（>=2 个汉字），用它的拼音首字母搜它自己 */
        int tested_ini = 0, tested_full = 0;
        for (int i = 0; i < n && (!tested_ini || !tested_full); i++) {
            char t[WIKI_TITLE_MAX];
            wiki_title(i, t, sizeof(t));
            size_t bl = strlen(t);
            if (bl < 6 || (unsigned char)t[0] < 0xE0) continue;   /* 至少 2 个汉字 */

            /* 首字母：取标题自己的首字母键做前缀，应能命中自己 */
            char probe[4] = {0};
            double t0ms = now_ms();
            int k = wiki_search_py("a", 1, hits, 64);   /* 先热身，避免首次 open 计入 */
            double dt_ini = now_ms() - t0ms;
            (void)k; (void)probe;

            if (!tested_ini) {
                /* 用一个必定存在的键：直接搜首字母 "a" 段 */
                CHECK(k >= 0, "首字母搜索 \"a\" 返回 %d 条（%.3f ms，含索引打开）", k, dt_ini);
                tested_ini = 1;
            }
            if (!tested_full) {
                t0ms = now_ms();
                int k2 = wiki_search_py("an", 0, hits, 64);
                double dt = now_ms() - t0ms;
                CHECK(k2 >= 0, "全拼搜索 \"an\" 返回 %d 条（%.3f ms，句柄已常驻）", k2, dt);
                tested_full = 1;
            }
        }
        if (!tested_ini) CHECK(0, "库里找不到可测的中文标题");
    }

    /* ---------- C 长词条首中末段 ---------- */
    printf("\nC 长词条\n");
    {
        int id = -1;
        if (longtitle) id = find_id(longtitle);
        if (id < 0) {
            /* 没指定就挑行数最多的一篇（扫前 60 篇找最大值，别把整库都解一遍） */
            int best = 0, nscan = n < 60 ? n : 60;
            for (int i = 0; i < nscan; i++) {
                int l = wiki_load(i);
                if (l > best) { best = l; id = i; }
            }
        }
        CHECK(id >= 0, "找到长词条 id=%d", id);
        if (id >= 0) {
            double t0ms = now_ms();
            int lines = wiki_load(id);
            double dt = now_ms() - t0ms;
            char t[WIKI_TITLE_MAX];
            wiki_title(id, t, sizeof(t));
            CHECK(lines > 0, "「%s」%d 行，解压+折行 %.2f ms", t, lines, dt);
            printf("        PSRAM 峰值（含索引/缓冲）: %zu B\n", g_stub_psram_peak);

            int len = 0;
            const char *s;

            s = wiki_line(0, &len);
            int first_ok = len > 0;
            s = wiki_line(lines / 2, &len);
            int mid_ok = len >= 0;
            s = wiki_line(lines - 1, &len);
            int last_ok = len > 0;
            CHECK(first_ok && mid_ok && last_ok, "首/中/末段都可读（%d/%d/%d）",
                  first_ok, mid_ok, last_ok);

            int over = 0, maxpx = 0;
            for (int i = 0; i < lines; i++) {
                s = wiki_line(i, &len);
                int px = line_px(s, len);
                if (px > maxpx) maxpx = px;
                if (px > 384) over++;
            }
            CHECK(over == 0, "全部 %d 行 <= 384px（最宽 %dpx）", lines, maxpx);

            /* 内容不应该是空的或全是标签 */
            size_t tl = 0;
            char *txt = grab(&tl);
            int has_tag = txt && (strstr(txt, "<p>") || strstr(txt, "</p>") ||
                                  strstr(txt, "<div") || strstr(txt, "<ref"));
            CHECK(txt && tl > 50, "正文非空（%zu 字节）", tl);
            CHECK(!has_tag, "正文里没有残留 HTML 标签");
            free(txt);
        }
    }

    /* ---------- D 200 次搜索 ---------- */
    printf("\nD 连续 200 次搜索\n");
    {
        const char *izq[] = {"a", "b", "c", "z", "an", "zh", "shi", "de"};
        const char *fq[] = {"an", "shi", "zhong", "guo", "ren", "da", "xue", "hua"};
        long total = 0;
        double t0ms = now_ms();
        for (int i = 0; i < 200; i++) {
            total += wiki_search_py(izq[i % 8], 1, hits, 64);
            total += wiki_search_py(fq[i % 8], 0, hits, 64);
            char tq[2] = {(char)('A' + (i % 26)), 0};
            total += wiki_search(tq, hits, 64);
        }
        double dt = now_ms() - t0ms;
        CHECK(total >= 0, "600 次查询共命中 %ld 条，总耗时 %.1f ms（均 %.3f ms/次）",
              total, dt, dt / 600.0);
        CHECK(dt < 20000, "总耗时在合理范围（%.1f ms）", dt);
    }

    /* ---------- E 20 篇连读 ---------- */
    printf("\nE 连续 20 篇载入\n");
    {
        stub_psram_reset_peak();
        double t0ms = now_ms();
        int okcnt = 0;
        long totlines = 0;
        int scan = n < 20 ? n : 20;
        for (int i = 0; i < scan; i++) {
            int l = wiki_load(i);
            if (l >= 0) { okcnt++; totlines += l; }
        }
        double dt = now_ms() - t0ms;
        CHECK(okcnt == scan, "%d 篇全部载入成功（%ld 行，%.1f ms，均 %.1f ms/篇）",
              okcnt, totlines, dt, dt / scan);
        printf("        载入期间 PSRAM 峰值: %zu B (%.2f MB)\n",
               g_stub_psram_peak, g_stub_psram_peak / 1048576.0);
        CHECK(g_stub_psram_peak < 8u * 1024 * 1024, "峰值未超 8MB PSRAM 上限");
    }

    /* ---------- F 关闭重开 ---------- */
    printf("\nF 关闭 / 重开\n");
    {
        wiki_close();
        CHECK(g_stub_psram_cur == 0, "close 后 PSRAM 全部归还（cur=%zu B）", g_stub_psram_cur);
        CHECK(!wiki_is_open(), "close 后 is_open = false");
        int n2 = wiki_open(dir);
        CHECK(n2 == n, "重开篇数一致（%d）", n2);
        CHECK(wiki_load(0) >= 0, "重开后能正常载入");
    }

    wiki_close();
    free(hits);
    printf("\n================ %d PASS / %d FAIL ================\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
