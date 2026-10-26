/* wiki_db.c — 离线维基数据服务实现
 *
 * 数据格式（PC 端 nomad-wiki/tools/build_wiki_db.py 生成）:
 *   index.bin         80B/条：title[64] chunk:u32 off:u32 clen:u32 ulen:u32
 *   articles_%04d.dat 每篇：'WK1\0' ulen:u32 clen:u32 crc:u32 + lz4 block
 *   metadata.json     只需 articleCount 与 chunks.pattern
 *
 * 一条读取链（不带任何缓存层，每次 load 重新走）：
 *   index.bin@id*80 → chunk 文件 @off → 16B 帧头 → clen 字节 → LZ4 → 原地剥 HTML → 折行
 */
#include "wiki_db.h"
#include "lz4_dec.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "wiki";

#define IDX_REC       80
#define FRAME_HDR     16
#define FRAME_MAGIC0  'W'
#define LINE_W        384        /* 400 宽 - 左右各 8 留白 */
#define LINE_SOFT     368        /* 折行软上限：留一个全角位，标点才能悬挂到行尾 */
#define DIR_ROOT      "/sdcard/wiki"
#define DIR_CAP       256        /* 库目录路径（留足，文件名还要往上拼） */
#define PATH_CAP      320        /* 拼好文件名的完整路径 */
#define META_CAP      (16 * 1024)

/* 一行：文本缓冲里的 (起点, 字节长度)。
 * ⚠ 不用就地 '\0' 截断 —— 恰好填满一行时行尾位置就是下一行的起点，
 *   把那里写 0 会把下一行首字节清掉（表现为每隔几行冒出一堆空行）。 */
typedef struct {
    uint32_t off;
    uint16_t len;
} wiki_ln_t;

static struct {
    bool  open;
    char  dir[DIR_CAP];
    char  name[64];
    int   count;

    FILE      *idx;              /* index.bin 常驻句柄：二分/取标题不再每次 open */
    char      *text;             /* PSRAM: 当前文章纯文本 */
    wiki_ln_t *line;             /* PSRAM: 每行 (起点, 长度) */
    int        nlines;
    int        lcap;
} S;

/* ------------------------------------------------------------------ 小工具 */

static void *ps_malloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

static void ps_free(void *p) {
    if (p) heap_caps_free(p);
}

/* 直接把整个小文件读进来（只用于 metadata.json，上限 META_CAP） */
static int read_small(const char *path, char *buf, int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(buf, 1, (size_t)cap - 1, f);
    fclose(f);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

/* metadata.json 里取整数：找 "key" 后第一个数字 */
static int json_int(const char *js, const char *key, int def) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p < '0' || *p > '9') return def;
    return atoi(p);
}

/* 读 index.bin 的一条记录。成功返回 true */
static bool read_rec(int id, char title[WIKI_TITLE_MAX], uint32_t *chunk,
                     uint32_t *off, uint32_t *clen, uint32_t *ulen) {
    if (id < 0 || id >= S.count || !S.idx) return false;
    bool ok = false;
    if (fseek(S.idx, (long)id * IDX_REC, SEEK_SET) == 0) {
        uint8_t b[IDX_REC];
        if (fread(b, 1, IDX_REC, S.idx) == IDX_REC) {
            if (title) { memcpy(title, b, WIKI_TITLE_MAX); title[WIKI_TITLE_MAX - 1] = 0; }
            memcpy(chunk, b + 64, 4);
            memcpy(off,   b + 68, 4);
            memcpy(clen,  b + 72, 4);
            memcpy(ulen,  b + 76, 4);
            ok = true;
        }
    }
    return ok;
}

/* ------------------------------------------------------------------ 打开/关闭 */

static bool dir_has_db(const char *dir) {
    char p[PATH_CAP];
    /* 显式限长：调用方传进来的目录串可能已经是满缓冲，用 %.*s 把上限说清楚 */
    snprintf(p, sizeof(p), "%.*s/metadata.json", (int)(sizeof(p) - 15), dir);
    return access(p, F_OK) == 0;
}

