/* EPUB -> TXT 转换器 (miniz 解压, 无其他依赖)
 * 参考: Porfiry-Petrovich 项目 EPUB 章节拆分思路 + ESP32-S3 ePub Reader 的 zip 方案
 */
#include "epub_convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "miniz.h"

#define EP_MAX_ENTRY (4u * 1024 * 1024)   /* 单章上限 4MB */

/* ---------- 通用工具 ---------- */

static void *zip_read(mz_zip_archive *zip, const char *name, size_t *size) {
    int idx = mz_zip_reader_locate_file(zip, name, NULL, 0);
    if (idx < 0) return NULL;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, (mz_uint)idx, &st)) return NULL;
    if (st.m_uncomp_size == 0 || st.m_uncomp_size > EP_MAX_ENTRY) return NULL;
    size_t sz = 0;
    void *p = mz_zip_reader_extract_to_heap(zip, (mz_uint)idx, &sz, 0);
    if (!p) return NULL;
    if (size) *size = sz;
    return p;
}

/* 在 XML 中找 name="..." 属性值 */
static bool attr_value(const char *xml, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    const char *p = xml;
    while ((p = strstr(p, name)) != NULL) {
        char prev = (p == xml) ? ' ' : p[-1];
        if (prev == ' ' || prev == '\t' || prev == '\n' || prev == '<') {
            p += nl;
            while (*p == ' ' || *p == '\t' || *p == '\n') p++;
            if (*p == '=') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    size_t n = 0;
                    while (*p && *p != q && n + 1 < cap) out[n++] = *p++;
                    out[n] = 0;
                    return n > 0;
                }
            }
        }
        p += nl;
    }
    return false;
}

