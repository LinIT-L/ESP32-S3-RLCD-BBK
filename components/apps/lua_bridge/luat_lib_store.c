/* luat_lib_store.c — LinTOS Lua 桥 · store 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ============================================================================
 * ★ 2026-10-26 linit「避免频繁写 NVS」: Lua 侧 NVS 写节流
 *
 * 问题: app.nvs.set_int/set_str 与 app.db.set/seti 都是**每次调用立刻 open+set+commit**,
 *   写 NVS 期间 flash cache 被关, 从 flash 执行的代码停顿十几~几十 ms。
 *   某个 Lua 应用在循环里写(比如每帧存一次分数), 就会反复掉帧, 而且白耗 flash 擦写。
 *
 * 做法: 同一 (命名空间, key) **1 秒窗口内只落盘一次**; 窗口内的后续写入只更新内存里的
 *   最新值(后写覆盖先写), 由一次性定时器在 1 秒后统一落盘。
 *   - 首次写该 key (本次开机) 立刻落盘, 不延迟
 *   - 读之前先把这个 key 的待落盘值刷下去 ⇒ **读到的永远是自己刚写的值**
 *   - 8 个槽位, 满了淘汰最久没写的那条(淘汰前先落盘, 不丢数据)
 *   - 定时器是 esp_timer, 应用退出/Lua 状态销毁后仍会把待写值落完
 * ⚠ 唯一风险窗口: 写完后 **1 秒内** 直接断电/进深睡, 最后一次写会丢。
 * ==========================================================================*/
#define LNS_WIN_US     (1000 * 1000)   /* 节流窗口: 1s */
#define LNS_SLOTS      8
#define LNS_KEY_MAX    16              /* NVS key 上限 15 字符 + NUL */
#define LNS_NS_LUAAPP  0
#define LNS_NS_COREDB  1

typedef struct {
    uint8_t  used;
    uint8_t  is_str;
    uint8_t  pending;   /* 有未落盘的新值 */
    uint8_t  ns;
    char     key[LNS_KEY_MAX];
    int32_t  ival;
    char    *sval;      /* is_str 时有效 (malloc) */
    int64_t  last_us;   /* 上次真正落盘的时刻 (0=本次开机还没写过) */
} lns_slot_t;

static lns_slot_t        s_lns[LNS_SLOTS];
static esp_timer_handle_t s_lns_timer;

static const char *lns_ns_name(uint8_t ns) { return ns ? "core_db" : "luaapp"; }