int wiki_open(const char *dir) {
    wiki_close();

    if (dir && dir[0]) {
        snprintf(S.dir, sizeof(S.dir), "%.*s", (int)(sizeof(S.dir) - 1), dir);
    } else {
        /* 自动挑 /sdcard/wiki 下第一个有 metadata.json 的子目录 */
        DIR *d = opendir(DIR_ROOT);
        if (!d) {
            ESP_LOGW(TAG, "没有 %s 目录", DIR_ROOT);
            return 0;
        }
        struct dirent *e;
        bool found = false;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char cand[PATH_CAP];
            snprintf(cand, sizeof(cand), "%s/%s", DIR_ROOT, e->d_name);
            if (dir_has_db(cand)) {
                snprintf(S.dir, sizeof(S.dir), "%.*s", (int)(sizeof(S.dir) - 1), cand);
                found = true;
                break;
            }
        }
        closedir(d);
        if (!found) {
            ESP_LOGW(TAG, "%s 下没有可用的库", DIR_ROOT);
            return 0;
        }
    }

    char meta[META_CAP];
    char mp[PATH_CAP];
    snprintf(mp, sizeof(mp), "%s/metadata.json", S.dir);
    if (read_small(mp, meta, META_CAP) <= 0) {
        ESP_LOGE(TAG, "读不到 %s", mp);
        return 0;
    }

    int ver = json_int(meta, "version", -1);
    int n   = json_int(meta, "articleCount", 0);
    if (n <= 0) { ESP_LOGE(TAG, "metadata 里没有 articleCount"); return 0; }
    if (ver != 2) { ESP_LOGE(TAG, "格式版本 %d 不支持（要 2）", ver); return 0; }

    /* 索引大小必须严格等于 count*80，否则就地拒读，别带着坏索引去二分 */
    char ip[PATH_CAP];
    snprintf(ip, sizeof(ip), "%s/index.bin", S.dir);
    FILE *f = fopen(ip, "rb");
    if (!f) { ESP_LOGE(TAG, "读不到 %s", ip); return 0; }
    fseek(f, 0, SEEK_END);
    long isz = ftell(f);
    fclose(f);
    if (isz != (long)n * IDX_REC) {
        ESP_LOGE(TAG, "index.bin %ld 字节 != %d×80（库不完整）", isz, n);
        return 0;
    }

    S.idx = fopen(ip, "rb");
    if (!S.idx) { ESP_LOGE(TAG, "index.bin 打不开"); return 0; }

    const char *slash = strrchr(S.dir, '/');
    snprintf(S.name, sizeof(S.name), "%.*s", (int)(sizeof(S.name) - 1), slash ? slash + 1 : S.dir);
    S.count = n;
    S.open  = true;
    ESP_LOGI(TAG, "打开 %s：%d 篇", S.dir, n);
    return n;
}

void wiki_close(void) {
    if (S.idx) { fclose(S.idx); S.idx = NULL; }
    ps_free(S.text); S.text = NULL;
    ps_free(S.line); S.line = NULL;
    S.nlines = S.lcap = 0;
    S.open = false;
    S.count = 0;
    S.dir[0] = S.name[0] = 0;
}

bool wiki_is_open(void) { return S.open && S.count > 0; }
int  wiki_count(void)   { return S.count; }
const char *wiki_name(void) { return S.name; }

/* ------------------------------------------------------------------ 查询 */

static int lower_bound(const char *q, size_t qlen) {
    int lo = 0, hi = S.count - 1, ans = S.count;
    char t[WIKI_TITLE_MAX];
    uint32_t c, o, cl, ul;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (!read_rec(mid, t, &c, &o, &cl, &ul)) return ans;
        size_t tl = strnlen(t, WIKI_TITLE_MAX);
        size_t m = qlen < tl ? qlen : tl;
        int cmp = memcmp(t, q, m);
        if (cmp == 0) cmp = (tl < qlen) ? -1 : 0;   /* 前缀短于查询 → 还要往后 */
        if (cmp < 0) lo = mid + 1;
        else { ans = mid; hi = mid - 1; }
    }
    return ans;
}

int wiki_search(const char *q, wiki_hit_t *out, int max) {
    if (!wiki_is_open() || !q || !q[0] || !out || max <= 0) return 0;
    size_t qlen = strlen(q);
    int id = lower_bound(q, qlen);
    int n = 0;
    char t[WIKI_TITLE_MAX];
    uint32_t c, o, cl, ul;
    for (; id < S.count && n < max; id++) {
        if (!read_rec(id, t, &c, &o, &cl, &ul)) break;
        if (memcmp(t, q, qlen) != 0) break;          /* 前缀不再匹配，收工 */
        out[n].id = id;
        memcpy(out[n].title, t, WIKI_TITLE_MAX);
        n++;
    }
    return n;
}

