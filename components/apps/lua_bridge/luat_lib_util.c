/* luat_lib_util.c — LinTOS Lua 桥 · util 模块 (通用工具)
 *
 * 定位: 给后续大量 Lua 应用预留的"瑞士军刀"。现在各应用各写一遍的循环、
 *       随机、存档序列化、校验, 统一收到这里, 行为一致、速度是 C。
 * 参考: LuatOS 的 rtos.timer / crypto / pack · Pico-8 的 rnd/flr/mid/clamp ·
 *       TIC-80 的 utility 集。只取 ESP32-S3 上**真的跑得动且常用**的那部分。
 *
 * 契约同其它 luat_lib_*: 只通过 lua_setfield(L, -2, "名字") 往栈顶的 app 表写字段。
 *
 * ⚠ 本项目 Lua 是 LUA_32BITS=1 (lua_Integer 是 32 位, 浮点 7 位有效数字):
 *   - 不要把 32 位无符号数直接 pushinteger (crc32 会变负数) → 一律**十六进制字符串**
 *   - 时间戳 1700000000 超过 2^24 的精度范围 → epoch() 返回字符串 "1700000000"
 */
#include "lua_bridge.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "mbedtls/base64.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static uint32_t u_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ============================ 定时器 ============================
 * app.util.after(ms, fn) → id     一次性
 * app.util.every(ms, fn) → id     周期
 * app.util.cancel(id)
 *
 * 实现: 每个槽存一个 Lua registry 引用 (luaL_ref), 由 page_lua.c 的 lua_poll()
 *       每帧驱动 lua_util_tick() —— 与应用自己的 poll() 同一拍, 不需要额外任务,
 *       也不占内部 RAM 栈 (只是 16 个 16 字节的结构)。
 * 上限 16 个: 够任何单文件应用用; 满了返回 nil (不是静默丢弃)。
 * ⚠ 回调在 Lua VM 里执行, 里面可以再 after/every/cancel, 但**不要写死循环**。 */
#define UTIL_TIMERS 16
static struct { int ref; uint32_t next, period; uint8_t used; } s_tmr[UTIL_TIMERS];

static int util_add(lua_State *L, int periodic)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms < 1) ms = 1;
    for (int i = 0; i < UTIL_TIMERS; i++) {
        if (s_tmr[i].used) continue;
        lua_pushvalue(L, 2);
        s_tmr[i].ref    = luaL_ref(L, LUA_REGISTRYINDEX);
        s_tmr[i].next   = u_now_ms() + (uint32_t)ms;
        s_tmr[i].period = periodic ? (uint32_t)ms : 0;
        s_tmr[i].used   = 1;
        lua_pushinteger(L, i + 1);       /* 1-based, 0 留给"没拿到" */
        return 1;
    }
    lua_pushnil(L);
    return 1;
}
static int l_after(lua_State *L)  { return util_add(L, 0); }
static int l_every(lua_State *L)  { return util_add(L, 1); }

static int l_cancel(lua_State *L)
{
    int id = (int)luaL_checkinteger(L, 1) - 1;
    if (id < 0 || id >= UTIL_TIMERS || !s_tmr[id].used) return 0;
    luaL_unref(L, LUA_REGISTRYINDEX, s_tmr[id].ref);
    s_tmr[id].used = 0;
    return 0;
}

/* 由 page_lua.c 每帧调用。L 必须与注册时的 lua_State 是同一个 (单 VM, 天然满足)。 */void lua_util_tick(lua_State *L)
{
    if (!L) return;
    /* ★ 换了 VM (上一个应用退出 / 重新加载) ⇒ 旧 ref 全部作废, 清零重来。
     *   放在这里而不是 lua_stop(): page_lua.c 有 6 处 lua_close(), 企不到每处都清;
     *   而这个函数每帧必调, 是唯一可靠的"发现换了 VM"的地方。 */
    static lua_State *s_last_L = NULL;
    if (L != s_last_L) { lua_util_reset(); s_last_L = L; }
    uint32_t n = u_now_ms();
    for (int i = 0; i < UTIL_TIMERS; i++) {
        if (!s_tmr[i].used) continue;
        if ((int32_t)(n - s_tmr[i].next) < 0) continue;
        int ref = s_tmr[i].ref;
        if (s_tmr[i].period) {
            s_tmr[i].next = n + s_tmr[i].period;
        } else {
            s_tmr[i].used = 0;                  /* 先失效, 回调里再 cancel 也不会二次释放 */
        }
        int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) lua_pop(L, 1);   /* 出错静默丢弃, 免得刷屏 */
        lua_settop(L, top);
        if (!s_tmr[i].period) luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
}

