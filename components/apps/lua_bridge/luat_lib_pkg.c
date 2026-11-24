/* luat_lib_pkg.c — LinTOS Lua 桥 · pkg 模块 (单文件应用包内资源按需读取)
 *
 * 为什么需要它
 * ------------
 * 之前「应用 + 素材」是两个东西: 应用是 .lua, 素材是 zip 资源包, 装完还要**解压到 SD**
 * (几十 MB 的包解包慢、占空间, 且 am_asset 对 deflate 条目在真机上会崩)。
 * 这里改成: **一个文件 = Lua 脚本 + 尾部 ZIP_STORED**, 运行时直接按名字从包里读,
 * 不解压、不建素材目录、不把整个包读进内存。
 *
 * 文件格式 (由 tools/pack_app.py --pkg 生成)
 * -----------------------------------------
 *   --@lapp v1
 *   --@app name=...
 *   --@pkg offset=000093269 size=01868354     ← 固定宽度, 便于打包器回填
 *   <Lua 脚本...>                              ← 前 offset 字节
 *   <ZIP 数据 (全部 STORED)>                   ← offset..offset+size
 *
 * ⚠ 只支持 **STORED(method=0)**: 固件对 deflate 的 tinfl 路径在真机上栈溢出过
 *   (am_asset.c:181 有同样的注释)。打包器必须用 ZIP_STORED, open 时遇到非 0 条目直接跳过。
 *
 * API
 * ---
 *   app.pkg.self()            -> h | nil   打开**当前应用**的单文件包 (沙箱记录的脚本路径)
 *   app.pkg.open(path)        -> h | nil   打开任意单文件包 (路径走 lua_path_guard, 只读)
 *   app.pkg.exists(h, name)   -> bool
 *   app.pkg.size(h, name)     -> n | nil
 *   app.pkg.read(h, name)     -> string | nil   按需读一个条目 (只读它, 不读别的)
 *   app.pkg.list(h)           -> { name = size, ... }
 *   app.pkg.close(h)                                (或等 GC / 应用退出自动关)
 *
 * 安全
 * ----
 *   · 条目名禁止 ".." 与绝对路径 (防 zip slip 语义; 这里不落盘, 但名字也别乱指)
 *   · 每次读都校验 local header + [data_off, data_off+size) 落在包的数据区内
 *   · 单条目上限 4 MB, 条数上限 256, 句柄上限 4
 *   · 不常驻 FILE*: read 时开关文件, 避免句柄泄漏与两个应用同时持同一句柄
 *   · 退出 (lua_bridge_reset_app_state) 统一释放; __gc 也会释放
 *
 * 内存
 * ----
 *   · 条目表 (名字 96B + 16B) × 256 ≈ 28 KB, 放 PSRAM; 只在实际打开时分配
 *   · 读出的字节只经过 Lua 字符串一次 (lua_pushlstring), 由 GC 回收; 不缓存
 */
#include "lua_bridge.h"
#include "lua_sandbox.h"
#include "lua_path_guard.h"

#define PKG_MAX          4
#define PKG_ENT_MAX      256
#define PKG_NAME_MAX     96
#define PKG_HDR_SCAN     2048          /* 扫 --@pkg 头的字节数 */
#define PKG_EOCD_SCAN    (64 * 1024)   /* zip 注释上限 */
#define PKG_ENT_MAX_SIZE (4u << 20)    /* 单条目 4 MB 防御上限 */
#define PKG_MT           "lintos.pkg"

typedef struct {
    char     name[PKG_NAME_MAX];
    uint32_t local_off;    /* local file header 相对 zip 起点的偏移 */
    uint32_t size;         /* 未压缩大小 (= 压缩大小, 因为只收 STORED) */
} pkg_ent_t;

typedef struct {
    bool       used;
    char       path[192];
    uint32_t   pkg_off;    /* zip 数据在文件里的绝对起点 */
    uint32_t   pkg_size;   /* zip 字节数 */
    uint32_t   file_size;
    int        n;
    pkg_ent_t *ent;        /* PSRAM */
} pkg_t;

static EXT_RAM_BSS_ATTR pkg_t s_pkg[PKG_MAX];

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void pkg_release(pkg_t *pk)
{
    if (pk->ent) { heap_caps_free(pk->ent); pk->ent = NULL; }
    pk->used = false;
    pk->n = 0;
    pk->path[0] = 0;
}