bool wiki_title(int id, char *buf, int cap) {
    if (!wiki_is_open() || !buf || cap <= 0) return false;
    uint32_t c, o, cl, ul;
    char t[WIKI_TITLE_MAX];
    if (!read_rec(id, t, &c, &o, &cl, &ul)) return false;
    snprintf(buf, (size_t)cap, "%s", t);
    return true;
}

int wiki_titles(int from, int n, wiki_hit_t *out) {
    if (!wiki_is_open() || !out || n <= 0) return 0;
    if (from < 0) from = 0;
    if (from >= S.count) return 0;
    if (from + n > S.count) n = S.count - from;

    if (!S.idx) return 0;
    int got = 0;
    if (fseek(S.idx, (long)from * IDX_REC, SEEK_SET) == 0) {
        uint8_t b[IDX_REC];
        for (int i = 0; i < n; i++) {
            if (fread(b, 1, IDX_REC, S.idx) != IDX_REC) break;
            out[got].id = from + i;
            memcpy(out[got].title, b, WIKI_TITLE_MAX);
            out[got].title[WIKI_TITLE_MAX - 1] = 0;
            got++;
        }
    }
    return got;
}

/* ---------------- 拼音搜索 ----------------
 * py.bin / pyi.bin 是 64 字节定长记录：key[60] 小写 ASCII + id u32。
 * 与标题索引同样二分，只是键换成拼音；没生成拼音索引时安静返回 0。 */
#define PY_KEY 60
#define PY_REC 64

int wiki_search_py(const char *q, int initials, wiki_hit_t *out, int max) {
    if (!wiki_is_open() || !q || !q[0] || !out || max <= 0) return 0;

    char key[PY_KEY];
    size_t ql = 0;
    for (const char *p = q; *p && ql < PY_KEY - 1; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == ' ') continue;                      /* 允许 "fu jian" 这种断开写 */
        key[ql++] = c;
    }
    key[ql] = 0;
    if (ql == 0) return 0;

    char path[PATH_CAP];
    snprintf(path, sizeof(path), "%s/%s", S.dir, initials ? "pyi.bin" : "py.bin");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    int n = (int)(sz / PY_REC);
    if (n <= 0) { fclose(f); return 0; }

    uint8_t rec[PY_REC];
    int lo = 0, hi = n - 1, at = n;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (fseek(f, (long)mid * PY_REC, SEEK_SET) != 0) break;
        if (fread(rec, 1, PY_REC, f) != PY_REC) break;
        if (memcmp(rec, key, ql) < 0) lo = mid + 1;
        else { at = mid; hi = mid - 1; }
    }

    int got = 0;
    for (int i = at; i < n && got < max; i++) {
        if (fseek(f, (long)i * PY_REC, SEEK_SET) != 0) break;
        if (fread(rec, 1, PY_REC, f) != PY_REC) break;
        if (memcmp(rec, key, ql) != 0) break;        /* 前缀不再匹配 */
        uint32_t id;
        memcpy(&id, rec + PY_KEY, 4);
        uint32_t c, o, cl, ul;
        if (!read_rec((int)id, out[got].title, &c, &o, &cl, &ul)) continue;
        out[got].id = (int)id;
        got++;
    }
    fclose(f);
    return got;
}

/* ------------------------------------------------------------------ HTML → 纯文本 */

static bool tag_is_block(const char *t, int n) {
    static const char *B[] = {
        "p", "div", "br", "hr", "li", "ul", "ol", "tr", "table", "h1", "h2", "h3",
        "h4", "h5", "h6", "blockquote", "dd", "dt", "section", "figure", "figcaption",
        "caption", "pre", "center", "article", "header", "footer",
    };
    for (size_t i = 0; i < sizeof(B) / sizeof(B[0]); i++) {
        if ((int)strlen(B[i]) == n && strncasecmp(t, B[i], (size_t)n) == 0) return true;
    }
    return false;
}

