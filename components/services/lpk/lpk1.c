/* lpk1.c — LPK 自包含资源包读取器 (格式见 lpk1.h; 纯 C, 零 ESP 依赖) */
#include "lpk1.h"

#include <stdlib.h>
#include <string.h>

/* ---------- 小端读取 (目标机与主机都是小端, 仍显式按字节拼, 不依赖对齐) ---------- */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------- crc32 (zlib 多项式, 16 项半字节表; 与 tools 侧 binascii.crc32 逐位一致) ---------- */
static const uint32_t CRC_T[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};
uint32_t lpk_crc32(uint32_t crc, const void *buf, uint32_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ CRC_T[crc & 0x0Fu];
        crc = (crc >> 4) ^ CRC_T[crc & 0x0Fu];
    }
    return ~crc;
}

uint32_t lpk_crc32_file(const char *path) {
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t buf[4096];
    uint32_t crc = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) crc = lpk_crc32(crc, buf, (uint32_t)n);
    bool err = ferror(f) != 0;
    fclose(f);
    return err ? 0 : crc;
}

/* ---------- 路径守卫 (与 am_manifest.c 同一套规则) ---------- */
bool lpk_path_ok(const char *rel) {
    if (!rel || !rel[0]) return false;
    size_t n = strlen(rel);
    if (n >= LPK_MAX_NAME) return false;
    if (rel[0] == '/') return false;                 /* 绝对路径 */
    if (rel[n - 1] == '/') return false;             /* 目录项不该出现在这里 */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)rel[i];
        if (c < 0x20 || c == 0x7F) return false;     /* 控制字符 */
        if (c == '\\') return false;                 /* 反斜杠 = Windows 式穿越 */
        if (c == ':')  return false;                 /* 盘符 / URL scheme */
    }
    const char *p = rel;
    while (*p) {
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t seg = (size_t)(p - s);
        if (seg == 0) return false;                  /* 空段 // */
        if (seg == 1 && s[0] == '.') return false;   /* . */
        if (seg == 2 && s[0] == '.' && s[1] == '.') return false;  /* .. */
        if (*p == '/') p++;
    }
    return true;
}

/* ============================ 打开 / 校验 ============================ */

static void close_and_fail(lpk_t *h, int rc) {
    if (h->fp) { fclose(h->fp); h->fp = NULL; }
    h->loaded = 0;
    (void)rc;
}

/* ASCII 折叠。只处理 A-Z —— UTF-8 多字节里的高位字节原样比较，
 * 不会被误折叠（中文文件名照常逐字节精确匹配）。 */
static inline unsigned char lpk_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

/* 重复路径 / 大小写冲突（读取端最后一道闸门）。
 *
 * 为什么必须在 open 时就查，而不是留到查找时：
 *   包里若有两条**完全同名**的条目，`lpk_find` 永远返回第一条 —— 读出来的到底是哪份
 *   资源，完全取决于打包顺序。这类包在我们自己的打包器里根本产不出来（它拒绝重名），
 *   所以一旦出现就说明包被改过/拼过，宁可整包拒绝，也不要"能跑但内容随机"。
 *
 * 为什么是 O(n²)：仙剑 109 条 ⇒ 约 6 千次折叠比较（<1 ms）；条数上限 8192 的畸形包
 *   最坏约 3 千万次（≈0.3 s），只在打开时付一次，可以接受。另外不额外分配内存。
 *
 * 返回：LPK_OK / LPK_E_ENTRY（完全同名）/ LPK_E_AMBIG（仅大小写不同）。 */
static int check_dups(const lpk_entry_t *e, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            const char *a = e[i].name, *b = e[j].name;
            bool same = true;
            while (*a && *b) {
                if (lpk_lower((unsigned char)*a) != lpk_lower((unsigned char)*b)) { same = false; break; }
                a++; b++;
            }
            if (same && *a == 0 && *b == 0) {
                return (strcmp(e[i].name, e[j].name) == 0) ? LPK_E_ENTRY : LPK_E_AMBIG;
            }
        }
    }
    return LPK_OK;
}

