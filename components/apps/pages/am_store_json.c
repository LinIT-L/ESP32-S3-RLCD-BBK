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
        /* 对象截到局部缓冲再按 key 取 —— 免得 j_key 的 strstr 串到下一个对象里去。
         * ★ 2026-10-31: 512 → 768。条目字段一直在加(name/label/desc/type/target/
         *   requires/manifest/cat/path/size/crc32/ver/author/date/build), 原尺寸离
         *   截断只差几十字节; 一旦截断, 排在后面的 key 直接读不到 —— 表现为
         *   "某个包莫名其妙少了一条", 极难查。 */
        char ob[1280];
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
        v = j_key(ob, "type");
        if (v) j_str(v, rk.type, sizeof(rk.type));          /* 可选: app / asset */
        /* ★ 2026-10-31: 显式依赖 "pkgs/x.zip[@ver]" —— 放在 desc 之后、cat 之前解析:
         *   ob[] 只有 512 字节, 超长条目会被截断, 靠前的字段才是"一定读得到"的。 */
        v = j_key(ob, "requires");
        if (v) j_str(v, rk.requires, sizeof(rk.requires));  /* 可选: 依赖的资源包 */
        v = j_key(ob, "target");
        if (v) j_str(v, rk.target, sizeof(rk.target));      /* 可选: asset 解包目录 */
        v = j_key(ob, "cat");
        if (ok && (!v || !j_str(v, rk.cat, sizeof(rk.cat)) || !rk.cat[0])) ok = false;
        v = j_key(ob, "path");
        if (ok && (!v || !j_str(v, rk.path, sizeof(rk.path)) || !rk.path[0])) ok = false;
        /* 可选: 资源包文件清单 (多文件直接下载用); 缺失 = 走原来的 zip 路径 */
        if (ok) {
            v = j_key(ob, "manifest");
            if (v) j_str(v, rk.manifest, sizeof(rk.manifest));
        }
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
            /* ★ 2026-11-01: SHA-256（可选；新 LPK 必须提供，缺失按旧协议处理） */
            v = j_key(ob, "sha256");
            if (v) j_str(v, rk.sha256, sizeof(rk.sha256));
        }
        if (ok) {
            /* ★ 2026-10-27: 读版本号 (可选字段, 缺失 = 0 = 未知版本, 不参与"更新"判断) */
            long vv = 0;
            v = j_key(ob, "ver");
            if (v && j_num(v, &vv) && vv > 0) rk.ver = (uint32_t)vv;
            /* ★ 2026-10-27 linit "统一添加更详细信息": 作者 / 日期。
             *   资源包(zip)没有 LAPP 头，它们的这两个字段**只在这里有**。 */
            v = j_key(ob, "author");
            if (v) j_str(v, rk.author, sizeof(rk.author));
            v = j_key(ob, "date");
            if (v) j_str(v, rk.date, sizeof(rk.date));
            /* ★ 2026-10-30: 构建时间 (仅显示/诊断; 2026-11-02 起不再参与更新判定) */
            v = j_key(ob, "build");
            if (v) j_str(v, rk.build, sizeof(rk.build));
            /* ★ 2026-11-02: 服务器文件的实际修改时间 —— **更新判定的唯一依据**。
             *   缺失(老索引) = 0 ⇒ 设备端退回 size+CRC32 核对，不造"更新"。 */
            long mt = 0;
            v = j_key(ob, "mtime");
            if (v && j_num(v, &mt) && mt > 0) rk.mtime = (uint32_t)mt;
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
