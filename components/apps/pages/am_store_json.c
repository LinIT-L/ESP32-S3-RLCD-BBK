/* ============================================================
 * am_store_json.c — 商店 index.json 的解析 (纯 C, 零 OS 依赖)
 * 见 am_store_json.h 说明。**不要**在这里 include os.h / esp_log.h ——
 * 保持"宿主机能直接编译"是这个文件存在的意义。
 * ============================================================ */
#include "am_store_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_skipped = 0;
int am_store_json_skipped(void) { return s_skipped; }

static const char *j_skip(const char *p) {
    while (*p && (unsigned char)*p <= ' ') p++;
    return p;
}
/* p 指向 '{' 或 '[' → 返回配对闭合符之后的位置 (跳过字符串字面量里的括号) */
static const char *j_close(const char *p) {
    if (*p != '{' && *p != '[') return NULL;
    const char open = *p, close = (open == '{') ? '}' : ']';
    int depth = 0; bool ins = false;
    for (; *p; p++) {
        if (ins) {
            if (*p == '\\') { if (p[1]) p++; }
            else if (*p == '"') ins = false;
        } else if (*p == '"') {
            ins = true;
        } else if (*p == open) {
            depth++;
        } else if (*p == close) {
            if (--depth == 0) return p + 1;
        }
    }
    return NULL;
}
/* "key" 之后冒号后的值起点 —— 只在 obj 这个**已截断**的片段里找, 不会串到下一个对象 */
static const char *j_key(const char *obj, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    return p ? p + 1 : NULL;
}
static int j_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* 取字符串值 (解 \" \\ \/ \n \r \t \b \f \uXXXX, 含 UTF-16 代理对) */
static bool j_str(const char *p, char *out, size_t cap) {
    p = j_skip(p);
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            switch (*p) {
            case 'n': c = '\n'; p++; break;
            case 't': c = '\t'; p++; break;
            case 'r': c = '\r'; p++; break;
            case 'b': c = '\b'; p++; break;
            case 'f': c = '\f'; p++; break;
            case '/': c = '/';  p++; break;
            case '"': c = '"';  p++; break;
            case '\\': c = '\\'; p++; break;
            case 'u': {
                if (j_hex((unsigned char)p[1]) < 0 || j_hex((unsigned char)p[2]) < 0 ||
                    j_hex((unsigned char)p[3]) < 0 || j_hex((unsigned char)p[4]) < 0) { p++; continue; }
                unsigned cp = (unsigned)((j_hex((unsigned char)p[1]) << 12) |
                                         (j_hex((unsigned char)p[2]) << 8) |
                                         (j_hex((unsigned char)p[3]) << 4) |
                                          j_hex((unsigned char)p[4]));
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u' &&
                    j_hex((unsigned char)p[2]) >= 0 && j_hex((unsigned char)p[3]) >= 0 &&
                    j_hex((unsigned char)p[4]) >= 0 && j_hex((unsigned char)p[5]) >= 0) {
                    unsigned lo = (unsigned)((j_hex((unsigned char)p[2]) << 12) |
                                             (j_hex((unsigned char)p[3]) << 8) |
                                             (j_hex((unsigned char)p[4]) << 4) |
                                              j_hex((unsigned char)p[5]));
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        p += 6;
                    }
                }
                if (o + 4 < cap) {          /* UTF-8 编码 */
                    if (cp < 0x80) {
                        out[o++] = (char)cp;
                    } else if (cp < 0x800) {
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[o++] = (char)(0xF0 | (cp >> 18));
                        out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                continue;
            }
            default: c = (unsigned char)*p; if (*p) p++; break;
            }
        } else {
            p++;
        }
        if (o + 1 < cap) out[o++] = (char)c;
    }
    out[o] = 0;
    return true;
}
static bool j_num(const char *p, long *v) {
    p = j_skip(p);
    if (*p < '0' || *p > '9') return false;
    *v = strtol(p, NULL, 10);
    return true;
}

int am_store_json_parse(const char *txt,
                        am_rpkg_t *out, int max, int *out_n,
                        char mir[][AM_MIRROR_LEN], int mir_max, int *mir_n) {
    s_skipped = 0;
    if (!txt || !out || !out_n || !mir || !mir_n || max <= 0 || mir_max <= 0) return -4;
    *out_n = 0;
    *mir_n = 0;

    /* ---- mirrors[] ---- */
    const char *m = strstr(txt, "\"mirrors\"");
    if (m) {
        const char *b = strchr(m, '[');
        const char *e = b ? j_close(b) : NULL;
        if (b && e) {
            b++;
            while (b < e && *mir_n < mir_max) {
                b = j_skip(b);
                if (*b != '"') { b++; continue; }
                if (!j_str(b, mir[*mir_n], AM_MIRROR_LEN)) break;
                const char *q = strchr(b + 1, '"');      /* 跳到字符串结束 */
                if (!q) break;
                b = q + 1;
                /* 去掉尾斜杠, 免得拼出 // */
                size_t l = strlen(mir[*mir_n]);
                while (l > 0 && mir[*mir_n][l - 1] == '/') mir[*mir_n][--l] = 0;
                if (l > 0) (*mir_n)++;
            }
        }
    }
    if (*mir_n == 0) return -1;   /* 没源就没法下载 */

    /* ---- pkgs[] ---- */
    const char *p = strstr(txt, "\"pkgs\"");
    if (!p) return -2;
    p = strchr(p, '[');
    if (!p) return -2;
    p++;
    while (*p && *out_n < max) {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;
        const char *e = j_close(p);
        if (!e) break;
        /* 对象截到局部缓冲再按 key 取 —— 免得 j_key 的 strstr 串到下一个对象里去 */
        char ob[512];
        size_t olen = (size_t)(e - p);
        if (olen >= sizeof(ob)) olen = sizeof(ob) - 1;
        memcpy(ob, p, olen);
        ob[olen] = 0;

        am_rpkg_t rk;
        memset(&rk, 0, sizeof(rk));
        bool ok = true;
        const char *v;
        v = j_key(ob, "name");
        if (!v || !j_str(v, rk.name, sizeof(rk.name)) || !rk.name[0]) ok = false;
        v = j_key(ob, "label");
        if (ok && (!v || !j_str(v, rk.label, sizeof(rk.label)))) ok = false;
        v = j_key(ob, "desc");
        if (ok && v) j_str(v, rk.desc, sizeof(rk.desc));    /* 可选: 列表第二行文案 */
        v = j_key(ob, "cat");
        if (ok && (!v || !j_str(v, rk.cat, sizeof(rk.cat)) || !rk.cat[0])) ok = false;
        v = j_key(ob, "path");
        if (ok && (!v || !j_str(v, rk.path, sizeof(rk.path)) || !rk.path[0])) ok = false;
        if (ok) {
            long n = 0;
            v = j_key(ob, "size");
            if (!v || !j_num(v, &n)) ok = false; else rk.size = (uint32_t)n;
        }
        if (ok) {
            char hex[16] = {0};
            v = j_key(ob, "crc32");
            if (v && j_str(v, hex, sizeof(hex))) rk.crc = (uint32_t)strtoul(hex, NULL, 16);
            /* 我们的包 crc32 恰好为 0 的概率 ≈ 0, 拿它当"字段确实解析到了"的判据 */
            if (rk.crc == 0) ok = false;
        }
        if (ok) {
            if (!rk.label[0]) snprintf(rk.label, sizeof(rk.label), "%s", rk.name);
            out[(*out_n)++] = rk;
        } else {
            s_skipped++;
        }
        p = e;
    }
    if (*out_n == 0) return -3;
    return 0;
}