/* 从头部文本里解析 --@pkg offset=N size=M。返回 false = 不是单文件包。 */
static bool parse_pkg_header(const char *buf, size_t len, uint32_t *off, uint32_t *size)
{
    const char *key = strstr(buf, "--@pkg");
    if (!key || (size_t)(key - buf) >= len) return false;
    const char *o = strstr(key, "offset=");
    const char *s = strstr(key, "size=");
    if (!o || !s) return false;
    /* 只认这一行内的数字 (遇到换行就停) */
    unsigned long ov = strtoul(o + 7, NULL, 10);
    unsigned long sv = strtoul(s + 5, NULL, 10);
    if (ov == 0 || sv == 0) return false;
    *off = (uint32_t)ov;
    *size = (uint32_t)sv;
    return true;
}

/* 打开一个单文件包: 解析头 → 找 EOCD → 读中央目录 → 建条目表 */
static int pkg_open_path(const char *path, pkg_t *pk)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -2; }
    long fsz = ftell(f);
    if (fsz < 128) { fclose(f); return -2; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -2; }

    uint8_t hdr[PKG_HDR_SCAN + 1];
    size_t hn = fread(hdr, 1, PKG_HDR_SCAN, f);
    hdr[hn] = 0;
    uint32_t pkg_off = 0, pkg_size = 0;
    if (!parse_pkg_header((const char *)hdr, hn, &pkg_off, &pkg_size)) { fclose(f); return -3; }
    if ((uint64_t)pkg_off + pkg_size > (uint64_t)fsz) { fclose(f); return -4; }

    /* 在 zip 尾部 64KB 内找 EOCD */
    uint32_t scan = (pkg_size < PKG_EOCD_SCAN) ? pkg_size : PKG_EOCD_SCAN;
    if (scan < 22) { fclose(f); return -4; }
    uint8_t *tail = heap_caps_malloc(scan, MALLOC_CAP_SPIRAM);
    if (!tail) { fclose(f); return -5; }
    if (fseek(f, (long)(pkg_off + pkg_size - scan), SEEK_SET) != 0 ||
        fread(tail, 1, scan, f) != scan) {
        heap_caps_free(tail); fclose(f); return -5;
    }
    long eocd = -1;
    for (long i = (long)scan - 22; i >= 0; i--) {
        if (rd32(tail + i) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd < 0) { heap_caps_free(tail); fclose(f); return -6; }

    uint16_t n_ent   = rd16(tail + eocd + 10);
    uint32_t cd_off  = rd32(tail + eocd + 16);
    if (n_ent == 0 || n_ent > PKG_ENT_MAX) { heap_caps_free(tail); fclose(f); return -6; }
    if ((uint64_t)cd_off + 46u * n_ent > pkg_size) { heap_caps_free(tail); fclose(f); return -6; }
    heap_caps_free(tail);

    pkg_ent_t *ents = heap_caps_malloc(sizeof(pkg_ent_t) * PKG_ENT_MAX, MALLOC_CAP_SPIRAM);
    if (!ents) { fclose(f); return -5; }

    int n = 0;
    uint32_t p = cd_off;
    for (int i = 0; i < n_ent && n < PKG_ENT_MAX; i++) {
        uint8_t cd[46];
        if (fseek(f, (long)(pkg_off + p), SEEK_SET) != 0) break;
        if (fread(cd, 1, 46, f) != 46) break;
        if (rd32(cd) != 0x02014b50u) break;
        uint16_t method  = rd16(cd + 10);
        uint32_t csize   = rd32(cd + 20);
        uint32_t usize   = rd32(cd + 24);
        uint16_t nlen    = rd16(cd + 28);
        uint16_t elen    = rd16(cd + 30);
        uint16_t clen    = rd16(cd + 32);
        uint32_t loff    = rd32(cd + 42);
        if (nlen == 0 || nlen >= PKG_NAME_MAX) { p += 46u + nlen + elen + clen; continue; }
        char nm[PKG_NAME_MAX];
        if (fread(nm, 1, nlen, f) != nlen) break;
        nm[nlen] = 0;
        p += 46u + nlen + elen + clen;
        /* 只收 STORED + 大小一致 + 非目录 + 名字干净 */
        if (method != 0 || csize != usize || usize == 0) continue;
        if (usize > PKG_ENT_MAX_SIZE) continue;
        if (nm[nlen - 1] == '/') continue;
        if (nm[0] == '/' || strstr(nm, "..")) continue;
        if ((uint64_t)loff + 30u + usize > pkg_size) continue;
        strcpy(ents[n].name, nm);
        ents[n].local_off = loff;
        ents[n].size = usize;
        n++;
    }
    fclose(f);

    if (n == 0) { heap_caps_free(ents); return -7; }

    strncpy(pk->path, path, sizeof(pk->path) - 1);
    pk->path[sizeof(pk->path) - 1] = 0;
    pk->pkg_off = pkg_off;
    pk->pkg_size = pkg_size;
    pk->file_size = (uint32_t)fsz;
    pk->n = n;
    pk->ent = ents;
    pk->used = true;
    ESP_LOGI(TAG, "pkg %s: zip@%u+%u, %d 条目", path, (unsigned)pkg_off, (unsigned)pkg_size, n);
    return 0;
}

