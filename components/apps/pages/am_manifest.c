/* am_manifest.c — 资源包清单解析 + 路径守卫 (纯 C, 见 am_manifest.h 的格式说明) */
#include "am_manifest.h"
#include <stdlib.h>
#include <string.h>

/* ---------------- 极简 JSON 扫描 (只处理本清单用得到的形状) ---------------- */

static const char *skip_ws(const char *p) {
    while (*p && (unsigned char)*p <= ' ') p++;
    return p;
}

/* p 指向 '{' → 返回配对 '}' 之后的位置 (跳过字符串里的括号) */
static const char *obj_end(const char *p) {
    if (*p != '{') return NULL;
    int depth = 0;
    bool ins = false;
    for (; *p; p++) {
        if (ins) {
            if (*p == '\\') { if (p[1]) p++; }
            else if (*p == '"') ins = false;
        } else if (*p == '"') {
            ins = true;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            if (--depth == 0) return p + 1;
        }
    }
    return NULL;
}

static const char *arr_end(const char *p) {
    if (*p != '[') return NULL;
    int depth = 0;
    bool ins = false;
    for (; *p; p++) {
        if (ins) {
            if (*p == '\\') { if (p[1]) p++; }
            else if (*p == '"') ins = false;
        } else if (*p == '"') {
            ins = true;
        } else if (*p == '[') {
            depth++;
        } else if (*p == ']') {
            if (--depth == 0) return p + 1;
        }
    }
    return NULL;
}

/* 在 obj 片段里找 "key" 后的值起点。obj 必须是**已限定范围**的片段(带 NUL 结尾),
 * 否则 strstr 会串到别的对象里 —— 调用方负责截断。 */
static const char *find_key(const char *obj, const char *key) {
    char pat[48];
    if (strlen(key) + 3 > sizeof(pat)) return NULL;
    pat[0] = '"';
    strcpy(pat + 1, key);
    strcat(pat, "\"");
    const char *p = strstr(obj, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    return p ? p + 1 : NULL;
}

/* 取字符串值 (清单里只有 ASCII 路径和包名, 但仍处理 \\ \" \/ 三种转义) */
static bool get_str(const char *p, char *out, int cap) {
    p = skip_ws(p);
    if (*p != '"') return false;
    p++;
    int o = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\') {
            p++;
            switch (*p) {
            case 'n': c = '\n'; p++; break;
            case 't': c = '\t'; p++; break;
            case 'r': c = '\r'; p++; break;
            case '/': c = '/';  p++; break;
            case '"': c = '"';  p++; break;
            case '\\': c = '\\'; p++; break;
            case 'u':                       /* 路径里不该出现 \uXXXX —— 直接判非法 */
                return false;
            default: c = *p; if (*p) p++; break;
            }
        } else {
            p++;
        }
        if (o + 1 < cap) out[o++] = c;
        else return false;                  /* 超长 = 非法, 不静默截断 */
    }
    if (*p != '"') return false;
    out[o] = 0;
    return true;
}

static bool get_num(const char *p, long *v) {
    p = skip_ws(p);
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    if (*p < '0' || *p > '9') return false;
    long x = strtol(p, NULL, 10);
    *v = neg ? -x : x;
    return true;
}

/* ============================ 路径守卫 ============================ */

bool am_mf_path_ok(const char *rel) {
    if (!rel || !rel[0]) return false;
    size_t n = strlen(rel);
    if (n >= AM_MF_PATH_MAX) return false;
    if (rel[0] == '/') return false;                 /* 绝对路径 */
    if (rel[n - 1] == '/') return false;             /* 目录项不该出现在清单里 */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)rel[i];
        if (c < 0x20 || c == 0x7F) return false;      /* 控制字符 */
        if (c == '\\') return false;                  /* 反斜杠 (Windows 风格的穿越) */
        if (c == ':')  return false;                  /* 盘符 / URL scheme */
    }
    /* 逐段: 空段(//) 与 "."、".." 一律拒收 */
    const char *p = rel;
    while (*p) {
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t seg = (size_t)(p - s);
        if (seg == 0) return false;
        if (seg == 1 && s[0] == '.') return false;
        if (seg == 2 && s[0] == '.' && s[1] == '.') return false;
        if (*p == '/') p++;
    }
    return true;
}

