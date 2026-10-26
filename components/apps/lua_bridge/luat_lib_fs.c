/* luat_lib_fs.c — LinTOS Lua 桥 · fs 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"


/* ---- app.fs.* : TF 卡文件存取 ---- */
static int l_fs_exists(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    bool ok = (access(p, F_OK) == 0);
    if (!ok) {
        /* ★ 2026-10-09: access() 走 VFS 时,**挂载点本身**(如 "/sdcard") 会被解析成空路径
         * 而失败 —— 于是 app.fs.exists("/sdcard") 恒为 false。后果: 地图程序 on_enter 里
         * 那句 `if not app.fs.exists(TIER0)` 永远成立 ⇒ 一直显示"未检测到 TF 卡",
         * 而 remcon `ls /sdcard` 却正常 (它用 opendir)。这里补 opendir 兜底。 */
        DIR *d = opendir(p);
        if (d) { closedir(d); ok = true; }
    }
    lua_pushboolean(L, ok);
    return 1;
}

/* app.fs.read 单次读取上限: 超过直接返回 nil, 避免一个大文件把堆吃光 */
#define LUA_FS_READ_MAX (512 * 1024)
static int l_fs_read(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > LUA_FS_READ_MAX) { fclose(f); return 0; }
    /* 读缓冲优先走 PSRAM: 大脚本/素材不该占用宝贵的内部 RAM */
    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)sz + 1);   /* PSRAM 不足时退回内部 RAM */
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    lua_pushlstring(L, buf, rd);
    heap_caps_free(buf);
    return 1;
}

static int l_fs_write(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    size_t len; const char *d = luaL_checklstring(L, 2, &len);
    FILE *f = fopen(p, "wb");
    if (!f) { lua_pushboolean(L, false); return 1; }
    bool ok = fwrite(d, 1, len, f) == len;
    fclose(f);
    lua_pushboolean(L, ok);
    return 1;
}

static int l_fs_append(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    size_t len; const char *d = luaL_checklstring(L, 2, &len);
    FILE *f = fopen(p, "ab");
    if (!f) { lua_pushboolean(L, false); return 1; }
    bool ok = fwrite(d, 1, len, f) == len;
    fclose(f);
    lua_pushboolean(L, ok);
    return 1;
}

static int l_fs_remove(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    lua_pushboolean(L, remove(p) == 0);
    return 1;
}

/* app.fs.read_at(path, off, len) -> 字符串 | nil
 *
 * ★ 为什么必须有它 (省级地图包的基石)
 *   `read_at` 之前只有"整文件读", 于是地图只能做成"一瓦片一个文件" ——
 *   示例省 = 607 个道路块 + 564 个地标块 = **1171 个文件 / 1171 次 HTTP 请求**。
 *   设备端一个一个下, 二十分钟都下不完。
 *   有了 read_at: 整省打成一个 18 MB 的 .bin, 索引里记每块的 (off,size),
 *   设备端精确读当前视口那几块 —— **一次请求下完整个省**, 内存只驻留几块。
 *   同样也是"大文件按需读"的唯一手段 (整文件读有 512 KB 上限, 而 bin 有 18 MB)。
 *
 * ⚠ 上限比 read 大 (1 MB): 单块最大 364 KB, 再留余量给将来的包。
 *   FATFS 的 fseek 是索引式的, 不像 SPIFFS 那样每次 seek 都扫目录, 可以放心用。 */
#define LUA_FS_READ_AT_MAX (1024 * 1024)
/* app.fs.rename(old, new) -> bool
 * 改名/移动。录音机改文件名、整理目录都用它。 */
static int l_fs_rename(lua_State *L) {
    const char *from = luaL_checkstring(L, 1);
    const char *to   = luaL_checkstring(L, 2);
    lua_pushboolean(L, rename(from, to) == 0);
    return 1;
}

static int l_fs_read_at(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    lua_Integer len = luaL_checkinteger(L, 3);
    if (off < 0 || len <= 0 || len > LUA_FS_READ_AT_MAX) return 0;
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return 0; }
    char *buf = heap_caps_malloc((size_t)len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = 0;
    lua_pushlstring(L, buf, rd);
    heap_caps_free(buf);
    return 1;
}

/* app.fs.size(path) -> 字节数 | nil
 * 读之前先问大小: 分块地图的最小/最大块判据、下载落地自检都要用它 */
static int l_fs_size(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    struct stat st;
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    lua_pushinteger(L, (lua_Integer)st.st_size);
    return 1;
}

/* app.fs.mkdir(path) -> bool
 * ★ 会**逐级创建**父目录 (等价 mkdir -p): 分块地图下载要落到 /sdcard/Map/tiles/,
 *   而 Lua 侧没有别的建目录手段 —— 没有它整个分层目录方案就落不了地。
 *   已存在也返回 true (幂等), 免得调用方每次都要先 exists() 判一遍。 */
static int l_fs_mkdir(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    char tmp[256];
    size_t n = strlen(p);
    if (n == 0 || n >= sizeof(tmp)) { lua_pushboolean(L, false); return 1; }
    memcpy(tmp, p, n + 1);
    for (char *q = tmp + 1; *q; q++) {           /* 从第 2 个字符起, 跳过根 '/' */
        if (*q != '/') continue;
        *q = 0;
        mkdir(tmp, 0777);                        /* 失败无所谓: 多半是已存在 */
        *q = '/';
    }
    lua_pushboolean(L, mkdir(tmp, 0777) == 0 || errno == EEXIST);
    return 1;
}

static int l_fs_listdir(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    DIR *d = opendir(p);
    if (!d) return 0;
    lua_newtable(L);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        n++; lua_pushlstring(L, e->d_name, strlen(e->d_name));
        lua_rawseti(L, -2, n);
    }
    closedir(d);
    return 1;
}

void lua_mod_fs_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_fs_exists);           lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, l_fs_read);             lua_setfield(L, -2, "read");
    lua_pushcfunction(L, l_fs_read_at);          lua_setfield(L, -2, "read_at");
    lua_pushcfunction(L, l_fs_write);            lua_setfield(L, -2, "write");
    lua_pushcfunction(L, l_fs_append);           lua_setfield(L, -2, "append");
    lua_pushcfunction(L, l_fs_remove);           lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, l_fs_rename);           lua_setfield(L, -2, "rename");
    lua_pushcfunction(L, l_fs_size);             lua_setfield(L, -2, "size");
    lua_pushcfunction(L, l_fs_mkdir);            lua_setfield(L, -2, "mkdir");
    lua_pushcfunction(L, l_fs_listdir);          lua_setfield(L, -2, "listdir");
    lua_setfield(L, -2, "fs");
}
