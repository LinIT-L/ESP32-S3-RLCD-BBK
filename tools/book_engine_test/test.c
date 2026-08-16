/* 电子书排版内核 (book_flow) 主机端一致性测试
 * 编译:  gcc -O2 -I ../../components/book_reader \
 *        -I ../../components/book_reader/libunibreak test.c \
 *        ../../components/book_reader/book_flow.c \
 *        (libunibreak 源文件: linebreak.c linebreakdata.c linebreakdef.c
 *         unibreakbase.c unibreakdef.c eastasianwidthdef.c) -o test
 * 运行:  ./test
 * 校验: 1) 分页不崩溃、页偏移严格递增且不超文件尾
 *        2) 每页从页首用同一内核重排版, 页边界必须等于下一页偏移 (渲染一致性)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>

#include "book_flow.h"

static uint32_t s_pages[200000];
static uint32_t s_page_n;

typedef struct {
    const uint8_t *data;
    uint32_t len;
} memfile_t;

static size_t mem_read(void *ud, uint32_t pos, uint8_t *buf, size_t want) {
    memfile_t *m = (memfile_t *)ud;
    if (pos >= m->len) return 0;
    size_t w = want;
    if (w > m->len - pos) w = m->len - pos;
    memcpy(buf, m->data + pos, w);
    return w;
}

static int add_page_cb(void *ud, uint32_t off) {
    (void)ud;
    if (s_page_n >= sizeof(s_pages) / sizeof(s_pages[0])) return -1;
    s_pages[s_page_n++] = off;
    return 0;
}

static int g_failures = 0;

static void fail(const char *fmt, ...) {
    g_failures++;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* 对一本内存书跑分页 + 逐页一致性校验 */
static void test_book(const char *name, const uint8_t *data, uint32_t len,
                      uint8_t enc, int rows, int line_max) {
    memfile_t mf = { data, len };
    bf_src_t src = { mem_read, &mf };
    s_page_n = 0;

    uint8_t *chunk = (uint8_t *)malloc(32768);
    bf_ch_t *win = (bf_ch_t *)malloc(sizeof(bf_ch_t) * BF_MAX_WIN);
    uint8_t *brk = (uint8_t *)malloc(BF_MAX_WIN);
    uint8_t *scratch = (uint8_t *)malloc(BF_MAX_WIN * 8 + 8);
    uint32_t *off = (uint32_t *)malloc(BF_MAX_WIN * 4);
    if (!chunk || !win || !brk || !scratch || !off) {
        fail("[%s] 分配失败", name);
        return;
    }

    uint32_t next;
    int r = bf_paginate(src, len, enc, 0, rows, line_max, 12, 24,
                        add_page_cb, NULL, NULL, NULL,
                        chunk, 32768, win, brk, scratch, BF_MAX_WIN * 8 + 8, off,
                        len, false, &next);
    if (r != 0 || next != len) {
        fail("[%s] bf_paginate 异常: r=%d next=%u len=%u", name, r, next, len);
    }
    if (s_page_n == 0) {
        fail("[%s] 无页面", name);
    }
    for (uint32_t p = 0; p < s_page_n; p++) {
        if (p > 0 && s_pages[p] <= s_pages[p - 1]) {
            fail("[%s] 页偏移非递增: p%u=%u p%u=%u", name, p, s_pages[p], p - 1, s_pages[p - 1]);
        }
        if (s_pages[p] > len) {
            fail("[%s] 页偏移越界: %u > %u", name, s_pages[p], len);
        }
        /* 渲染一致性: 从页首解码窗口 -> 排版 -> 页边界必须等于下一偏移 */
        uint32_t pos = s_pages[p];
        int n = 0;
        uint32_t cpos = pos;
        while (n < BF_MAX_WIN && cpos < len) {
            bf_ch_t ch = bf_next_ch(data + cpos, data + len, enc);
            if (ch.adv == 0) break;
            win[n] = ch;
            off[n] = cpos;
            n++;
            cpos += ch.adv;
        }
        if (n == 0) {
            fail("[%s] 页%u 无字符", name, p);
            continue;
        }
        uint32_t cpos_end = cpos;
        bf_breaks(win, n, enc, brk, scratch, BF_MAX_WIN * 8 + 8);
        int line_end[BF_MAX_LINES], lc, boundary;
        bf_layout(win, n, brk, rows, line_max, 12, 24, line_end, &lc, &boundary);
        uint32_t babs = (boundary < n) ? off[boundary] : cpos_end;
        uint32_t expected = (p + 1 < s_page_n) ? s_pages[p + 1] : cpos_end;
        if (babs != expected) {
            fail("[%s] 页%u 边界不一致: 引擎=%u 预期=%u (chars=%d boundary=%d)", name, p, babs, expected, n, boundary);
        }
        if (lc <= 0 || lc > rows) {
            fail("[%s] 页%u 行数异常: %d", name, p, lc);
        }
    }
    printf("  %-28s %8u 字节  %6u 页  OK\n", name, len, s_page_n);
    free(chunk);
    free(win);
    free(brk);
    free(scratch);
    free(off);
}