/* 应用退出时清空所有定时器。
 * ⚠ 不需要 luaL_unref: lua_close() 会把整个 registry 一起销毁, 这里只清标志,
 *   防止**下一个应用**继承上一轮留下的 ref (指向已销毁的 registry 槽 → 崩)。 */
void lua_util_reset(void)
{
    for (int i = 0; i < UTIL_TIMERS; i++) s_tmr[i].used = 0;
}

/* ============================ 随机 ============================
 * app.util.rand([min,]max) → 整数    rand() 无参 = 1..100; rand(n) = 1..n
 * app.util.randf()         → [0,1) 浮点
 * 硬件 TRNG (esp_random), 不用播种。 */
static int l_rand(lua_State *L)
{
    int n = lua_gettop(L);
    lua_Integer lo = 1, hi = 100;
    if (n >= 2)      { lo = luaL_checkinteger(L, 1); hi = luaL_checkinteger(L, 2); }
    else if (n == 1) { hi = luaL_checkinteger(L, 1); }
    if (hi < lo) { lua_Integer t = lo; lo = hi; hi = t; }
    uint32_t span = (uint32_t)(hi - lo) + 1u;
    lua_pushinteger(L, lo + (lua_Integer)(esp_random() % span));
    return 1;
}
static int l_randf(lua_State *L)
{
    lua_pushnumber(L, (lua_Number)(esp_random() & 0xFFFFFF) / 16777216.0);
    return 1;
}

/* ============================ 校验 / 编码 ============================
 * app.util.crc32(s)    → 8 位十六进制字符串 (如 "e413aa54")
 * app.util.b64(s)      → base64 串
 * app.util.unb64(s)    → 原始串 | nil(格式错)
 * 存档校验 / 接口签名 / 传输前的紧凑编码都在这。 */
static int l_crc32(lua_State *L)
{
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    uint32_t c = esp_rom_crc32_le(0, (const uint8_t *)s, len);
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", (unsigned)c);
    lua_pushstring(L, buf);
    return 1;
}
static int l_b64(lua_State *L)
{
    size_t len = 0;
    const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &len);
    size_t olen = 0;
    if (mbedtls_base64_encode(NULL, 0, &olen, s, len) != 0 || olen == 0) { lua_pushnil(L); return 1; }
    char *out = (char *)malloc(olen + 1);
    if (!out) { lua_pushnil(L); return 1; }
    if (mbedtls_base64_encode((unsigned char *)out, olen + 1, &olen, s, len) != 0) {
        free(out); lua_pushnil(L); return 1;
    }
    lua_pushlstring(L, out, olen);
    free(out);
    return 1;
}
static int l_unb64(lua_State *L)
{
    size_t len = 0;
    const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &len);
    size_t olen = 0;
    if (mbedtls_base64_decode(NULL, 0, &olen, s, len) != 0 || olen == 0) { lua_pushnil(L); return 1; }
    unsigned char *out = (unsigned char *)malloc(olen + 1);
    if (!out) { lua_pushnil(L); return 1; }
    if (mbedtls_base64_decode(out, olen + 1, &olen, s, len) != 0) {
        free(out); lua_pushnil(L); return 1;
    }
    lua_pushlstring(L, (const char *)out, olen);
    free(out);
    return 1;
}

/* ============================ 序列化 ============================
 * app.util.serialize(t)     → 字符串   (nil/bool/number/string/table, 不支持环)
 * app.util.unserialize(s)   → 值 | nil
 * 比 json 省事: 存档直接一行写完, 读回来就是原来的表 (数组下标、负数键都保得住)。
 * 格式示例: {1,2,"abc",k=3.5,n={true}}
 * ⚠ 只用于**自己写的存档**, 不要拿它解析外部不可信数据。 */
