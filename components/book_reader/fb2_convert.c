/* FB2 流式 XML 解析 (无 XML 库依赖):
 * - 跳过 <description> 元数据 (仅抓取 book-title)
 * - <body> 内: <title> 行 = 章节标题, <p> 行 = 段落, <empty-line/> = 空行
 * - 实体解码 (&amp; &lt; &gt; &quot; &apos; &#N; &#xH;), CDATA 原文
 */
#include "fb2_convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef struct {
    FILE *out;
    char  title[96];
    size_t title_len;
    /* 状态 */
    bool in_body;
    bool in_title;
    bool in_p;
    bool in_comment;
    bool in_cdata;
    int  section_depth;
    bool in_book_title;
    /* 当前行缓冲 (标题或段落文本) */
    char line[2048];
    size_t line_len;
    bool line_ws;      /* 上一个字符是空白 (折叠用) */
} fb2_ctx_t;

static void line_flush(fb2_ctx_t *c, bool blank_after) {
    /* 去掉尾部空白 */
    while (c->line_len > 0 && (c->line[c->line_len - 1] == ' ' ||
                              c->line[c->line_len - 1] == '\t')) {
        c->line_len--;
    }
    if (c->line_len > 0) {
        fwrite(c->line, 1, c->line_len, c->out);
    }
    if (blank_after) {
        fputc('\n', c->out);
        fputc('\n', c->out);
    } else {
        fputc('\n', c->out);
    }
    c->line_len = 0;
    c->line_ws = false;
}

static void line_text(fb2_ctx_t *c, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (c->line_len > 0 && !c->line_ws && c->line_len < sizeof(c->line) - 1) {
                c->line[c->line_len++] = ' ';
                c->line_ws = true;
            }
        } else {
            if (c->line_len < sizeof(c->line) - 1) {
                c->line[c->line_len++] = ch;
                c->line_ws = false;
            }
        }
    }
}