static pkg_t *pkg_alloc_slot(void)
{
    for (int i = 0; i < PKG_MAX; i++) if (!s_pkg[i].used) return &s_pkg[i];
    pkg_release(&s_pkg[0]);           /* 满了顶掉最旧的 (与 img 槽同策略) */
    return &s_pkg[0];
}

static pkg_t *pkg_from_index(int idx)
{
    if (idx < 1 || idx > PKG_MAX) return NULL;
    return s_pkg[idx - 1].used ? &s_pkg[idx - 1] : NULL;
}

static pkg_ent_t *pkg_find(pkg_t *pk, const char *name)
{
    for (int i = 0; i < pk->n; i++) {
        if (strcmp(pk->ent[i].name, name) == 0) return &pk->ent[i];
    }
    return NULL;
}

static int push_handle(lua_State *L, pkg_t *pk)
{
    int idx = (int)(pk - s_pkg) + 1;
    int *ud = (int *)lua_newuserdata(L, sizeof(int));
    *ud = idx;
    luaL_getmetatable(L, PKG_MT);
    lua_setmetatable(L, -2);
    return 1;                          /* userdata 已在栈顶 */
}

/* 从脚本路径截出所在目录 (guard 的 app_dir 要目录) */
static const char *app_dir_of_script(char *out, size_t cap)
{
    const char *sp = lua_sandbox_script_path();
    if (!sp || !sp[0]) return NULL;
    const char *slash = strrchr(sp, '/');
    size_t n = slash ? (size_t)(slash - sp) : 0;
    if (n == 0 || n >= cap) return NULL;
    memcpy(out, sp, n);
    out[n] = 0;
    return out;
}

/* ---- app.pkg.open(path) ---- */
static int l_pkg_open(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    char real[192], app_dir[192];
    const char *ad = app_dir_of_script(app_dir, sizeof(app_dir));
    /* 只读路径, 与 app.fs 同一套守卫 (数据目录 + 自身目录) */
    if (!lua_path_guard(lua_sandbox_data_dir(), ad, path, false, real, sizeof(real))) {
        ESP_LOGW(TAG, "pkg.open 拒绝路径: %s", path);
        lua_pushnil(L);
        return 1;
    }
    pkg_t *pk = pkg_alloc_slot();
    if (pkg_open_path(real, pk) != 0) { pkg_release(pk); lua_pushnil(L); return 1; }
    return push_handle(L, pk);
}

/* ---- app.pkg.self() ---- */
static int l_pkg_self(lua_State *L)
{
    const char *sp = lua_sandbox_script_path();
    if (!sp || !sp[0]) { lua_pushnil(L); return 1; }
    /* 已经开过就直接复用 (脚本每帧调 self() 也不会堆句柄) */
    for (int i = 0; i < PKG_MAX; i++) {
        if (s_pkg[i].used && strcmp(s_pkg[i].path, sp) == 0) return push_handle(L, &s_pkg[i]);
    }
    pkg_t *pk = pkg_alloc_slot();
    if (pkg_open_path(sp, pk) != 0) { pkg_release(pk); lua_pushnil(L); return 1; }
    return push_handle(L, pk);
}

static pkg_t *pk_arg(lua_State *L, int idx)
{
    int *ud = (int *)luaL_testudata(L, idx, PKG_MT);
    return ud ? pkg_from_index(*ud) : NULL;
}

/* ---- app.pkg.exists(h, name) ---- */
static int l_pkg_exists(lua_State *L)
{
    pkg_t *pk = pk_arg(L, 1);
    const char *name = luaL_checkstring(L, 2);
    lua_pushboolean(L, pk && pkg_find(pk, name) != NULL);
    return 1;
}