#define SER_MAX_DEPTH 16

static void ser_val(lua_State *L, luaL_Buffer *b, int idx, int depth);

static void ser_str(lua_State *L, luaL_Buffer *b, int idx)
{
    size_t n = 0;
    const char *s = lua_tolstring(L, idx, &n);
    luaL_addchar(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { luaL_addchar(b, '\\'); luaL_addchar(b, (char)c); }
        else if (c == '\n') luaL_addstring(b, "\\n");
        else if (c == '\r') luaL_addstring(b, "\\r");
        else if (c == '\t') luaL_addstring(b, "\\t");
        else luaL_addchar(b, (char)c);
    }
    luaL_addchar(b, '"');
}

static void ser_tab(lua_State *L, luaL_Buffer *b, int idx, int depth)
{
    if (depth > SER_MAX_DEPTH) { luaL_addstring(b, "nil"); return; }
    luaL_addchar(b, '{');
    int first = 1;
    /* 数组部分: 1..n 连续 */
    lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
    for (lua_Integer i = 1; i <= n; i++) {
        if (!first) luaL_addchar(b, ',');
        first = 0;
        lua_rawgeti(L, idx, (lua_Integer)i);
        ser_val(L, b, lua_gettop(L), depth + 1);
        lua_pop(L, 1);
    }
    /* 散列部分: 跳过数组段 */
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        int vt = lua_gettop(L);
        int kt = vt - 1;
        bool skip = false;
        if (lua_type(L, kt) == LUA_TNUMBER) {
            lua_Number kn = lua_tonumber(L, kt);
            if (kn >= 1 && kn <= (lua_Number)n && (lua_Number)(lua_Integer)kn == kn) skip = true;
        }
        if (!skip) {
            if (!first) luaL_addchar(b, ',');
            first = 0;
            if (lua_type(L, kt) == LUA_TSTRING) {
                ser_str(L, b, kt);
            } else if (lua_type(L, kt) == LUA_TNUMBER) {
                char tmp[40];
                snprintf(tmp, sizeof(tmp), "[%lld]", (long long)lua_tointeger(L, kt));
                luaL_addstring(b, tmp);
            } else {
                luaL_addstring(b, "[nil]");   /* 表/函数当键不支持 */
            }
            luaL_addchar(b, '=');
            ser_val(L, b, vt, depth + 1);
        }
        lua_pop(L, 1);   /* 弹出 value, 保留 key 给 next */
    }
    luaL_addchar(b, '}');
}

static void ser_val(lua_State *L, luaL_Buffer *b, int idx, int depth)
{
    switch (lua_type(L, idx)) {
    case LUA_TNIL:     luaL_addstring(b, "nil"); break;
    case LUA_TBOOLEAN: luaL_addstring(b, lua_toboolean(L, idx) ? "true" : "false"); break;
    case LUA_TNUMBER: {
        char tmp[40];
        lua_Number v = lua_tonumber(L, idx);
        if (v == (lua_Number)(lua_Integer)v) snprintf(tmp, sizeof(tmp), "%lld", (long long)lua_tointeger(L, idx));
        else                                  snprintf(tmp, sizeof(tmp), "%.7g", (double)v);
        luaL_addstring(b, tmp);
        break;
    }
    case LUA_TSTRING: ser_str(L, b, idx); break;
    case LUA_TTABLE:  ser_tab(L, b, idx, depth); break;
    default:          luaL_addstring(b, "nil"); break;
    }
}

static int l_serialize(lua_State *L)
{
    luaL_checkany(L, 1);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    ser_val(L, &b, 1, 0);
    luaL_pushresult(&b);
    return 1;
}

/* 解析器: 递归下降, 出错整串返回 nil (不抛异常, 存档坏了应用自己能兜底) */
typedef struct { const char *p, *e; int ok; } sp_t;
static void sp_ws(sp_t *s) { while (s->p < s->e && (*s->p == ' ' || *s->p == '\n' || *s->p == '\t')) s->p++; }
static int sp_val(lua_State *L, sp_t *s, int depth);