/* 分步扫描 (阶段限界) 与一次扫完必须产生相同页表 */
static void test_stages(const char *name, const uint8_t *data, uint32_t len,
                        uint8_t enc, int rows, int line_max, uint32_t step) {
    memfile_t mf = { data, len };
    bf_src_t src = { mem_read, &mf };

    uint8_t *chunk = (uint8_t *)malloc(32768);
    bf_ch_t *win = (bf_ch_t *)malloc(sizeof(bf_ch_t) * BF_MAX_WIN);
    uint8_t *brk = (uint8_t *)malloc(BF_MAX_WIN);
    uint8_t *scratch = (uint8_t *)malloc(BF_MAX_WIN * 8 + 8);
    uint32_t *off = (uint32_t *)malloc(BF_MAX_WIN * 4);
    static uint32_t once_pages[200000];
    uint32_t once_n = 0, step_n = 0;

    s_page_n = 0;
    uint32_t next;
    bf_paginate(src, len, enc, 0, rows, line_max, 12, 24,
                add_page_cb, NULL, NULL, NULL,
                chunk, 32768, win, brk, scratch, BF_MAX_WIN * 8 + 8, off,
                len, false, &next);
    once_n = s_page_n;
    memcpy(once_pages, s_pages, once_n * sizeof(uint32_t));

    /* 分步 */
    s_page_n = 0;
    uint32_t cur = 0;
    while (cur < len) {
        uint32_t lim = cur + step;
        if (lim > len) lim = len;
        bf_paginate(src, len, enc, cur, rows, line_max, 12, 24,
                    add_page_cb, NULL, NULL, NULL,
                    chunk, 32768, win, brk, scratch, BF_MAX_WIN * 8 + 8, off,
                    lim, false, &next);
        if (next <= cur) { fail("[%s] 分步无进展: cur=%u next=%u", name, cur, next); break; }
        cur = next;
    }
    step_n = s_page_n;
    if (once_n != step_n) {
        fail("[%s] 分步页数不一致: 一次=%u 分步=%u", name, once_n, step_n);
    } else {
        int bad = 0;
        for (uint32_t i = 0; i < once_n; i++) {
            if (once_pages[i] != s_pages[i]) { bad++; break; }
        }
        if (bad) fail("[%s] 分步页表不一致", name);
        else printf("  %-28s 分步扫描页表与一次扫完一致 (%u 页)  OK\n", name, once_n);
    }
    free(chunk); free(win); free(brk); free(scratch); free(off);
}

/* ---- 样例生成 ---- */

static uint8_t *s_buf;
static size_t s_buf_cap, s_buf_len;

static void buf_init(size_t cap) {
    s_buf = (uint8_t *)malloc(cap);
    s_buf_cap = cap;
    s_buf_len = 0;
}

static void buf_add(const uint8_t *p, size_t n) {
    if (s_buf_len + n > s_buf_cap) return;
    memcpy(s_buf + s_buf_len, p, n);
    s_buf_len += n;
}

static void buf_add_str(const char *s) { buf_add((const uint8_t *)s, strlen(s)); }

/* UTF-8 中文段落 (含避头尾标点) */
static const char *cn_para =
    "第一章 风起\n\n他说：“你好。”然后走了。道路是曲折的，前途是光明的。"
    "每当夜幕降临，他总是想起那些遥远的故事。\n\n"
    "第二章 云涌\n\n风雨交加，雷鸣电闪。他抬起头，望着天际，心中默念："
    "“一切都会好起来的。”\n\n";

/* GBK 编码的同文 (手动编码关键标点/汉字以覆盖 GBK 路径) */
static const uint8_t gbk_sample[] = {
    0xB5,0xDA,0xD2,0xBB,0xD5,0xC2,0x20,0xB7,0xE7,0xC6,0xF0,0x0A,0x0A,   /* 第一章 风起\r\n\r\n */
    0xCB,0xFB,0xCB,0xB5,0xA3,0xBA,0x20,0xB5,0xDA,0x20,0xB5,0xDA,0x20,0x0A,0x0A,
    0xD7,0xDF,0xC1,0xCB,0xA3,0xAC,0x0A,0x0A,                            /* 走了， */
    0xB5,0xDA,0xB6,0xFE,0xD5,0xC2,0x20,0xD4,0xC6,0xD3,0xBF,0x0A,0x0A    /* 第二章 云涌 */
};

static void build_utf8_sample(void) {
    buf_init(64 * 1024);
    for (int i = 0; i < 80; i++) buf_add_str(cn_para);
    /* 长 ASCII 无断点串 */
    for (int i = 0; i < 300; i++) buf_add_str("abcdefghijklmnopqrstuvwxyz");
    buf_add_str("\n\n");
    /* CRLF 混合 */
    for (int i = 0; i < 50; i++) buf_add_str("第一行内容，第二行内容。\r\n");
    /* 截断的 UTF-8 尾 (不完整 3 字节字符) */
    buf_add_str("结尾测试");
    uint8_t tail[] = { 0xE4, 0xB8 };   /* “中”的前两字节 */
    buf_add(tail, sizeof(tail));
}

