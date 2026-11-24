/* luat_lib_wiki.c — LinTOS Lua 桥 · wiki 模块（离线维基）
 *
 * 分工：Lua 应用负责界面/按键/滚动；这里只提供 Lua 干不了的活 ——
 *       SD 随机读、80 字节索引二分、LZ4 解压、HTML→纯文本、按 16px 折行。
 *
 * 契约（同其它 luat_lib_*.c）：只往栈顶的 app 表写字段。
 */
#include "lua_bridge.h"
#include "wiki_db.h"
#include "sd_scan.h"   /* sd_wait_ready: 卡挂载是异步的, 打开库前先等它就绪 */

/* 把 wiki_load 的负错误码翻译成人话 */
static const char *wiki_err(int rc) {
    switch (rc) {
        case -1: return "没有打开维基库";
        case -2: return "文章编号越界";
        case -3: return "这一条构建时就坏了";
        case -4: return "块文件打不开（TF 卡还在吗）";
        case -5: return "帧头损坏（索引和数据对不上）";
        case -6: return "读取不完整";
        case -7: return "解压失败";
        case -8: return "内存不足";
        default: return "未知错误";
    }
}

/* app.wiki.open([dir]) -> 篇数 | nil,错误
 * dir 省略时自动找 /sdcard/wiki 下第一个库 */
static int l_wiki_open(lua_State *L) {
    const char *dir = luaL_optstring(L, 1, NULL);
    /* ★ 2026-10-24: 开机挂载异步, Lua 应用一进来就 open 会误判"没库"。先过门闸。 */
    sd_wait_ready(1200, true);
    int n = wiki_open(dir);
    if (n <= 0) {
        lua_pushnil(L);
        lua_pushstring(L, "没找到维基库（/sdcard/wiki/<库名>/metadata.json）");
        return 2;
    }
    lua_pushinteger(L, n);
    return 1;
}

/* app.wiki.info() -> {count=, name=} */
static int l_wiki_info(lua_State *L) {
    if (!wiki_is_open()) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, wiki_count());  lua_setfield(L, -2, "count");
    lua_pushstring(L, wiki_name());    lua_setfield(L, -2, "name");
    lua_pushinteger(L, wiki_lines());  lua_setfield(L, -2, "lines");   /* 当前文章行数 */
    return 1;
}

/* app.wiki.search(q[, max]) -> {{id=,title=},...}
 * 标题前缀匹配（大小写敏感，和索引排序一致） */
static int l_wiki_search(lua_State *L) {
    const char *q = luaL_checkstring(L, 1);
    int max = (int)luaL_optinteger(L, 2, 20);
    if (max <= 0) max = 1;
    if (max > 50) max = 50;                 /* 候选数组按 50 上限分配，别让脚本撑爆栈 */

    static wiki_hit_t hits[50];
    int n = wiki_search(q, hits, max);
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        lua_newtable(L);
        lua_pushinteger(L, hits[i].id);        lua_setfield(L, -2, "id");
        lua_pushstring(L, hits[i].title);     lua_setfield(L, -2, "title");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* app.wiki.title(id) -> string */
static int l_wiki_title(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    char buf[WIKI_TITLE_MAX];
    if (!wiki_title(id, buf, sizeof(buf))) { lua_pushnil(L); return 1; }
    lua_pushstring(L, buf);
    return 1;
}

/* app.wiki.search_py(q[, initials[, max]]) -> {{id=,title=},...}
 * initials 真 = 首字母（bj），假 = 全拼（beijing）。库没拼音索引时返回空表。 */
static int l_wiki_search_py(lua_State *L) {
    const char *q = luaL_checkstring(L, 1);
    int initials = (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) ? (lua_toboolean(L, 2) != 0) : 0;
    int max = (int)luaL_optinteger(L, 3, 20);
    if (max <= 0) max = 1;
    if (max > 50) max = 50;

    static wiki_hit_t hits[50];
    int n = wiki_search_py(q, initials, hits, max);
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        lua_newtable(L);
        lua_pushinteger(L, hits[i].id);       lua_setfield(L, -2, "id");
        lua_pushstring(L, hits[i].title);     lua_setfield(L, -2, "title");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* app.wiki.titles(from[, n]) -> {{id=,title=},...}  浏览列表用，一次读一段 */
static int l_wiki_titles(lua_State *L) {
    int from = (int)luaL_checkinteger(L, 1);
    int n = (int)luaL_optinteger(L, 2, 16);
    if (n <= 0) n = 1;
    if (n > 32) n = 32;

    static wiki_hit_t rows[32];
    int got = wiki_titles(from, n, rows);
    lua_newtable(L);
    for (int i = 0; i < got; i++) {
        lua_newtable(L);
        lua_pushinteger(L, rows[i].id);       lua_setfield(L, -2, "id");
        lua_pushstring(L, rows[i].title);     lua_setfield(L, -2, "title");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* app.wiki.load(id) -> 行数 | nil,错误 */
static int l_wiki_load(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    int n = wiki_load(id);
    if (n < 0) {
        lua_pushnil(L);
        lua_pushstring(L, wiki_err(n));
        return 2;
    }
    lua_pushinteger(L, n);
    return 1;
}

/* app.wiki.line(i) -> string   0 基 */
static int l_wiki_line(lua_State *L) {
    int i = (int)luaL_checkinteger(L, 1);
    int len = 0;
    const char *s = wiki_line(i, &len);
    lua_pushlstring(L, s, (size_t)len);       /* 行不是 NUL 结尾，必须带长度推 */
    return 1;
}

/* app.wiki.lines() -> int */
static int l_wiki_lines(lua_State *L) {
    lua_pushinteger(L, wiki_lines());
    return 1;
}

/* app.wiki.close() */
static int l_wiki_close(lua_State *L) {
    (void)L;
    wiki_close();
    return 0;
}

void lua_mod_wiki_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_wiki_open);     lua_setfield(L, -2, "open");
    lua_pushcfunction(L, l_wiki_info);     lua_setfield(L, -2, "info");
    lua_pushcfunction(L, l_wiki_search);   lua_setfield(L, -2, "search");
    lua_pushcfunction(L, l_wiki_title);    lua_setfield(L, -2, "title");
    lua_pushcfunction(L, l_wiki_search_py); lua_setfield(L, -2, "search_py");
    lua_pushcfunction(L, l_wiki_titles);   lua_setfield(L, -2, "titles");
    lua_pushcfunction(L, l_wiki_load);     lua_setfield(L, -2, "load");
    lua_pushcfunction(L, l_wiki_line);     lua_setfield(L, -2, "line");
    lua_pushcfunction(L, l_wiki_lines);    lua_setfield(L, -2, "lines");
    lua_pushcfunction(L, l_wiki_close);    lua_setfield(L, -2, "close");
    lua_setfield(L, -2, "wiki");
}