int lpk_open(lpk_t *h, const char *path, lpk_entry_t *entries, int max_entries) {
    if (!h) return LPK_E_ARG;
    memset(h, 0, sizeof(*h));
    if (!path || !path[0]) return LPK_E_ARG;

    FILE *f = fopen(path, "rb");
    if (!f) return LPK_E_OPEN;
    h->fp = f;

    /* ---- 真实文件大小: 用来发现"被截断的包", 比只信头里的 total_size 严格 ---- */
    if (fseek(f, 0, SEEK_END) != 0) { close_and_fail(h, LPK_E_IO); return LPK_E_IO; }
    long fsz = ftell(f);
    if (fsz < 0) { close_and_fail(h, LPK_E_IO); return LPK_E_IO; }
    if (fseek(f, 0, SEEK_SET) != 0) { close_and_fail(h, LPK_E_IO); return LPK_E_IO; }

    uint8_t hdr[LPK_HDR_SIZE];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { close_and_fail(h, LPK_E_TRUNC); return LPK_E_TRUNC; }

    if (memcmp(hdr, LPK_MAGIC, 4) != 0) { close_and_fail(h, LPK_E_MAGIC); return LPK_E_MAGIC; }
    uint16_t ver = rd16(hdr + 4);
    uint16_t hsz = rd16(hdr + 6);
    if (ver != LPK_VERSION) { close_and_fail(h, LPK_E_VERSION); return LPK_E_VERSION; }
    if (hsz != LPK_HDR_SIZE) { close_and_fail(h, LPK_E_HDR); return LPK_E_HDR; }

    h->total_size = rd32(hdr + 8);
    h->lua_off    = rd32(hdr + 12);
    h->lua_size   = rd32(hdr + 16);
    h->lua_crc    = rd32(hdr + 20);
    uint32_t man_off = rd32(hdr + 24);
    uint32_t man_size = rd32(hdr + 28);
    h->count      = rd32(hdr + 32);
    h->data_off   = rd32(hdr + 36);
    h->pkg_crc    = rd32(hdr + 40);
    h->flags      = rd32(hdr + 44);

    /* ---- 头自洽 (全部用 64 位算, 杜绝 32 位回绕) ---- */
    if ((uint64_t)h->total_size != (uint64_t)fsz) { close_and_fail(h, LPK_E_TRUNC); return LPK_E_TRUNC; }
    /* lua_size == 0 是合法的: 纯资源包(仙剑这类 C 引擎游戏就没有 Lua 入口) */
    if (h->lua_size > 0 &&
        ((uint64_t)h->lua_off + h->lua_size > h->total_size || h->lua_off < LPK_HDR_SIZE)) {
        close_and_fail(h, LPK_E_HDR); return LPK_E_HDR;
    }
    if ((uint64_t)man_off + man_size > h->total_size) { close_and_fail(h, LPK_E_HDR); return LPK_E_HDR; }
    if (h->count > LPK_MAX_ENTRIES) { close_and_fail(h, LPK_E_NOMEM); return LPK_E_NOMEM; }
    if ((uint64_t)h->count * LPK_ENTRY_SIZE > man_size) { close_and_fail(h, LPK_E_HDR); return LPK_E_HDR; }

    if (!entries) { h->loaded = 0; return LPK_OK; }      /* 只要头 */
    if (max_entries < (int)h->count) { close_and_fail(h, LPK_E_NOMEM); return LPK_E_NOMEM; }

    if (h->count == 0) { h->loaded = 0; return LPK_OK; } /* 合法: 只有 Lua 没有资源 */

    /* ---- 读索引: 一次读到尾部(含名字池), 避免逐条 seek ---- */
    uint8_t *meta = (uint8_t *)malloc(man_size);
    if (!meta) { close_and_fail(h, LPK_E_NOMEM); return LPK_E_NOMEM; }
    if (fseek(f, (long)man_off, SEEK_SET) != 0 || fread(meta, 1, man_size, f) != man_size) {
        free(meta); close_and_fail(h, LPK_E_IO); return LPK_E_IO;
    }

    int rc = LPK_OK;
    for (uint32_t i = 0; i < h->count; i++) {
        const uint8_t *r = meta + (size_t)i * LPK_ENTRY_SIZE;
        uint32_t name_off = rd32(r + 0);
        uint32_t off      = rd32(r + 4);
        uint32_t size     = rd32(r + 8);
        uint32_t crc      = rd32(r + 12);
        uint16_t nlen     = rd16(r + 16);
        uint8_t  type     = r[18];

        /* 名字必须在字符串池里且不越界 */
        if (nlen == 0 || nlen >= LPK_MAX_NAME) { rc = LPK_E_ENTRY; break; }
        if ((uint64_t)name_off + nlen > man_size) { rc = LPK_E_ENTRY; break; }
        const char *nm = (const char *)(meta + name_off);
        if (memchr(nm, 0, nlen) != NULL) { rc = LPK_E_PATH; break; }   /* 名字里不许有 NUL */

        lpk_entry_t *e = &entries[i];
        memcpy(e->name, nm, nlen);
        e->name[nlen] = 0;

        if (!lpk_path_ok(e->name)) { rc = LPK_E_PATH; break; }
        if (size == 0) { rc = LPK_E_ENTRY; break; }
        if ((uint64_t)off + size > h->total_size) { rc = LPK_E_ENTRY; break; }
        if ((uint64_t)off < (uint64_t)man_off + man_size) { rc = LPK_E_ENTRY; break; }

        e->off = off; e->size = size; e->crc32 = crc; e->type = type;
    }
    free(meta);
    if (rc != LPK_OK) { close_and_fail(h, rc); return rc; }

    /* 重复路径 / 大小写冲突：整包拒绝，不给"能打开但读到哪份看运气"的机会 */
    rc = check_dups(entries, h->count);
    if (rc != LPK_OK) { close_and_fail(h, rc); return rc; }

    h->loaded = (int)h->count;
    return LPK_OK;
}