static int utf8_put(char *d, uint32_t cp) {
    if (cp < 0x80) { d[0] = (char)cp; return 1; }
    if (cp < 0x800) { d[0] = (char)(0xC0 | (cp >> 6)); d[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        d[0] = (char)(0xE0 | (cp >> 12));
        d[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        d[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    d[0] = (char)(0xF0 | (cp >> 18));
    d[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    d[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    d[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* 把 HTML 原地压成纯文本。返回新长度。out 指针永远 <= in 指针，可以原地写 */
/* 输出末尾已有几个换行（决定了还能不能再加一个） */
static inline int trailing_nl(const char *s, size_t w) {
    int k = 0;
    while (k < 3 && (size_t)k < w && s[w - 1 - (size_t)k] == '\n') k++;
    return k;
}

static size_t html_to_text(char *s, size_t n) {
    size_t i = 0, w = 0;
    bool at_line_start = true;

    while (i < n) {
        char c = s[i];

        if (c == '<') {
            /* 取标签名 */
            size_t j = i + 1;
            bool closing = false;
            if (j < n && s[j] == '/') { closing = true; j++; }
            size_t ns = j;
            while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z')
                             || (s[j] >= '0' && s[j] <= '9'))) j++;
            size_t nlen = j - ns;
            size_t gt = j;
            while (gt < n && s[gt] != '>') gt++;
            if (gt >= n) break;                     /* 没闭合的 '<'，丢掉剩余 */

            if (!closing && nlen == 6 && strncasecmp(s + ns, "script", 6) == 0) {
                /* script/style 内容整段跳过（预处理已剥，这里兜底） */
                i = gt + 1;
                continue;
            }
            if (!closing && nlen == 5 && strncasecmp(s + ns, "style", 5) == 0) {
                i = gt + 1;
                continue;
            }
            if (tag_is_block(s + ns, (int)nlen)) {
                /* 最多留 2 个换行 = 段落之间一个空行。
                 * ⚠ 必须看输出尾部判断：正文里满是 &#160; 这类不可见字符，
                 *   任何"输入侧计数器"都会被它们重置，结果每段后面刷 5~8 个空行。 */
                if (w > 0 && trailing_nl(s, w) < 2) s[w++] = '\n';
                at_line_start = true;
            }
            i = gt + 1;
            continue;
        }

        if (c == '&') {
            /* 实体：&amp; &lt; &gt; &quot; &nbsp; &#1234; &#x4e2d; */
            size_t semi = i + 1;
            while (semi < n && semi < i + 12 && s[semi] != ';') semi++;
            if (semi < n && s[semi] == ';') {
                uint32_t cp = 0;
                bool known = true;
                size_t len = semi - i - 1;
                const char *e = s + i + 1;
                if (len == 3 && !strncmp(e, "amp", 3)) cp = '&';
                else if (len == 2 && !strncmp(e, "lt", 2)) cp = '<';
                else if (len == 2 && !strncmp(e, "gt", 2)) cp = '>';
                else if (len == 4 && !strncmp(e, "quot", 4)) cp = '"';
                else if (len == 4 && !strncmp(e, "nbsp", 4)) cp = ' ';
                else if (len == 4 && !strncmp(e, "middot", 4)) cp = 0xB7;
                else if (len == 5 && !strncmp(e, "mdash", 5)) cp = 0x2014;
                else if (len == 5 && !strncmp(e, "ndash", 5)) cp = 0x2013;
                else if (len == 6 && !strncmp(e, "hellip", 6)) cp = 0x2026;
                else if (len >= 2 && e[0] == '#') {
                    if (e[1] == 'x' || e[1] == 'X') cp = (uint32_t)strtoul(e + 2, NULL, 16);
                    else cp = (uint32_t)strtoul(e + 1, NULL, 10);
                    if (cp == 0 || cp > 0x10FFFF) known = false;
                } else known = false;

                if (known) {
                    /* &#160; 不间断空格、&#8203; 零宽空格：一个当空格，一个直接扔 */
                    if (cp == 160) cp = ' ';
                    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) {
                        i = semi + 1;
                        continue;
                    }
                    if (cp == ' ') {
                        if (!at_line_start && w > 0 && s[w - 1] != ' ' && s[w - 1] != '\n') s[w++] = ' ';
                    } else {
                        w += (size_t)utf8_put(s + w, cp);
                        at_line_start = false;
                    }
                    i = semi + 1;
                    continue;
                }
                /* 不认识的实体原样留下 */
            }
        }

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!at_line_start && w > 0 && s[w - 1] != ' ' && s[w - 1] != '\n') s[w++] = ' ';
            i++;
            continue;
        }

        s[w++] = c;
        if ((unsigned char)c > 0x1F) at_line_start = false;
        i++;
    }

    /* 收尾：去掉尾部空白与多余换行 */
    while (w > 0 && (s[w - 1] == ' ' || s[w - 1] == '\n')) w--;
    return w;
}

/* ------------------------------------------------------------------ 折行 */

/* 与 ui_common.c 的 ui_text16w 同一口径：3 字节序列 = 16px(全角)，其余 = 8px */
static inline int char_adv(const unsigned char *p) {
    return ((*p & 0xE0) == 0xE0) ? 3 : 1;
}

/* 中文收尾标点：不能出现在行首，宁可让它多占一格挂在上一行末尾 */
static bool is_closing_punct(const char *p) {
    const unsigned char *u = (const unsigned char *)p;
    if ((u[0] & 0xF0) != 0xE0) return false;
    uint32_t cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    switch (cp) {
        case 0x3001: case 0x3002:            /* 、 。 */
        case 0xFF0C: case 0xFF1B: case 0xFF1A: case 0xFF01: case 0xFF1F: case 0xFF09:
        case 0x3009: case 0x300B: case 0x300D: case 0x300F: case 0x3011:
        case 0x3015: case 0x2026: case 0x201D:
            return true;
        default:
            return false;
    }
}

static bool line_is_noise(const char *s, size_t len) {
    if (len == 0) return false;
    if (len == 1) {
        char c = s[0];
        return c == '>' || c == '<' || c == '|' || c == '/' || c == '\\' ||
               c == '*' || c == '=' || c == '_' || c == '-' || c == '+' || c == '^';
    }
    for (size_t i = 0; i < len; i++) {
        if (s[i] != '>' && s[i] != '<' && s[i] != '.' && s[i] != ' ') return false;
    }
    return true;
}

static bool push_line(uint32_t off, uint16_t len) {
    if (S.nlines == S.lcap) {
        int nc = S.lcap ? S.lcap * 2 : 512;
        wiki_ln_t *p = ps_malloc((size_t)nc * sizeof(wiki_ln_t));
        if (!p) return false;
        if (S.line) {
            memcpy(p, S.line, (size_t)S.nlines * sizeof(wiki_ln_t));
            ps_free(S.line);
        }
        S.line = p;
        S.lcap = nc;
    }
    S.line[S.nlines].off = off;
    S.line[S.nlines].len = len;
    S.nlines++;
    return true;
}

/* 把 text 切成行：'\n' 强制断行，否则按 384px 折行。
 * 只记录 (起点, 长度)，不改动 text —— 见 wiki_ln_t 上的说明。 */
static int layout_lines(char *t, size_t n) {
    S.nlines = 0;
    size_t i = 0;
    int blanks = 0;

    while (i < n) {
        size_t j = i;
        int w = 0;
        size_t last_sp = 0;
        bool has_sp = false;
        while (j < n && t[j] != '\n') {
            int a = char_adv((const unsigned char *)t + j);
            if (j + (size_t)a > n) break;
            int cw = (a == 3) ? 16 : 8;
            if (w + cw > LINE_SOFT) break;
            if (a == 1 && t[j] == ' ' && j > i) { last_sp = j; has_sp = true; }
            w += cw;
            j += (size_t)a;
        }
        /* 标点悬挂：断点正好是收尾标点，就把它拉上来（软上限留的那一格正好够） */
        if (j < n && j > i && is_closing_punct(t + j)) {
            int a = char_adv((const unsigned char *)t + j);
            if (j + (size_t)a <= n) j += (size_t)a;
        }
        /* 英文别在单词中间断：断点落在 ASCII 单词里就退回最近的空格 */
        if (j < n && t[j] != '\n' && has_sp && last_sp > i + 8 &&
            ((t[j] >= 'a' && t[j] <= 'z') || (t[j] >= 'A' && t[j] <= 'Z') ||
             (t[j] >= '0' && t[j] <= '9'))) {
            j = last_sp;
        }

        size_t end = j;
        while (end > i && t[end - 1] == ' ') end--;

        /* 行首/行尾孤立的 '<' '>'：mw 删掉上标引用后残留的箭头，屏上就是噪点 */
        size_t st = i;
        while (st < end && (t[st] == '>' || t[st] == '<' || t[st] == ' ')) st++;
        while (end > st && (t[end - 1] == '>' || t[end - 1] == '<')) end--;
        while (end > st && t[end - 1] == ' ') end--;

        if (end <= st) {
            blanks++;
            if (blanks <= 1 && S.nlines > 0) {        /* 段落之间留一个空行 */
                if (!push_line((uint32_t)i, 0)) return -1;
            }
        } else if (!line_is_noise(t + st, end - st)) {
            blanks = 0;
            if (!push_line((uint32_t)st, (uint16_t)(end - st))) return -1;
        }

        if (j < n && t[j] == '\n') j++;
        if (j == i) j = i + 1;                        /* 防御：绝不原地打转 */
        i = j;
    }
    return S.nlines;
}

/* ------------------------------------------------------------------ 载入 */

int wiki_load(int id) {
    if (!wiki_is_open()) return -1;
    if (id < 0 || id >= S.count) return -2;

    uint32_t chunk, off, clen, ulen;
    char title[WIKI_TITLE_MAX];
    if (!read_rec(id, title, &chunk, &off, &clen, &ulen)) return -2;
    if (chunk == 0xFFFFFFFFu) return -3;
    if (ulen == 0 || ulen > WIKI_MAX_ARTICLE || clen == 0 || clen > WIKI_MAX_ARTICLE) return -3;

    char cp[PATH_CAP];
    snprintf(cp, sizeof(cp), "%s/articles_%04d.dat", S.dir, (int)chunk);

    FILE *f = fopen(cp, "rb");
    if (!f) { ESP_LOGE(TAG, "打不开 %s", cp); return -4; }

    uint8_t hdr[FRAME_HDR];
    if (fseek(f, (long)off, SEEK_SET) != 0 || fread(hdr, 1, FRAME_HDR, f) != FRAME_HDR) {
        fclose(f); return -5;
    }
    if (hdr[0] != FRAME_MAGIC0 || hdr[1] != 'K' || hdr[2] != '1' || hdr[3] != 0) {
        fclose(f); return -5;
    }
    uint32_t h_ulen, h_clen;
    memcpy(&h_ulen, hdr + 4, 4);
    memcpy(&h_clen, hdr + 8, 4);
    if (h_ulen != ulen || h_clen != clen) { fclose(f); return -5; }

    uint8_t *comp = ps_malloc(clen);
    char    *raw  = ps_malloc((size_t)ulen + 1);
    if (!comp || !raw) {
        ps_free(comp); ps_free(raw); fclose(f);
        ESP_LOGE(TAG, "PSRAM 不足：要 %u + %u 字节", (unsigned)clen, (unsigned)ulen);
        return -8;
    }
    size_t rd = fread(comp, 1, clen, f);
    fclose(f);
    if (rd != clen) { ps_free(comp); ps_free(raw); return -6; }

    size_t out = 0;
    int rc = lz4_dec_block(comp, clen, (uint8_t *)raw, ulen, &out);
    ps_free(comp);
    if (rc != 0 || out != ulen) {
        ps_free(raw);
        ESP_LOGE(TAG, "解压失败 id=%d rc=%d out=%u/%u", id, rc, (unsigned)out, (unsigned)ulen);
        return -7;
    }
    raw[ulen] = 0;

    size_t tlen = html_to_text(raw, ulen);
    ESP_LOGD(TAG, "id=%d ulen=%u 去标签后=%u", id, (unsigned)ulen, (unsigned)tlen);
#ifdef WIKI_DEBUG_DUMP
    { extern void wiki_dbg(const char*, size_t); wiki_dbg(raw, tlen); }
#endif

    ps_free(S.text);
    S.text = raw;                       /* 直接接管，省一次 1.7MB 拷贝 */
    if (layout_lines(S.text, tlen) < 0) {
        ps_free(S.text); S.text = NULL; S.nlines = 0;
        return -8;
    }
    return S.nlines;
}

int wiki_lines(void) { return S.nlines; }

const char *wiki_line(int i, int *len) {
    if (!S.text || i < 0 || i >= S.nlines) {
        if (len) *len = 0;
        return "";
    }
    if (len) *len = S.line[i].len;
    return S.text + S.line[i].off;
}