/* ---- app.pkg.size(h, name) ---- */
static int l_pkg_size(lua_State *L)
{
    pkg_t *pk = pk_arg(L, 1);
    const char *name = luaL_checkstring(L, 2);
    pkg_ent_t *e = pk ? pkg_find(pk, name) : NULL;
    if (!e) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)e->size);
    return 1;
}

/* ---- app.pkg.read(h, name) -> string ---- */
static int l_pkg_read(lua_State *L)
{
    pkg_t *pk = pk_arg(L, 1);
    const char *name = luaL_checkstring(L, 2);
    pkg_ent_t *e = pk ? pkg_find(pk, name) : NULL;
    if (!e) { lua_pushnil(L); return 1; }

    FILE *f = fopen(pk->path, "rb");
    if (!f) { lua_pushnil(L); return 1; }

    /* local header: 30 字节固定 + 名字 + extra —— 真实数据偏移要按它算 */
    uint8_t lh[30];
    uint32_t lp = pk->pkg_off + e->local_off;
    if (fseek(f, (long)lp, SEEK_SET) != 0 || fread(lh, 1, 30, f) != 30 ||
        rd32(lh) != 0x04034b50u) {
        fclose(f); lua_pushnil(L); return 1;
    }
    uint16_t nlen = rd16(lh + 26), elen = rd16(lh + 28);
    uint64_t data = (uint64_t)lp + 30u + nlen + elen;
    /* 边界: 必须整段落在 zip 数据区内 */
    if (data + e->size > (uint64_t)pk->pkg_off + pk->pkg_size ||
        data + e->size > pk->file_size) {
        ESP_LOGW(TAG, "pkg.read 越界: %s", name);
        fclose(f); lua_pushnil(L); return 1;
    }
    char *buf = heap_caps_malloc(e->size, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); lua_pushnil(L); return 1; }
    bool ok = (fseek(f, (long)data, SEEK_SET) == 0) && (fread(buf, 1, e->size, f) == e->size);
    fclose(f);
    if (!ok) { heap_caps_free(buf); lua_pushnil(L); return 1; }
    lua_pushlstring(L, buf, e->size);   /* 拷进 Lua 字符串, 随即释放临时缓冲 */
    heap_caps_free(buf);
    return 1;
}

/* ---- app.pkg.list(h) -> {name=size} ---- */
static int l_pkg_list(lua_State *L)
{
    pkg_t *pk = pk_arg(L, 1);
    lua_newtable(L);
    if (!pk) return 1;
    for (int i = 0; i < pk->n; i++) {
        lua_pushinteger(L, (lua_Integer)pk->ent[i].size);
        lua_setfield(L, -2, pk->ent[i].name);
    }
    return 1;
}

/* ---- app.pkg.close(h) ---- */
static int l_pkg_close(lua_State *L)
{
    pkg_t *pk = pk_arg(L, 1);
    if (pk) pkg_release(pk);
    return 0;
}

/* __gc: 句柄被回收时释放 (脚本忘了 close 也不会漏) */
static int l_pkg_gc(lua_State *L)
{
    int *ud = (int *)luaL_testudata(L, 1, PKG_MT);
    if (ud) {
        pkg_t *pk = pkg_from_index(*ud);
        if (pk) pkg_release(pk);
        *ud = 0;
    }
    return 0;
}

/* 应用退出时统一释放 (由 lua_bridge_reset_app_state 调) */
void lua_pkg_reset_all(void)
{
    for (int i = 0; i < PKG_MAX; i++) if (s_pkg[i].used) pkg_release(&s_pkg[i]);
}

void lua_mod_pkg_register(lua_State *L)
{
    luaL_newmetatable(L, PKG_MT);
    lua_pushcfunction(L, l_pkg_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_pkg_close);
    lua_setfield(L, -2, "__close");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, l_pkg_self);   lua_setfield(L, -2, "self");
    lua_pushcfunction(L, l_pkg_open);   lua_setfield(L, -2, "open");
    lua_pushcfunction(L, l_pkg_exists); lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, l_pkg_size);   lua_setfield(L, -2, "size");
    lua_pushcfunction(L, l_pkg_read);   lua_setfield(L, -2, "read");
    lua_pushcfunction(L, l_pkg_list);   lua_setfield(L, -2, "list");
    lua_pushcfunction(L, l_pkg_close);  lua_setfield(L, -2, "close");
    lua_setfield(L, -2, "pkg");
}