static void build_utf16_sample(int be) {
    size_t cn = strlen(cn_para);
    buf_init(64 * 1024);
    for (int i = 0; i < cn; i++) {
        uint8_t c = (uint8_t)cn_para[i];
        uint16_t u = c;
        uint8_t b[2];
        if (be) { b[0] = (uint8_t)(u >> 8); b[1] = (uint8_t)u; }
        else    { b[0] = (uint8_t)u; b[1] = (uint8_t)(u >> 8); }
        buf_add(b, 2);
    }
    /* 截断尾: 半个 UTF-16 单元 */
    uint8_t tail[] = { 0x41 };
    buf_add(tail, 1);
}

static void build_gbk_sample(void) {
    buf_init(64 * 1024);
    for (int i = 0; i < 300; i++) buf_add(gbk_sample, sizeof(gbk_sample));
}

static void build_random_sample(uint32_t len) {
    buf_init(len);
    uint32_t x = 12345;
    for (uint32_t i = 0; i < len; i++) {
        x = x * 1664525u + 1013904223u;
        buf_add((const uint8_t *)&x, 1);
    }
}

static void build_big_sample(uint32_t mb) {
    size_t target = (size_t)mb * 1024 * 1024;
    buf_init(target + 4096);
    for (uint32_t i = 0; s_buf_len < target; i++) {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "\xE7\xAC\xAC%d\xE7\xAB\xA0 \xE6\xB5\x8B\xE8\xAF\x95\n\n", i + 1);
        buf_add_str(hdr);
        for (int k = 0; k < 40; k++) buf_add_str(cn_para + 8);   /* 跳过章节行 */
        buf_add_str("\n");
    }
    /* 再塞一个长 ASCII 段 */
    for (int k = 0; k < 5000; k++) buf_add_str("abcdefghij0123456789");
    buf_add_str("\n");
}

int main(void) {
    printf("book_flow 排版内核一致性测试\n");

    build_utf8_sample();
    test_book("UTF-8 中文+ASCII+CRLF+截断", s_buf, (uint32_t)s_buf_len, BF_ENC_UTF8, 10, 384);
    test_book("UTF-8 竖屏布局", s_buf, (uint32_t)s_buf_len, BF_ENC_UTF8, 13, 264);
    test_stages("UTF-8 分步(64KB)", s_buf, (uint32_t)s_buf_len, BF_ENC_UTF8, 10, 384, 65536);

    build_utf16_sample(0);
    test_book("UTF-16LE+截断", s_buf, (uint32_t)s_buf_len, BF_ENC_UTF16LE, 10, 384);
    build_utf16_sample(1);
    test_book("UTF-16BE+截断", s_buf, (uint32_t)s_buf_len, BF_ENC_UTF16BE, 10, 384);

    build_gbk_sample();
    test_book("GBK 纯宽度换行", s_buf, (uint32_t)s_buf_len, BF_ENC_GBK, 10, 384);
    test_book("GBK 竖屏布局", s_buf, (uint32_t)s_buf_len, BF_ENC_GBK, 13, 264);

    build_random_sample(512 * 1024);
    test_book("随机字节 UTF-8 容错", s_buf, (uint32_t)s_buf_len, BF_ENC_UTF8, 10, 384);
    test_book("随机字节 UTF-16LE 容错", s_buf, (uint32_t)s_buf_len, BF_ENC_UTF16LE, 10, 384);
    test_book("随机字节 GBK 容错", s_buf, (uint32_t)s_buf_len, BF_ENC_GBK, 10, 384);

    /* 极小文件 */
    uint8_t tiny1[] = { 0x41 };
    test_book("单字节", tiny1, 1, BF_ENC_UTF8, 10, 384);
    uint8_t tiny2[] = { 0x0A };
    test_book("仅换行", tiny2, 1, BF_ENC_UTF8, 10, 384);
    uint8_t tiny3[] = { 0xE4, 0xB8 };   /* 只有半个 UTF-8 字符 */
    test_book("半个 UTF-8 字符", tiny3, 2, BF_ENC_UTF8, 10, 384);

    /* 20MB 大书性能 + 一致性 */
    build_big_sample(20);
    clock_t t0 = clock();
    test_book("20MB 大书 UTF-8", s_buf, (uint32_t)s_buf_len, BF_ENC_UTF8, 10, 384);
    test_stages("20MB 分步(256KB)", s_buf, (uint32_t)s_buf_len, BF_ENC_UTF8, 10, 384, 262144);
    clock_t t1 = clock();
    printf("  20MB 分页耗时: %.2f 秒\n", (double)(t1 - t0) / CLOCKS_PER_SEC);

    if (g_failures == 0) {
        printf("\n全部通过 ✓\n");
        return 0;
    }
    printf("\n失败 %d 项 ✗\n", g_failures);
    return 1;
}