static void decode_entity(fb2_ctx_t *c, const char *e, size_t n) {
    char buf[8];
    if (n == 5 && memcmp(e, "&amp;", 5) == 0) { buf[0] = '&'; line_text(c, buf, 1); }
    else if (n == 4 && memcmp(e, "&lt;", 4) == 0) { buf[0] = '<'; line_text(c, buf, 1); }
    else if (n == 4 && memcmp(e, "&gt;", 4) == 0) { buf[0] = '>'; line_text(c, buf, 1); }
    else if (n == 6 && memcmp(e, "&quot;", 6) == 0) { buf[0] = '"'; line_text(c, buf, 1); }
    else if (n == 6 && memcmp(e, "&apos;", 6) == 0) { buf[0] = '\''; line_text(c, buf, 1); }
    else if (n > 3 && e[1] == '#') {
        long v = 0;
        int i = 2;
        if (e[i] == 'x' || e[i] == 'X') {
            i++;
            for (; i + 1 < (int)n; i++) {
                char ch = e[i];
                v = v * 16 + (ch >= '0' && ch <= '9' ? ch - '0' :
                              ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
                              ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1);
                if (v < 0) break;
            }
        } else {
            for (; i + 1 < (int)n; i++) {
                if (e[i] < '0' || e[i] > '9') break;
                v = v * 10 + (e[i] - '0');
            }
        }
        if (v > 0 && v < 0x110000) {
            /* UTF-8 编码 */
            if (v < 0x80) { buf[0] = (char)v; line_text(c, buf, 1); }
            else if (v < 0x800) {
                buf[0] = (char)(0xC0 | (v >> 6));
                buf[1] = (char)(0x80 | (v & 0x3F));
                line_text(c, buf, 2);
            } else if (v < 0x10000) {
                buf[0] = (char)(0xE0 | (v >> 12));
                buf[1] = (char)(0x80 | ((v >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (v & 0x3F));
                line_text(c, buf, 3);
            } else {
                buf[0] = (char)(0xF0 | (v >> 18));
                buf[1] = (char)(0x80 | ((v >> 12) & 0x3F));
                buf[2] = (char)(0x80 | ((v >> 6) & 0x3F));
                buf[3] = (char)(0x80 | (v & 0x3F));
                line_text(c, buf, 4);
            }
        }
    } else {
        line_text(c, e, n);
    }
}

/* tag 处理: name 为小写标签名, closing=是否结束标签, self_close=是否自闭合 */
static void handle_tag(fb2_ctx_t *c, const char *name, int nlen, bool closing, bool self_close) {
    char tag[32];
    if (nlen > 31) nlen = 31;
    for (int i = 0; i < nlen; i++) {
        char ch = name[i];
        tag[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    tag[nlen] = 0;

    if (strcmp(tag, "book-title") == 0) {
        c->in_book_title = !closing;
        return;
    }
    if (strcmp(tag, "body") == 0) {
        if (!closing) c->in_body = true;
        else c->in_body = false;
        return;
    }
    if (!c->in_body) return;   /* description 等内容一律跳过 */

    if (strcmp(tag, "title") == 0) {
        c->in_title = !closing;
        if (!closing && c->line_len == 0) c->line_ws = false;
        if (closing && !self_close) {
            line_flush(c, true);
        }
        return;
    }
    if (strcmp(tag, "p") == 0) {
        c->in_p = !closing;
        if (!closing && c->line_len == 0) c->line_ws = false;
        if (closing && !self_close) {
            line_flush(c, true);
        }
        return;
    }
    if (strcmp(tag, "empty-line") == 0) {
        fputc('\n', c->out);
        return;
    }
    if (strcmp(tag, "section") == 0) {
        if (!closing) c->section_depth++;
        else if (c->section_depth > 0) {
            c->section_depth--;
            fputc('\n', c->out);
        }
        return;
    }
    /* 其他标签: 忽略 */
}

int fb2_convert(const char *src, const char *dst, char *title, size_t title_cap) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    fb2_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.out = out;

    static uint8_t buf[4096];
    size_t n;
    int st = 0;        /* 0=普通文本 1=在标签内 2=注释 3=CDATA */
    char tag[64];
    size_t tag_len = 0;
    bool tag_closing = false;

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        for (size_t i = 0; i < n; i++) {
            char ch = (char)buf[i];
            if (st == 2) {           /* 注释 */
                if (ch == '-' && i + 2 < n && buf[i + 1] == '-' && buf[i + 2] == '>') {
                    st = 0;
                    i += 2;
                }
                continue;
            }
            if (st == 3) {           /* CDATA */
                if (ch == ']' && i + 2 < n && buf[i + 1] == ']' && buf[i + 2] == '>') {
                    st = 0;
                    i += 2;
                } else if (c.in_title || c.in_p) {
                    char one[2] = { ch, 0 };
                    line_text(&c, one, 1);
                }
                continue;
            }
            if (st == 1) {           /* 标签内 */
                if (ch == '>') {
                    /* 自闭合判断 */
                    bool self_close = (tag_len > 0 && tag[tag_len - 1] == '/');
                    if (self_close) tag_len--;
                    handle_tag(&c, tag, (int)tag_len, tag_closing, self_close);
                    tag_len = 0;
                    tag_closing = false;
                    st = 0;
                    continue;
                }
                if (tag_len < sizeof(tag) - 1) tag[tag_len++] = ch;
                continue;
            }
            /* 普通文本 */
            if (ch == '<') {
                /* 识别 !-- 注释 和 ![CDATA[ */
                if (i + 3 < n && buf[i + 1] == '!' && buf[i + 2] == '-' && buf[i + 3] == '-') {
                    st = 2;
                    i += 3;
                    continue;
                }
                if (i + 8 < n && memcmp(buf + i + 1, "![CDATA[", 8) == 0) {
                    st = 3;
                    i += 8;
                    continue;
                }
                st = 1;
                tag_len = 0;
                tag_closing = false;
                if (i + 1 < n && buf[i + 1] == '/') {
                    tag_closing = true;
                    i++;
                }
                continue;
            }
            if (ch == '&') {
                /* 收集实体 */
                size_t j = i;
                while (j < n && j < i + 10 && buf[j] != ';') j++;
                if (j < n && buf[j] == ';') {
                    size_t elen = j - i + 1;
                    if (c.in_title || c.in_p) {
                        static char entbuf[16];
                        if (elen < sizeof(entbuf)) {
                            memcpy(entbuf, buf + i, elen);
                            entbuf[elen] = 0;
                            decode_entity(&c, entbuf, elen);
                        }
                    }
                    i = j;
                    continue;
                }
            }
            if (c.in_title || c.in_p) {
                char one[2] = { ch, 0 };
                line_text(&c, one, 1);
            }
            /* 书名提取: 仅 <book-title> 内文本 */
            if (c.in_book_title && c.title_len < sizeof(c.title) - 1 &&
                ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') {
                c.title[c.title_len++] = ch;
            }
        }
    }

    /* 收尾: 未闭合的行 */
    if (c.line_len > 0) line_flush(&c, false);
    if (c.title_len > 0 && title && title_cap) {
        if (c.title_len > title_cap - 1) c.title_len = title_cap - 1;
        memcpy(title, c.title, c.title_len);
        title[c.title_len] = 0;
    }

    fclose(out);
    fclose(in);
    return 0;
}