static int sp_str(lua_State *L, sp_t *s)
{
    if (s->p >= s->e || *s->p != '"') { s->ok = 0; return 0; }
    s->p++;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    while (s->p < s->e && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\' && s->p < s->e) {
            char n = *s->p++;
            if      (n == 'n') luaL_addchar(&b, '\n');
            else if (n == 'r') luaL_addchar(&b, '\r');
            else if (n == 't') luaL_addchar(&b, '\t');
            else               luaL_addchar(&b, n);
        } else {
            luaL_addchar(&b, c);
        }
    }
    if (s->p >= s->e) { s->ok = 0; return 0; }
    s->p++;                                  /* 收尾的 " */
    luaL_pushresult(&b);
    return 1;
}

static int sp_tab(lua_State *L, sp_t *s, int depth)
{
    if (depth > SER_MAX_DEPTH) { s->ok = 0; return 0; }
    if (s->p >= s->e || *s->p != '{') { s->ok = 0; return 0; }
    s->p++;
    lua_newtable(L);
    int arr = 1;
    for (;;) {
        sp_ws(s);
        if (s->p >= s->e) { s->ok = 0; return 1; }
        if (*s->p == '}') { s->p++; return 1; }
        if (*s->p == ',') { s->p++; continue; }
        bool has_key = false;
        if (*s->p == '[') {                  /* [key]= */
            s->p++;
            sp_ws(s);
            int isnum = (*s->p >= '0' && *s->p <= '9') || *s->p == '-';
            if (isnum) {
                char *end = NULL;
                long long k = strtoll(s->p, &end, 10);
                if (!end || end == s->p) { s->ok = 0; return 1; }
                s->p = end;
                sp_ws(s);
                if (s->p >= s->e || *s->p != ']') { s->ok = 0; return 1; }
                s->p++;
                if ((int)k != arr) { lua_pushinteger(L, (lua_Integer)k); has_key = true; }
            } else {
                sp_val(L, s, depth + 1);     /* 字符串键 */
                if (!s->ok) return 1;
                has_key = true;
            }
            sp_ws(s);
            if (s->p >= s->e || *s->p != '=') { s->ok = 0; return 1; }
            s->p++;
        } else if ((*s->p >= 'A' && *s->p <= 'Z') || (*s->p >= 'a' && *s->p <= 'z') || *s->p == '_') {
            /* 裸标识符键: name= */
            const char *st = s->p;
            while (s->p < s->e && ((*s->p >= 'A' && *s->p <= 'Z') || (*s->p >= 'a' && *s->p <= 'z') ||
                                   (*s->p >= '0' && *s->p <= '9') || *s->p == '_')) s->p++;
            lua_pushlstring(L, st, (size_t)(s->p - st));
            has_key = true;
            sp_ws(s);
            if (s->p >= s->e || *s->p != '=') { s->ok = 0; return 1; }
            s->p++;
        }
        sp_val(L, s, depth + 1);
        if (!s->ok) return 1;
        if (has_key) lua_rawset(L, -3);
        else         lua_rawseti(L, -2, arr++);
    }
}

static int sp_val(lua_State *L, sp_t *s, int depth)
{
    sp_ws(s);
    if (s->p >= s->e) { s->ok = 0; return 0; }
    if (*s->p == '{') return sp_tab(L, s, depth);
    if (*s->p == '"') return sp_str(L, s);
    if (!strncmp(s->p, "nil", 3))   { s->p += 3; lua_pushnil(L);     return 1; }
    if (!strncmp(s->p, "true", 4))  { s->p += 4; lua_pushboolean(L, 1); return 1; }
    if (!strncmp(s->p, "false", 5)) { s->p += 5; lua_pushboolean(L, 0); return 1; }
    {
        char *end = NULL;
        double v = strtod(s->p, &end);
        if (!end || end == s->p) { s->ok = 0; return 0; }
        s->p = end;
        lua_pushnumber(L, (lua_Number)v);
        return 1;
    }
}