void lpk_close(lpk_t *h) {
    if (!h) return;
    if (h->fp) { fclose(h->fp); h->fp = NULL; }
    h->loaded = 0;
}

int lpk_find(const lpk_t *h, const lpk_entry_t *entries, const char *name) {
    if (!h || !entries || !name || !name[0]) return -1;
    for (int i = 0; i < h->loaded; i++)
        if (strcmp(entries[i].name, name) == 0) return i;
    return -1;
}

int lpk_find_ci(const lpk_t *h, const lpk_entry_t *entries, const char *name) {
    if (!h || !entries || !name || !name[0]) return -1;
    /* ① 精确命中: 常见情况，一条都不用多看 */
    for (int i = 0; i < h->loaded; i++)
        if (strcmp(entries[i].name, name) == 0) return i;
    /* ② 折叠大小写再扫一遍；命中两条及以上 ⇒ 无法判定，明确拒绝而不是随便挑 */
    int hit = -1;
    for (int i = 0; i < h->loaded; i++) {
        const char *a = entries[i].name, *b = name;
        while (*a && *b && lpk_lower(*a) == lpk_lower(*b)) { a++; b++; }
        if (*a == 0 && *b == 0) {
            if (hit >= 0) return LPK_E_AMBIG;
            hit = i;
        }
    }
    return hit;
}

/* ============================ 读 ============================ */

int lpk_read(lpk_t *h, const lpk_entry_t *e, uint32_t off, void *buf, uint32_t len) {
    if (!h || !h->fp || !e || !buf) return LPK_E_ARG;
    if (len == 0) return 0;
    if ((uint64_t)off + len > e->size) return LPK_E_ARG;      /* 读到资源尾部之外 */
    if (fseek(h->fp, (long)(e->off + off), SEEK_SET) != 0) return LPK_E_IO;
    size_t got = fread(buf, 1, len, h->fp);
    if (got != len) return LPK_E_IO;                          /* 卡拔了/文件被截断 */
    return LPK_OK;
}

int lpk_read_lua(lpk_t *h, void *buf, uint32_t cap, uint32_t *out_len) {
    if (!h || !h->fp || !buf) return LPK_E_ARG;
    if (h->lua_size == 0) return LPK_E_ARG;                   /* 纯资源包, 没有 Lua 入口 */
    if (cap < h->lua_size) return LPK_E_NOMEM;
    if (fseek(h->fp, (long)h->lua_off, SEEK_SET) != 0) return LPK_E_IO;
    if (fread(buf, 1, h->lua_size, h->fp) != h->lua_size) return LPK_E_IO;
    if (h->lua_crc && lpk_crc32(0, buf, h->lua_size) != h->lua_crc) return LPK_E_ENTRY;
    if (out_len) *out_len = h->lua_size;
    return LPK_OK;
}

int lpk_verify_pkg(lpk_t *h) {
    if (!h || !h->fp) return LPK_E_ARG;
    if (h->pkg_crc == 0) return LPK_OK;                       /* 未记录, 跳过 */
    if (fseek(h->fp, 0, SEEK_SET) != 0) return LPK_E_IO;
    uint8_t buf[4096];
    uint32_t crc = 0;
    long left = (long)h->total_size;
    long pos = 0;
    while (left > 0) {
        size_t want = (size_t)(left < (long)sizeof(buf) ? left : (long)sizeof(buf));
        size_t got = fread(buf, 1, want, h->fp);
        if (got != want) return LPK_E_IO;
        /* 约定: pkg_crc 计算时把头部 [40,44) 这 4 字节(pkg_crc 字段本身)视为 0，
         * 否则"回填 CRC 会改变 CRC"就成了鸡生蛋问题。生成器按同一约定算。 */
        for (size_t i = 0; i < got; i++) {
            long a = pos + (long)i;
            if (a >= 40 && a < 44) buf[i] = 0;
        }
        crc = lpk_crc32(crc, buf, (uint32_t)got);
        left -= (long)got;
        pos += (long)got;
    }
    return (crc == h->pkg_crc) ? LPK_OK : LPK_E_ENTRY;
}