bool am_mf_join(char *dst, int cap, const char *dir, const char *rel) {
    if (!dst || cap <= 0 || !dir || !dir[0]) return false;
    if (!am_mf_path_ok(rel)) return false;
    size_t dl = strlen(dir);
    size_t rl = strlen(rel);
    bool need_slash = (dir[dl - 1] != '/');
    size_t need = dl + (need_slash ? 1 : 0) + rl + 1;
    if (need > (size_t)cap) return false;
    memcpy(dst, dir, dl);
    size_t o = dl;
    if (need_slash) dst[o++] = '/';
    memcpy(dst + o, rel, rl);
    dst[o + rl] = 0;
    return true;
}

/* ============================ 头部解析 ============================ */

int am_mf_head_parse(const char *txt, am_mf_head_t *out) {
    if (!txt || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (!strchr(txt, '{')) return -1;

    long v = 0;
    const char *p;
    p = find_key(txt, "fmt");
    if (!p || !get_num(p, &v)) return -1;
    out->fmt = (int)v;
    p = find_key(txt, "count");
    if (!p || !get_num(p, &v) || v <= 0) return -1;
    out->nfiles = (int)v;
    p = find_key(txt, "total");
    if (!p || !get_num(p, &v) || v <= 0) return -1;
    out->total_bytes = (uint32_t)v;
    p = find_key(txt, "ver");
    if (p && get_num(p, &v) && v > 0) out->ver = (int)v;
    p = find_key(txt, "pkg");
    if (p) get_str(p, out->pkg, (int)sizeof(out->pkg));

    if (out->fmt != 1) return -1;                    /* 只认格式 1 */
    if (out->nfiles > AM_MF_MAX_FILES) return -1;
    return 0;
}

/* ============================ 文件表解析 ============================ */

int am_mf_files_parse(const char *txt, am_mf_file_t *arr, int max) {
    if (!txt || !arr || max <= 0) return -1;
    const char *p = strstr(txt, "\"files\"");
    if (!p) return -2;
    p = strchr(p, '[');
    if (!p) return -2;
    const char *e = arr_end(p);
    if (!e) return -2;
    p++;

    int n = 0;
    while (p < e) {
        while (p < e && *p != '{' && *p != ']') p++;
        if (p >= e || *p != '{') break;
        const char *oe = obj_end(p);
        if (!oe || oe > e) return -3;
        /* 对象截到局部缓冲再按 key 取 —— 免得 find_key 串到相邻条目 */
        char ob[320];
        size_t olen = (size_t)(oe - p);
        if (olen >= sizeof(ob)) return -3;           /* 单条过长 = 畸形 */
        memcpy(ob, p, olen);
        ob[olen] = 0;

        if (n >= max) return -5;
        am_mf_file_t f;
        memset(&f, 0, sizeof(f));

        const char *v = find_key(ob, "path");
        if (!v || !get_str(v, f.path, (int)sizeof(f.path))) return -3;
        if (!am_mf_path_ok(f.path)) return -4;

        long sz = 0;
        v = find_key(ob, "size");
        if (!v || !get_num(v, &sz) || sz <= 0) return -6;
        f.size = (uint32_t)sz;

        char hex[16];
        memset(hex, 0, sizeof(hex));
        v = find_key(ob, "crc32");
        if (v && get_str(v, hex, (int)sizeof(hex))) f.crc32 = (uint32_t)strtoul(hex, NULL, 16);
        if (f.crc32 == 0) return -3;                 /* crc==0 ≈ 字段没读到, 宁可拒收 */

        arr[n++] = f;
        p = oe;
    }
    if (n == 0) return -3;
    return n;
}

/* ============================ 总字节校验 ============================ */

bool am_mf_verify_total(const am_mf_file_t *arr, int n, const am_mf_head_t *h) {
    if (!arr || !h || n <= 0) return false;
    if (n != h->nfiles) return false;
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) sum += arr[i].size;
    return sum == (uint64_t)h->total_bytes;
}

/* ============================ URL 编码 ============================ */

void am_mf_url_encode(const char *in, char *out, int cap) {
    if (!out || cap <= 0) return;
    int o = 0;
    if (!in) { out[0] = 0; return; }
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = "0123456789ABCDEF"[c >> 4];
            out[o++] = "0123456789ABCDEF"[c & 0x0F];
        }
    }
    out[o] = 0;
}