static int l_unserialize(lua_State *L)
{
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    sp_t st = { s, s + len, 1 };
    int base = lua_gettop(L);
    if (!sp_val(L, &st, 0) || !st.ok) {
        lua_settop(L, base);
        lua_pushnil(L);
        return 1;
    }
    return 1;
}

/* ============================ 时间 ============================
 * app.util.epoch() → "1700000000" (字符串, 32 位整数装不下 10 位数)
 * app.util.sleep(ms) → 让出 CPU 指定毫秒 (不要用在大循环里, 会掉帧) */
static int l_epoch(lua_State *L)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", (long long)time(NULL));
    lua_pushstring(L, buf);
    return 1;
}
static int l_sleep(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms > 0 && ms < 60000) vTaskDelay(pdMS_TO_TICKS((uint32_t)ms));
    return 0;
}

/* ============================ 游戏常用数学 ============================
 * app.util.clamp(v,lo,hi) / lerp(a,b,t) / dist(x1,y1,x2,y2) / round(v) / sign(v)
 * 这些用 Lua 写也不难, 但每个游戏都要写一遍且容易写错边界, 所以收到 C。 */
static int l_clamp(lua_State *L)
{
    lua_Number v = luaL_checknumber(L, 1);
    lua_Number lo = luaL_checknumber(L, 2);
    lua_Number hi = luaL_checknumber(L, 3);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    lua_pushnumber(L, v);
    return 1;
}
static int l_lerp(lua_State *L)
{
    lua_Number a = luaL_checknumber(L, 1);
    lua_Number b = luaL_checknumber(L, 2);
    lua_Number t = luaL_checknumber(L, 3);
    lua_pushnumber(L, a + (b - a) * t);
    return 1;
}
static int l_dist(lua_State *L)
{
    lua_Number x1 = luaL_checknumber(L, 1), y1 = luaL_checknumber(L, 2);
    lua_Number x2 = luaL_checknumber(L, 3), y2 = luaL_checknumber(L, 4);
    lua_Number dx = x2 - x1, dy = y2 - y1;
    lua_pushnumber(L, (lua_Number)sqrt((double)(dx * dx + dy * dy)));
    return 1;
}
static int l_round(lua_State *L)
{
    lua_Number v = luaL_checknumber(L, 1);
    lua_pushinteger(L, (lua_Integer)(v < 0 ? v - 0.5 : v + 0.5));
    return 1;
}
static int l_sign(lua_State *L)
{
    lua_Number v = luaL_checknumber(L, 1);
    lua_pushinteger(L, v > 0 ? 1 : (v < 0 ? -1 : 0));
    return 1;
}

/* ============================ 注册 ============================ */
void lua_mod_util_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_after);      lua_setfield(L, -2, "after");
    lua_pushcfunction(L, l_every);      lua_setfield(L, -2, "every");
    lua_pushcfunction(L, l_cancel);     lua_setfield(L, -2, "cancel");
    lua_pushcfunction(L, l_rand);       lua_setfield(L, -2, "rand");
    lua_pushcfunction(L, l_randf);      lua_setfield(L, -2, "randf");
    lua_pushcfunction(L, l_crc32);      lua_setfield(L, -2, "crc32");
    lua_pushcfunction(L, l_b64);        lua_setfield(L, -2, "b64");
    lua_pushcfunction(L, l_unb64);      lua_setfield(L, -2, "unb64");
    lua_pushcfunction(L, l_serialize);  lua_setfield(L, -2, "serialize");
    lua_pushcfunction(L, l_unserialize);lua_setfield(L, -2, "unserialize");
    lua_pushcfunction(L, l_epoch);      lua_setfield(L, -2, "epoch");
    lua_pushcfunction(L, l_sleep);      lua_setfield(L, -2, "sleep");
    lua_pushcfunction(L, l_clamp);      lua_setfield(L, -2, "clamp");
    lua_pushcfunction(L, l_lerp);       lua_setfield(L, -2, "lerp");
    lua_pushcfunction(L, l_dist);       lua_setfield(L, -2, "dist");
    lua_pushcfunction(L, l_round);      lua_setfield(L, -2, "round");
    lua_pushcfunction(L, l_sign);       lua_setfield(L, -2, "sign");
    lua_setfield(L, -2, "util");
}
