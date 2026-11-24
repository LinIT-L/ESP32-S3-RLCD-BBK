/* lua_path_guard.c — 见头文件。纯 C, 不依赖 ESP-IDF。 */
#include "lua_path_guard.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define PG_MAX_SEG 24

/* ★ 2026-10-31 修: **反斜杠也是分隔符**。
 *   依据: esp-idf-v60/components/fatfs/src/ff.c:54
 *         #define IsSeparator(c)  ((c) == '/' || (c) == '\\')
 *   一开始只按 '/' 切段, 于是 "..\\..\\x" 被当成一个普通文件名原样放过,
 *   拼成 "<数据目录>/..\..\x" —— 到了 FatFs 就变成真的向上穿越两级。
 *   这是主机测试 T8 抓出来的真实越权路径, 不是理论风险。 */
static bool is_sep(char c) { return c == '/' || c == '\\'; }

/* 判断 a 是否以 b 为前缀且边界是 '/' 或结束（防止 /data/foo 匹配到 /data/foobar） */
static bool prefix_dir(const char *a, const char *b)
{
    size_t n = strlen(b);
    if (strncmp(a, b, n) != 0) return false;
    if (a[n] == 0) return true;
    if (a[n] == '/') return true;
    return false;
}

bool lua_path_guard(const char *data_dir, const char *app_dir,
                    const char *in, bool want_write,
                    char *out, size_t cap)
{
    if (!data_dir || !data_dir[0] || !in || !in[0] || !out || cap < 2) return false;

    /* 绝对路径里出现 ".." 一律拒绝 —— 不做"归一后仍落在允许目录所以放行"这种推理:
     * 那种推理依赖归一化实现完全正确, 一旦有偏差就是越权。绝对路径本来也不需要回退。
     * 反斜杠同样算分隔符(FatFs 语义), 所以 "..\\.." 也在这里被拦下。 */
    const bool is_abs = is_sep(in[0]);
    if (is_abs) {
        for (const char *q = in; *q; q++)
            if (q[0] == '.' && q[1] == '.' &&
                (q == in || is_sep(q[-1])) && (q[2] == 0 || is_sep(q[2])))
                return false;
    }

    /* 1) 逐段规范化: 把 in 拆成段压栈, ".." 弹栈, 弹出的段数超过已压入的段数
     *    = 试图越过根 ⇒ 直接拒绝（不管后面拼成什么）。 */
    const char *seg[PG_MAX_SEG];
    size_t seg_len[PG_MAX_SEG];
    int nseg = 0;
    const char *p = in;
    while (*p) {
        while (is_sep(*p)) p++;                /* 跳过重复/前导斜杠(含反斜杠) */
        if (!*p) break;
        const char *s = p;
        while (*p && !is_sep(*p)) p++;
        size_t l = (size_t)(p - s);

        if (l == 1 && s[0] == '.') continue;   /* "." 忽略 */
        if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (nseg == 0) return false;       /* 越过根 ⇒ 拒绝 */
            nseg--;
            continue;
        }
        if (nseg >= PG_MAX_SEG) return false;  /* 层数过多 */
        seg[nseg] = s;
        seg_len[nseg] = l;
        nseg++;
    }

    /* 2) 相对路径拼到 data_dir 下; 绝对路径自己带根, 先试 data_dir 再说 */
    char norm[256];
    size_t dl = strlen(data_dir);
    if (dl >= sizeof(norm)) return false;
    memcpy(norm, data_dir, dl);
    size_t o = dl;
    /* data_dir 末尾可能有 '/', 规整掉 */
    while (o > 1 && norm[o - 1] == '/') o--;

    if (is_abs) {
        /* 绝对路径: 先规整成 "/a/b", 再判断是否落在 data_dir / app_dir 内。
         * 注意不能用上面的栈结果直接拼, 因为绝对路径的根不是 data_dir。 */
        o = 0;
        for (int i = 0; i < nseg; i++) {
            if (o + 1 + seg_len[i] + 1 > sizeof(norm)) return false;
            norm[o++] = '/';
            memcpy(norm + o, seg[i], seg_len[i]);
            o += seg_len[i];
        }
        norm[o] = 0;
        if (o == 0) return false;
    } else {
        for (int i = 0; i < nseg; i++) {
            if (o + 1 + seg_len[i] + 1 > sizeof(norm)) return false;
            norm[o++] = '/';
            memcpy(norm + o, seg[i], seg_len[i]);
            o += seg_len[i];
        }
        norm[o] = 0;
    }

    /* 3) 授权判断 */
    const bool in_data = prefix_dir(norm, data_dir);
    const bool in_app  = (app_dir && app_dir[0]) ? prefix_dir(norm, app_dir) : false;

    if (want_write) {
        if (!in_data) return false;            /* 写: 只在数据目录 */
    } else {
        if (!in_data && !in_app) return false; /* 读: 数据目录 + 自身目录 */
    }

    size_t nl = strlen(norm);
    if (nl + 1 > cap) return false;            /* 超长拒绝, 不截断 */
    memcpy(out, norm, nl + 1);
    return true;
}

bool lua_path_appid(const char *script_path, char *out, size_t cap)
{
    if (!script_path || !script_path[0] || !out || cap < 2) return false;

    const char *base = script_path;
    for (const char *q = script_path; *q; q++)
        if (is_sep(*q)) base = q + 1;

    size_t bl = strlen(base);
    if (bl >= 4 && strcmp(base + bl - 4, ".lua") == 0) bl -= 4;

    size_t o = 0;
    for (size_t i = 0; i < bl; i++) {
        const char c = base[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) continue;                      /* 中文/空格等一律丢弃 */
        if (o + 1 >= cap) return false;
        out[o++] = c;
    }
    if (o > 0) { out[o] = 0; return true; }

    /* ★ 纯中文名(商店里大量存在, 如"英雄坛说.lua"): 上面会全被过滤光。
     *   不能因此拒绝启动 —— 改用**名字哈希**生成稳定 id。
     *   规则(与 lua_sandbox_ns / 文档统一): 哈希型 appid = 恰好 8 位小写十六进制。
     *   不再加 'a' 前缀 —— 上一版写成 "a%08x"(9 位), 与文档里 "8hex" 的说法对不上,
     *   两层规则不一致会让人误判 NVS 命名空间长度。同一文件名永远得到同一 id。 */
    if (bl == 0) return false;
    uint32_t h = 2166136261u;                   /* FNV-1a */
    for (size_t i = 0; i < bl; i++) {
        h ^= (uint8_t)base[i];
        h *= 16777619u;
    }
    if (cap < 9) return false;
    snprintf(out, cap, "%08lx", (unsigned long)(h & 0xFFFFFFFFu));
    return true;
}
