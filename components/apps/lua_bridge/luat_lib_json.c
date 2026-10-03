/* luat_lib_json.c — LinTOS Lua 桥 · json 模块
 *
 * 为什么放在固件里 (而不是让脚本手搓字符串匹配):
 *   ① 联网应用 (航班雷达 / 天气 / API 查询) 全都要解析 JSON, 每写一个就用 Lua 模式
 *      匹配一次 = 重复劳动, 而且遇到转义/嵌套就会错。
 *   ② 本项目原则是"核心能力进固件、只对脚本开放 API"。JSON 解析属于核心能力。
 *   ③ **只物化用得到的字段**: 一趟扫描直接吐出 Lua 表, 不给 Lua 堆留中间树
 *      (adsb.fi 一次 100 架 × 39 字段, 全量建表会吃掉几百 KB)。
 *
 * API:
 *   app.json.pick(json, array_key, fields[, max]) -> 表数组
 *       取顶层 array_key 指向的**对象数组**, 每个对象只保留 fields (逗号分隔) 里的键。
 *       字段缺失 → 该键不出现; 数字 → number; "ground" 之类 → 原样字符串。
 *   app.json.num(json, key[, def]) -> number      顶层数字字段
 *   app.json.str(json, key)        -> string      顶层字符串字段 (无则 nil)
 *   app.json.bool(json, key)       -> bool
 *
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段。
 */
#include "lua_bridge.h"
#include <ctype.h>

/* ---- 只读游标 (不建树) ---- */
typedef struct { const char *s; const char *end; } jsc_t;

#define JSK_MAX_DEPTH 12
#define JSK_KEY_MAX   40     /* 键名长度上限 (超长键一律不匹配) */
#define JSK_FIELD_MAX 24     /* pick 的字段数上限 */

static void jsk_ws(jsc_t *j) {
    while (j->s < j->end &&
           (*j->s == ' ' || *j->s == '\t' || *j->s == '\n' || *j->s == '\r')) j->s++;
}

/* 解析一个字符串 (游标须停在 '"' 上); 处理转义; 超长截断。返回是否成功。 */
static bool jsk_str(jsc_t *j, char *out, size_t cap) {
    jsk_ws(j);
    if (j->s >= j->end || *j->s != '"') return false;
    j->s++;
    size_t n = 0;
    while (j->s < j->end && *j->s != '"') {
        char c = *j->s++;
        if (c == '\\' && j->s < j->end) {
            char e = *j->s++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                /* \uXXXX: 字库只有 ASCII + CJK, 且本场景数据是 ASCII → 非 ASCII 折成 '?' */
                unsigned v = 0; int k = 0;
                while (k < 4 && j->s < j->end && isxdigit((unsigned char)*j->s)) {
                    char h = *j->s++;
                    v = v * 16u + (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                    k++;
                }
                c = (v < 0x80u) ? (char)v : '?';
                break;
            }
            default: c = e; break;                 /* \" \\ \/ 等 */
            }
        }
        if (n + 1 < cap) out[n++] = c;
    }
    if (j->s < j->end) j->s++;                      /* 吃掉收尾 '"' */
    if (cap) out[n] = 0;
    return true;
}

/* 跳过一个任意 JSON 值 (递归下降, 深度受限) */
static bool jsk_skip(jsc_t *j, int depth) {
    if (depth > JSK_MAX_DEPTH) return false;
    jsk_ws(j);
    if (j->s >= j->end) return false;
    char c = *j->s;
    if (c == '{') {
        j->s++;
        jsk_ws(j);
        if (j->s < j->end && *j->s == '}') { j->s++; return true; }
        for (;;) {
            char kbuf[8];
            if (!jsk_str(j, kbuf, sizeof(kbuf))) return false;
            jsk_ws(j);
            if (j->s >= j->end || *j->s != ':') return false;
            j->s++;
            if (!jsk_skip(j, depth + 1)) return false;
            jsk_ws(j);
            if (j->s < j->end && *j->s == ',') { j->s++; continue; }
            if (j->s < j->end && *j->s == '}') { j->s++; return true; }
            return false;
        }
    }
    if (c == '[') {
        j->s++;
        jsk_ws(j);
        if (j->s < j->end && *j->s == ']') { j->s++; return true; }
        for (;;) {
            if (!jsk_skip(j, depth + 1)) return false;
            jsk_ws(j);
            if (j->s < j->end && *j->s == ',') { j->s++; continue; }
            if (j->s < j->end && *j->s == ']') { j->s++; return true; }
            return false;
        }
    }
    if (c == '"') { char t[4]; return jsk_str(j, t, sizeof(t)); }
    while (j->s < j->end && *j->s != ',' && *j->s != '}' && *j->s != ']' &&
           *j->s != ' ' && *j->s != '\n' && *j->s != '\r' && *j->s != '\t') j->s++;
    return true;
}

