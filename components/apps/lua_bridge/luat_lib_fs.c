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
    lua_pushboolean(L, access(p, F_OK) == 0);
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
    lua_pushcfunction(L, l_fs_write);            lua_setfield(L, -2, "write");
    lua_pushcfunction(L, l_fs_append);           lua_setfield(L, -2, "append");
    lua_pushcfunction(L, l_fs_remove);           lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, l_fs_listdir);          lua_setfield(L, -2, "listdir");
    lua_setfield(L, -2, "fs");
}
