/* luat_lib_store.c — LinTOS Lua 桥 · store 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"

static int l_nvs_get_int(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    nvs_handle_t h = lua_nvs_h();
    if (!h) return 0;
    int32_t v = 0;
    esp_err_t e = svc_nvs_get_i32(h, k, &v);
    svc_nvs_close(h);
    if (e != ESP_OK) return 0;
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

static int l_nvs_set_int(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    int32_t v = (int32_t)luaL_checkinteger(L, 2);
    nvs_handle_t h = lua_nvs_h();
    if (!h) { lua_pushboolean(L, false); return 1; }
    esp_err_t e = svc_nvs_set_i32(h, k, v);
    if (e == ESP_OK) svc_nvs_commit(h);
    svc_nvs_close(h);
    lua_pushboolean(L, e == ESP_OK);
    return 1;
}

static int l_nvs_get_str(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    nvs_handle_t h = lua_nvs_h();
    if (!h) return 0;
    size_t len = 0;
    if (svc_nvs_get_str(h, k, NULL, &len) != ESP_OK) { svc_nvs_close(h); return 0; }
    char *s = malloc(len + 1);
    esp_err_t e = svc_nvs_get_str(h, k, s, &len);
    svc_nvs_close(h);
    if (e != ESP_OK) { free(s); return 0; }
    lua_pushstring(L, s);
    free(s);
    return 1;
}

static int l_nvs_set_str(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    const char *v = luaL_checkstring(L, 2);
    nvs_handle_t h = lua_nvs_h();
    if (!h) { lua_pushboolean(L, false); return 1; }
    esp_err_t e = svc_nvs_set_str(h, k, v);
    if (e == ESP_OK) svc_nvs_commit(h);
    svc_nvs_close(h);
    lua_pushboolean(L, e == ESP_OK);
    return 1;
}


static int l_db_set(lua_State *L){
    const char *k=luaL_checkstring(L,1); const char *v=luaL_checkstring(L,2);
    lua_pushboolean(L, os_core_db_set(k,v)==0); return 1;
}

static int l_db_get(lua_State *L){
    const char *k=luaL_checkstring(L,1); char b[128];
    if(os_core_db_get(k,b,sizeof(b))<0){ lua_pushnil(L); return 1; }
    lua_pushstring(L,b); return 1;
}

static int l_db_del(lua_State *L){
    const char *k=luaL_checkstring(L,1); lua_pushboolean(L, os_core_db_del(k)==0); return 1;
}

static int l_db_seti(lua_State *L){
    const char *k=luaL_checkstring(L,1); char b[32]; snprintf(b,sizeof(b),"%lld",(long long)luaL_checkinteger(L,2));
    lua_pushboolean(L, os_core_db_set(k,b)==0); return 1;
}

static int l_db_geti(lua_State *L){
    const char *k=luaL_checkstring(L,1); long long def=(long long)luaL_optinteger(L,2,0); char b[32];
    if(os_core_db_get(k,b,sizeof(b))<0){ lua_pushinteger(L,(lua_Integer)def); return 1;}
    lua_pushinteger(L,(lua_Integer)strtoll(b,NULL,10)); return 1;
}

void lua_mod_store_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_nvs_get_int);         lua_setfield(L, -2, "get_int");
    lua_pushcfunction(L, l_nvs_set_int);         lua_setfield(L, -2, "set_int");
    lua_pushcfunction(L, l_nvs_get_str);         lua_setfield(L, -2, "get_str");
    lua_pushcfunction(L, l_nvs_set_str);         lua_setfield(L, -2, "set_str");
    lua_setfield(L, -2, "nvs");
    lua_newtable(L);
    lua_pushcfunction(L, l_db_set);              lua_setfield(L, -2, "set");
    lua_pushcfunction(L, l_db_get);              lua_setfield(L, -2, "get");
    lua_pushcfunction(L, l_db_del);              lua_setfield(L, -2, "del");
    lua_pushcfunction(L, l_db_seti);             lua_setfield(L, -2, "seti");
    lua_pushcfunction(L, l_db_geti);             lua_setfield(L, -2, "geti");
    lua_setfield(L, -2, "db");
}
