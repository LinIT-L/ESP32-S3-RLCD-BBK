/**
 * am_core_logic.c — 统一应用核心的纯逻辑实现（见头文件，宿主机可编译）
 *
 * 本文件是 tests/app_core 的被测对象，也是设备端唯一使用的实现 ——
 * 不存在"测试副本"，改了这里两边同时生效。
 */
#include "am_core_logic.h"

#include <string.h>

void am_core_norm(const char *in, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!in) return;

    while (*in == ' ' || *in == '\t') in++;
    size_t n = strlen(in);
    while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t')) n--;

    /* 去尾部扩展名：**只认应用包扩展名**（.lua / .lpk，大小写不敏感）。
     * ⚠ 曾经无条件切最后一个 '.'，于是 LPK 的 app_id "com.lintos.map" 被切成
     *   "com.lintos" —— 所有带点号的 app_id 都会撞在一起（旧 am_lpkg_stem_of
     *   还因此把 com.lintos.recorder 截成 "com"，registry 永远查不到）。
     *   现在只切真正的文件扩展名，标识符里的点原样保留。 */
    if (n > 4) {
        const char *t = in + n - 4;
        const bool lua = (t[0] == '.' && (t[1] | 0x20) == 'l' &&
                          (t[2] | 0x20) == 'u' && (t[3] | 0x20) == 'a');
        const bool lpk = (t[0] == '.' && (t[1] | 0x20) == 'l' &&
                          (t[2] | 0x20) == 'p' && (t[3] | 0x20) == 'k');
        if (lua || lpk) n -= 4;
    }

    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');   /* 非 ASCII（中文）原样 */
        out[i] = c;
    }
    out[n] = 0;
}

void am_core_make_key(am_core_kind_t kind, const char *raw, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    const char *pre = (kind == AM_KIND_LUA) ? "lua:"
                    : (kind == AM_KIND_LPK) ? "lpk:" : "builtin:";
    char norm[96];
    am_core_norm(raw, norm, sizeof(norm));
    size_t pl = strlen(pre);
    size_t nl = strlen(norm);
    if (pl + nl >= cap) nl = (cap > pl) ? (cap - pl - 1) : 0;
    memcpy(out, pre, pl);
    memcpy(out + pl, norm, nl);
    out[pl + nl] = 0;
}

bool am_core_same(const char *key_a, const char *key_b)
{
    if (!key_a || !key_b || !key_a[0] || !key_b[0]) return false;
    /* 键里已带类型前缀 ⇒ 不同类型天然不相等。⛔ 不做任何"看起来像"的合并。 */
    return strcmp(key_a, key_b) == 0;
}

bool am_core_type_is_res(const char *index_type)
{
    if (!index_type) return false;
    return strcmp(index_type, "asset") == 0 || strcmp(index_type, "lpk") == 0;
}

/* 版本字符串比较（都是定长数字串 → 字典序即时间序）。
 * 任一侧为空表示"不知道"：返回 0，调用方按"没新版"处理，绝不凭空造更新。 */
static int ver_cmp(const char *a, const char *b)
{
    if (!a || !a[0]) return 0;
    if (!b || !b[0]) return 0;
    return strcmp(a, b);
}

am_st_t am_core_decide(bool installed, bool in_catalog, bool idx_ready,
                       const char *build_local, const char *ver_local,
                       const char *build_remote, const char *ver_remote)
{
    if (!installed) return AM_ST_NONE;

    if (!in_catalog) {
        /* 索引拉到过 + 没有这条 ⇒ 应用已下架（按钮仍是「卸载」）。
         * 索引没拉到（离线/首进商店）⇒ 只代表"不知道" → 按已安装处理。 */
        return idx_ready ? AM_ST_ORPHAN : AM_ST_INSTALLED;
    }

    /* 已安装 + 目录有：比版本。build 优先（定长数字串），退回 ver（数字串）。 */
    return am_core_remote_newer(build_local, ver_local, build_remote, ver_remote)
         ? AM_ST_UPDATE : AM_ST_INSTALLED;
    /* 本地比服务器新 → INSTALLED ⇒ 按钮「卸载」，不会降级更新 ✓ */
}

bool am_core_remote_newer(const char *build_local, const char *ver_local,
                          const char *build_remote, const char *ver_remote)
{
    int c = ver_cmp(build_remote, build_local);
    if (c == 0 && !(build_remote && build_remote[0] && build_local && build_local[0]))
        c = ver_cmp(ver_remote, ver_local);
    return c > 0;
}

bool am_core_center_ok(bool valid, am_core_kind_t kind, unsigned loc,
                       bool storage_ok, bool center_hide)
{
    if (!valid) return false;
    if (center_hide) return false;            /* 同 id 已有内置 C 应用，避免两条 */
    if (kind == AM_KIND_BUILTIN) return true;
    if (loc & AM_LOC_FLASH) return true;      /* 内置那份永远可访问 */
    if (loc & AM_LOC_TF) return storage_ok;   /* TF 拔了 ⇒ 暂不可见，但安装事实不变 */
    return false;                             /* 未安装（含未装 LPK 包）*/
}

bool am_core_store_ok(bool valid, am_core_kind_t kind)
{
    if (!valid) return false;
    return kind != AM_KIND_BUILTIN;           /* 内置 C 不进商店 */
}