static bool lns_write(lns_slot_t *s)
{
    nvs_handle_t h;
    if (svc_nvs_open(lns_ns_name(s->ns), NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = s->is_str ? svc_nvs_set_str(h, s->key, s->sval ? s->sval : "")
                            : svc_nvs_set_i32(h, s->key, s->ival);
    if (e == ESP_OK) e = svc_nvs_commit(h);
    svc_nvs_close(h);
    if (e == ESP_OK) { s->pending = 0; s->last_us = esp_timer_get_time(); }
    return e == ESP_OK;
}

static void lns_timer_cb(void *arg)
{
    (void)arg;
    for (int i = 0; i < LNS_SLOTS; i++)
        if (s_lns[i].used && s_lns[i].pending) lns_write(&s_lns[i]);
}

static void lns_arm_timer(void)
{
    if (!s_lns_timer) {
        const esp_timer_create_args_t a = { .callback = lns_timer_cb, .name = "lns_flush" };
        if (esp_timer_create(&a, &s_lns_timer) != ESP_OK) { s_lns_timer = NULL; return; }
    }
    esp_timer_stop(s_lns_timer);                              /* 可能没启动, 忽略返回值 */
    esp_timer_start_once(s_lns_timer, LNS_WIN_US);
}

static lns_slot_t *lns_slot_find(uint8_t ns, const char *key)
{
    for (int i = 0; i < LNS_SLOTS; i++)
        if (s_lns[i].used && s_lns[i].ns == ns && strncmp(s_lns[i].key, key, LNS_KEY_MAX - 1) == 0)
            return &s_lns[i];
    return NULL;
}

/* 取槽位: 已有 → 复用; 否则找空位; 再否则淘汰最久没写的 (先落盘) */
static lns_slot_t *lns_slot_take(uint8_t ns, const char *key, bool is_str)
{
    lns_slot_t *s = lns_slot_find(ns, key);
    if (s) { s->is_str = is_str ? 1 : 0; return s; }

    s = NULL;
    for (int i = 0; i < LNS_SLOTS; i++) if (!s_lns[i].used) { s = &s_lns[i]; break; }
    if (!s) {                                   /* 满了: 淘汰最久没写的 */
        s = &s_lns[0];
        for (int i = 1; i < LNS_SLOTS; i++) if (s_lns[i].last_us < s->last_us) s = &s_lns[i];
        if (s->pending) lns_write(s);            /* 淘汰前先落盘, 不丢数据 */
        free(s->sval);
    }
    memset(s, 0, sizeof(*s));
    s->used  = 1;
    s->ns    = ns;
    s->is_str = is_str ? 1 : 0;
    snprintf(s->key, sizeof(s->key), "%s", key);
    return s;
}

/* 核心入口: 同 key 1s 内只落盘一次 */
static bool lns_put(uint8_t ns, const char *key, bool is_str, int32_t iv, const char *sv)
{
    if (!key || !key[0]) return false;
    if (strlen(key) > LNS_KEY_MAX - 1) return false;   /* 超长 key NVS 本身也不接受 */

    lns_slot_t *s = lns_slot_take(ns, key, is_str);
    if (!s) return false;

    if (is_str) {
        char *nv = strdup(sv ? sv : "");
        if (!nv) return false;
        free(s->sval);
        s->sval = nv;
    } else {
        s->ival = iv;
    }

    int64_t now = esp_timer_get_time();
    if (s->last_us == 0 || now - s->last_us >= LNS_WIN_US) return lns_write(s);   /* 窗口外: 立刻落盘 */
    s->pending = 1;                                                              /* 窗口内: 只留最新值 */
    lns_arm_timer();
    return true;
}

/* 读之前把该 key 的待落盘值刷下去 ⇒ 读到的永远是自己刚写的值 */
static void lns_flush_key(uint8_t ns, const char *key)
{
    if (!key) return;
    lns_slot_t *s = lns_slot_find(ns, key);
    if (s && s->pending) lns_write(s);
}

static int l_nvs_get_int(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    lns_flush_key(LNS_NS_LUAAPP, k);          /* 有未落盘的新值先写下去, 保证读到自己刚写的 */
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
    lua_pushboolean(L, lns_put(LNS_NS_LUAAPP, k, false, v, NULL));   /* ★ 1s 内同 key 只落盘一次 */
    return 1;
}

static int l_nvs_get_str(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    lns_flush_key(LNS_NS_LUAAPP, k);
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
    lua_pushboolean(L, lns_put(LNS_NS_LUAAPP, k, true, 0, v));       /* ★ 同上 */
    return 1;
}


static int l_db_set(lua_State *L){
    const char *k=luaL_checkstring(L,1); const char *v=luaL_checkstring(L,2);
    lua_pushboolean(L, lns_put(LNS_NS_COREDB, k, true, 0, v));       /* ★ 同上 (core_db) */
    return 1;
}

static int l_db_get(lua_State *L){
    const char *k=luaL_checkstring(L,1); char b[128];
    lns_flush_key(LNS_NS_COREDB, k);
    if(os_core_db_get(k,b,sizeof(b))<0){ lua_pushnil(L); return 1; }
    lua_pushstring(L,b); return 1;
}

static int l_db_del(lua_State *L){
    const char *k=luaL_checkstring(L,1); lua_pushboolean(L, os_core_db_del(k)==0); return 1;
}

static int l_db_seti(lua_State *L){
    const char *k=luaL_checkstring(L,1); char b[32]; snprintf(b,sizeof(b),"%lld",(long long)luaL_checkinteger(L,2));
    lua_pushboolean(L, lns_put(LNS_NS_COREDB, k, true, 0, b));       /* ★ 同上 (core_db) */
    return 1;
}

static int l_db_geti(lua_State *L){
    const char *k=luaL_checkstring(L,1); long long def=(long long)luaL_optinteger(L,2,0); char b[32];
    lns_flush_key(LNS_NS_COREDB, k);
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