/* 提取 <tag>...</tag> 文本 (用于 dc:title) */
static bool tag_text(const char *xml, const char *tag, char *out, size_t cap) {
    char open[64], close[64];
    snprintf(open, sizeof(open), "<%s", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *s = strstr(xml, open);
    if (!s) return false;
    s = strchr(s, '>');
    if (!s) return false;
    s++;
    const char *e = strstr(s, close);
    if (!e) return false;
    size_t n = (size_t)(e - s);
    if (n > cap - 1) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
    return n > 0;
}

/* ---------- HTML 文本提取 (流式) ---------- */

typedef struct {
    FILE *out;
    char line[2048];
    size_t line_len;
    bool line_ws;
    bool have_para;       /* 是否已输出过段落 (控制首部空行) */
    bool in_skip_tag;     /* script/style/head 内 */
    char skip_tag[16];
    size_t skip_len;
    bool in_title_tag;
    char title[96];
    size_t title_len;
    bool title_done;
} html_ctx_t;

static void hline_flush(html_ctx_t *c, bool blank) {
    while (c->line_len > 0 && (c->line[c->line_len - 1] == ' ' ||
                              c->line[c->line_len - 1] == '\t')) c->line_len--;
    if (c->line_len > 0) {
        if (c->have_para) fputc('\n', c->out);
        fwrite(c->line, 1, c->line_len, c->out);
        fputc('\n', c->out);
        if (blank) fputc('\n', c->out);
        c->have_para = true;
    }
    c->line_len = 0;
    c->line_ws = false;
}

static void hline_text(html_ctx_t *c, const char *s, size_t n) {
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

static void hdecode(html_ctx_t *c, const char *e, size_t n) {
    char buf[8];
    static const struct { const char *name; uint32_t cp; } named[] = {
        { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' },
        { "apos;", '\'' }, { "nbsp;", 0xA0 }, { "mdash;", 0x2014 },
        { "ndash;", 0x2013 }, { "hellip;", 0x2026 }, { "ldquo;", 0x201C },
        { "rdquo;", 0x201D }, { "lsquo;", 0x2018 }, { "rsquo;", 0x2019 },
        { "laquo;", 0x00AB }, { "raquo;", 0x00BB }, { "middot;", 0x00B7 },
        { "copy;", 0x00A9 }, { "reg;", 0x00AE }, { "eacute;", 0x00E9 },
        { "egrave;", 0x00E8 }, { "agrave;", 0x00E0 }, { "ccedil;", 0x00E7 },
        { "ouml;", 0x00F6 }, { "uuml;", 0x00FC }, { "auml;", 0x00E4 },
        { "szlig;", 0x00DF }, { "times;", 0x00D7 }, { "divide;", 0x00F7 },
        { "deg;", 0x00B0 }, { "plusmn;", 0x00B1 }, { "frac12;", 0x00BD },
        { "sup2;", 0x00B2 }, { "sup3;", 0x00B3 }, { "para;", 0x00B6 },
        { "sect;", 0x00A7 }, { "bull;", 0x2022 }, { "trade;", 0x2122 },
        { "euro;", 0x20AC }, { "yen;", 0x00A5 }, { "pound;", 0x00A3 },
    };
    if (n > 3 && e[1] == '#') {
        long v = 0;
        int i = 2;
        if (e[i] == 'x' || e[i] == 'X') {
            i++;
            for (; i + 1 < (int)n; i++) {
                char ch = e[i];
                int d = (ch >= '0' && ch <= '9') ? ch - '0' :
                        (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 :
                        (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
                if (d < 0) break;
                v = v * 16 + d;
            }
        } else {
            for (; i + 1 < (int)n; i++) {
                if (e[i] < '0' || e[i] > '9') break;
                v = v * 10 + (e[i] - '0');
            }
        }
        if (v > 0 && v < 0x110000) {
            if (v < 0x80) { buf[0] = (char)v; hline_text(c, buf, 1); }
            else if (v < 0x800) {
                buf[0] = (char)(0xC0 | (v >> 6));
                buf[1] = (char)(0x80 | (v & 0x3F));
                hline_text(c, buf, 2);
            } else if (v < 0x10000) {
                buf[0] = (char)(0xE0 | (v >> 12));
                buf[1] = (char)(0x80 | ((v >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (v & 0x3F));
                hline_text(c, buf, 3);
            } else {
                buf[0] = (char)(0xF0 | (v >> 18));
                buf[1] = (char)(0x80 | ((v >> 12) & 0x3F));
                buf[2] = (char)(0x80 | ((v >> 6) & 0x3F));
                buf[3] = (char)(0x80 | (v & 0x3F));
                hline_text(c, buf, 4);
            }
        }
        return;
    }
    for (size_t k = 0; k < sizeof(named) / sizeof(named[0]); k++) {
        size_t nl = strlen(named[k].name) + 1;   /* 含 & */
        if (n == nl && memcmp(e, "&", 1) == 0 && memcmp(e + 1, named[k].name, nl - 1) == 0) {
            uint32_t v = named[k].cp;
            if (v < 0x80) { buf[0] = (char)v; hline_text(c, buf, 1); }
            else if (v < 0x800) {
                buf[0] = (char)(0xC0 | (v >> 6));
                buf[1] = (char)(0x80 | (v & 0x3F));
                hline_text(c, buf, 2);
            } else {
                buf[0] = (char)(0xE0 | (v >> 12));
                buf[1] = (char)(0x80 | ((v >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (v & 0x3F));
                hline_text(c, buf, 3);
            }
            return;
        }
    }
    hline_text(c, e, n);
}

/* 把 HTML 文本流式写入 out. 返回 0 成功 */
static int html_to_text(const char *html, size_t len, FILE *out,
                        char *title, size_t title_cap) {
    html_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.out = out;
    int st = 0;            /* 0=文本 1=标签 2=注释 */
    char tag[64];
    size_t tag_len = 0;
    bool tag_closing = false;
    static const char *block_tags[] = {
        "p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6",
        "li", "tr", "td", "blockquote", "section", "article", "table", "pre"
    };
    static const char *skip_tags[] = { "script", "style", "head", "title", "svg" };

    for (size_t i = 0; i < len; i++) {
        char ch = html[i];
        if (st == 2) {
            if (ch == '-' && i + 2 < len && html[i + 1] == '-' && html[i + 2] == '>') {
                st = 0;
                i += 2;
            }
            continue;
        }
        if (st == 1) {
            if (ch == '>') {
                bool self_close = (tag_len > 0 && tag[tag_len - 1] == '/');
                if (self_close) tag_len--;
                /* 标签名 (去掉属性) */
                char name[64];
                size_t nl = 0;
                for (size_t k = 0; k < tag_len && nl < sizeof(name) - 1; k++) {
                    char tc = tag[k];
                    if (tc == ' ' || tc == '\t' || tc == '\n' || tc == '/') break;
                    name[nl++] = (tc >= 'A' && tc <= 'Z') ? (char)(tc - 'A' + 'a') : tc;
                }
                name[nl] = 0;
                if (nl == 0) { st = 0; continue; }
                if (!tag_closing && !self_close) {
                    for (int si = 0; si < 5; si++) {
                        if (strcmp(name, skip_tags[si]) == 0) {
                            c.in_skip_tag = true;
                            snprintf(c.skip_tag, sizeof(c.skip_tag), "%.15s", name);
                            break;
                        }
                    }
                    if (strcmp(name, "h1") == 0 || strcmp(name, "h2") == 0 ||
                        strcmp(name, "h3") == 0) {
                        c.in_title_tag = true;
                    }
                    for (int bi = 0; bi < 17; bi++) {
                        if (strcmp(name, block_tags[bi]) == 0) {
                            hline_flush(&c, false);
                            break;
                        }
                    }
                } else if (tag_closing) {
                    if (c.in_skip_tag && strcmp(name, c.skip_tag) == 0) {
                        c.in_skip_tag = false;
                        c.skip_tag[0] = 0;
                    }
                    if (c.in_title_tag && (strcmp(name, "h1") == 0 ||
                                           strcmp(name, "h2") == 0 ||
                                           strcmp(name, "h3") == 0)) {
                        if (!c.title_done) {
                            while (c.line_len > 0 && c.line[c.line_len - 1] == ' ') c.line_len--;
                            size_t n = c.line_len;
                            if (n > title_cap - 1) n = title_cap - 1;
                            if (title && n > 0) {
                                memcpy(title, c.line, n);
                                title[n] = 0;
                                c.title_done = true;
                            }
                            hline_flush(&c, true);
                        } else {
                            hline_flush(&c, true);
                        }
                        c.in_title_tag = false;
                    }
                    if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
                        strcmp(name, "li") == 0 || strcmp(name, "blockquote") == 0 ||
                        strcmp(name, "section") == 0 || strcmp(name, "pre") == 0 ||
                        strcmp(name, "tr") == 0 || strcmp(name, "td") == 0) {
                        hline_flush(&c, true);
                    }
                }
                tag_len = 0;
                tag_closing = false;
                st = 0;
                continue;
            }
            if (tag_len < sizeof(tag) - 1) tag[tag_len++] = ch;
            continue;
        }
        if (ch == '<') {
            if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
                st = 2;
                i += 3;
                continue;
            }
            st = 1;
            tag_len = 0;
            tag_closing = false;
            if (i + 1 < len && html[i + 1] == '/') {
                tag_closing = true;
                i++;
            }
            continue;
        }
        if (c.in_skip_tag) continue;
        if (ch == '&') {
            size_t j = i;
            while (j < len && j < i + 12 && html[j] != ';') j++;
            if (j < len && html[j] == ';') {
                size_t elen = j - i + 1;
                static char ent[16];
                if (elen < sizeof(ent)) {
                    memcpy(ent, html + i, elen);
                    ent[elen] = 0;
                    hdecode(&c, ent, elen);
                }
                i = j;
                continue;
            }
        }
        char one[2] = { ch, 0 };
        hline_text(&c, one, 1);
    }
    hline_flush(&c, false);
    return 0;
}

/* ---------- EPUB 主流程 ---------- */

int epub_convert(const char *src, const char *dst, char *title, size_t title_cap) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, src, 0)) return -1;

    /* 1. container.xml -> OPF 路径 */
    char opf[256] = {0};
    {
        size_t sz = 0;
        char *cxml = (char *)zip_read(&zip, "META-INF/container.xml", &sz);
        if (cxml) {
            attr_value(cxml, "full-path", opf, sizeof(opf));
            free(cxml);
        }
    }
    if (!opf[0]) { mz_zip_reader_end(&zip); return -1; }

    /* 2. OPF -> 标题 + spine */
    size_t osz = 0;
    char *opf_xml = (char *)zip_read(&zip, opf, &osz);
    if (!opf_xml) { mz_zip_reader_end(&zip); return -1; }
    char bt[96] = {0};
    tag_text(opf_xml, "dc:title", bt, sizeof(bt));
    if (!bt[0]) tag_text(opf_xml, "title", bt, sizeof(bt));

    FILE *out = fopen(dst, "wb");
    if (!out) { free(opf_xml); mz_zip_reader_end(&zip); return -1; }

    /* 3. 按 spine 顺序输出各章 */
    char opf_dir[192] = {0};
    const char *slash = strrchr(opf, '/');
    if (slash) {
        size_t dl = (size_t)(slash - opf);
        if (dl > sizeof(opf_dir) - 1) dl = sizeof(opf_dir) - 1;
        memcpy(opf_dir, opf, dl);
        opf_dir[dl] = 0;
    }

    const char *itemref = opf_xml;
    int docs = 0;
    while ((itemref = strstr(itemref, "itemref")) != NULL) {
        char idref[128] = {0};
        if (!attr_value(itemref, "idref", idref, sizeof(idref))) {
            itemref += 7;
            continue;
        }
        /* 在 manifest 里找 id=idref 的 href */
        char href[256] = {0};
        const char *mi = opf_xml;
        while ((mi = strstr(mi, "<item")) != NULL) {
            char mid[128] = {0};
            if (attr_value(mi, "id", mid, sizeof(mid)) && strcmp(mid, idref) == 0) {
                char mt[64] = {0};
                attr_value(mi, "media-type", mt, sizeof(mt));
                if (!mt[0] || strstr(mt, "xhtml") || strstr(mt, "html") || strstr(mt, "xml")) {
                    attr_value(mi, "href", href, sizeof(href));
                }
                break;
            }
            mi += 5;
        }
        if (href[0]) {
            /* 解析完整路径 */
            char full[512];
            if (href[0] == '/') {
                snprintf(full, sizeof(full), "%s", href + 1);
            } else if (opf_dir[0]) {
                snprintf(full, sizeof(full), "%s/%s", opf_dir, href);
            } else {
                snprintf(full, sizeof(full), "%s", href);
            }
            /* 规范化 ./ */
            size_t sz = 0;
            char *x = (char *)zip_read(&zip, full, &sz);
            if (!x) {
                /* 尝试去掉 ./ 前缀 */
                const char *clean = strstr(full, "./");
                if (clean) x = (char *)zip_read(&zip, clean + 2, &sz);
            }
            if (x) {
                char dt[96] = {0};
                html_to_text(x, sz, out, dt, sizeof(dt));
                if (docs == 0 && dt[0] && title && title_cap) {
                    size_t n = strlen(dt);
                    if (n > title_cap - 1) n = title_cap - 1;
                    memcpy(title, dt, n);
                    title[n] = 0;
                }
                free(x);
                docs++;
            }
        }
        itemref += 7;
    }
    free(opf_xml);
    fclose(out);
    mz_zip_reader_end(&zip);
    if (docs == 0) return -1;
    return 0;
}