/* 在顶层对象里找 key; 命中则把游标停在"值的起点"上 */
static bool js_top_find(const char *json, const char *key, jsc_t *val) {
    jsc_t j = { json, json + strlen(json) };
    jsk_ws(&j);
    if (j.s >= j.end || *j.s != '{') return false;
    j.s++;
    for (;;) {
        jsk_ws(&j);
        if (j.s >= j.end || *j.s == '}') return false;
        char k[JSK_KEY_MAX];
        if (!jsk_str(&j, k, sizeof(k))) return false;
        jsk_ws(&j);
        if (j.s >= j.end || *j.s != ':') return false;
        j.s++;
        jsk_ws(&j);
        if (j.s >= j.end) return false;
        if (strcmp(k, key) == 0) { val->s = j.s; val->end = j.end; return true; }
        if (!jsk_skip(&j, 0)) return false;
        jsk_ws(&j);
        if (j.s < j.end && *j.s == ',') { j.s++; continue; }
        return false;
    }
}

/* 把游标处的标量值压栈; 返回压了几个 (0 = 对象/数组/无法识别, 调用方当作"无值") */
static int jsk_push_scalar(lua_State *L, jsc_t *j, int depth) {
    (void)depth;
    jsk_ws(j);
    if (j->s >= j->end) return 0;
    char c = *j->s;
    if (c == '"') {
        /* 字符串可能很长 (callsign 顶多 8 字符, 但 ownOp 可以长) —— 用 Lua 缓冲按块拼 */
        luaL_Buffer b;
        j->s++;
        luaL_buffinit(L, &b);
        while (j->s < j->end && *j->s != '"') {
            char *w = luaL_prepbuffsize(&b, 64);
            size_t cap = 64, n = 0;
            while (n < cap && j->s < j->end && *j->s != '"') {
                char ch = *j->s++;
                if (ch == '\\' && j->s < j->end) {
                    char e = *j->s++;
                    switch (e) {
                    case 'n': ch = '\n'; break;  case 't': ch = '\t'; break;
                    case 'r': ch = '\r'; break;  case 'b': ch = '\b'; break;
                    case 'f': ch = '\f'; break;
                    case 'u': {
                        unsigned v = 0; int k = 0;
                        while (k < 4 && j->s < j->end && isxdigit((unsigned char)*j->s)) {
                            char h = *j->s++;
                            v = v * 16u + (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                            k++;
                        }
                        ch = (v < 0x80u) ? (char)v : '?';
                        break;
                    }
                    default: ch = e; break;
                    }
                }
                w[n++] = ch;
            }
            luaL_addsize(&b, n);
        }
        if (j->s < j->end) j->s++;
        luaL_pushresult(&b);
        return 1;
    }
    if (c == 't') { j->s += 4; lua_pushboolean(L, true);  return 1; }
    if (c == 'f') { j->s += 5; lua_pushboolean(L, false); return 1; }
    if (c == 'n') { j->s += 4; lua_pushnil(L);            return 1; }
    if (c == '{' || c == '[') { jsk_skip(j, 0); return 0; }   /* 嵌套结构: 不物化 */
    /* 数字 */
    {
        char nbuf[40];
        size_t n = 0;
        while (j->s < j->end && n + 1 < sizeof(nbuf) &&
               *j->s != ',' && *j->s != '}' && *j->s != ']' &&
               *j->s != ' ' && *j->s != '\n' && *j->s != '\r' && *j->s != '\t') nbuf[n++] = *j->s++;
        nbuf[n] = 0;
        if (!n) return 0;
        lua_pushnumber(L, (lua_Number)strtod(nbuf, NULL));
        return 1;
    }
}

/* ---- app.json.pick(json, array_key, fields[, max]) -> 表数组 ---- */
static int l_json_pick(lua_State *L) {
    const char *json = luaL_checkstring(L, 1);
    const char *akey = luaL_optstring(L, 2, "");
    const char *fields = luaL_optstring(L, 3, "");
    int max = (int)luaL_optinteger(L, 4, 200);
    if (max <= 0 || max > 512) max = 200;

    /* 字段白名单 */
    char want[JSK_FIELD_MAX][JSK_KEY_MAX];
    int nwant = 0;
    {
        const char *p = fields;
        while (*p && nwant < JSK_FIELD_MAX) {
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            int n = 0;
            while (*p && *p != ',' && *p != ' ' && n < JSK_KEY_MAX - 1) want[nwant][n++] = *p++;
            want[nwant][n] = 0;
            if (n) nwant++;
            while (*p && *p != ',') p++;
        }
    }
    if (!nwant) { lua_newtable(L); return 1; }

    jsc_t v;
    if (!js_top_find(json, akey, &v)) { lua_newtable(L); return 1; }
    jsk_ws(&v);
    if (v.s >= v.end || *v.s != '[') { lua_newtable(L); return 1; }
    v.s++;

    lua_newtable(L);                                   /* 结果数组 */
    int out = 0;
    jsk_ws(&v);
    if (v.s < v.end && *v.s == ']') { v.s++; return 1; }
    for (;;) {
        jsk_ws(&v);
        if (v.s >= v.end) break;
        if (*v.s == '{') {
            jsc_t o = { v.s + 1, v.end };
            lua_newtable(L);                           /* 行 */
            for (;;) {
                jsk_ws(&o);
                if (o.s >= o.end || *o.s == '}') break;
                char k[JSK_KEY_MAX];
                if (!jsk_str(&o, k, sizeof(k))) break;
                jsk_ws(&o);
                if (o.s >= o.end || *o.s != ':') break;
                o.s++;
                jsk_ws(&o);
                bool keep = false;
                for (int i = 0; i < nwant; i++) if (strcmp(k, want[i]) == 0) { keep = true; break; }
                if (keep) {
                    jsc_t vv = { o.s, o.end };
                    if (jsk_push_scalar(L, &vv, 0) == 1) lua_setfield(L, -2, k);
                    o.s = vv.s;
                }
                if (!jsk_skip(&o, 0)) break;
                jsk_ws(&o);
                if (o.s < o.end && *o.s == ',') { o.s++; continue; }
                break;
            }
            /* 跳过整个对象 (上面可能提前 break, 用独立游标保证正确) */
            jsc_t sk = { v.s, v.end };
            if (jsk_skip(&sk, 0)) v.s = sk.s;
            out++;
            lua_rawseti(L, -2, out);
            if (out >= max) break;
        } else {
            if (!jsk_skip(&v, 0)) break;
        }
        jsk_ws(&v);
        if (v.s < v.end && *v.s == ',') { v.s++; continue; }
        if (v.s < v.end && *v.s == ']') break;
    }
    return 1;
}

/* ---- app.json.num / str / bool ---- */
static int l_json_num(lua_State *L) {
    const char *json = luaL_checkstring(L, 1);
    const char *key  = luaL_checkstring(L, 2);
    double def = (double)luaL_optnumber(L, 3, 0);
    jsc_t v;
    if (!js_top_find(json, key, &v)) { lua_pushnumber(L, (lua_Number)def); return 1; }
    jsk_ws(&v);
    if (v.s < v.end && *v.s == '"') {                  /* 有的字段是字符串数字 */
        char b[32];
        if (jsk_str(&v, b, sizeof(b))) { lua_pushnumber(L, (lua_Number)strtod(b, NULL)); return 1; }
        lua_pushnumber(L, (lua_Number)def);
        return 1;
    }
    {
        int n = jsk_push_scalar(L, &v, 0);
        if (n == 1 && lua_isnumber(L, -1)) return 1;
        if (n == 1) lua_pop(L, 1);
    }
    lua_pushnumber(L, (lua_Number)def);
    return 1;
}

static int l_json_str(lua_State *L) {
    const char *json = luaL_checkstring(L, 1);
    const char *key  = luaL_checkstring(L, 2);
    jsc_t v;
    if (!js_top_find(json, key, &v)) return 0;
    jsk_ws(&v);
    if (v.s >= v.end || *v.s != '"') return 0;
    return jsk_push_scalar(L, &v, 0) == 1 ? 1 : 0;
}

static int l_json_bool(lua_State *L) {
    const char *json = luaL_checkstring(L, 1);
    const char *key  = luaL_checkstring(L, 2);
    jsc_t v;
    if (!js_top_find(json, key, &v)) { lua_pushboolean(L, false); return 1; }
    jsk_ws(&v);
    if (v.s >= v.end) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, *v.s == 't');
    return 1;
}

void lua_mod_json_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_json_pick); lua_setfield(L, -2, "pick");
    lua_pushcfunction(L, l_json_num);  lua_setfield(L, -2, "num");
    lua_pushcfunction(L, l_json_str);  lua_setfield(L, -2, "str");
    lua_pushcfunction(L, l_json_bool); lua_setfield(L, -2, "bool");
    lua_setfield(L, -2, "json");
}
