/**
 * @file luat_lib_haptic.c
 * @brief Lua 触觉接口: app.haptic.play / sequence / stop / active / list.
 *
 * 目标: Lua 侧只写"效果名", 不关心是实现是 PWM 还是 DRV2605L。
 *   app.haptic.play("soft_click")        -> true | false, "ok" | "cooldown" | ...
 *   app.haptic.sequence({ {strength=30, duration=15}, {strength=0, duration=25}, ... })
 *   app.haptic.stop()
 *   app.haptic.active()                   -> bool
 *   app.haptic.list()                     -> { {name=, ms=, pulses=, strength=, priority=, cooldown=}, ... }
 *
 * 参数能力 (底层只有"占空比 + 开关"):
 *   ✅ strength 0..100 · duration ms · gap(用 strength=0 的段表示) · 段数 · priority
 *   ❌ frequency / waveform / brake —— 底层不支持, 这里不开放, 传了也不认
 *
 * ⛔ 旧接口 app.vibrate(ms[, strength]) 保持不动; 本模块只新增 app.haptic 子表。
 */
#include "lua_bridge.h"
#include "haptic.h"

static bool s_hap_inited = false;

static void hap_ensure(void)
{
    if (!s_hap_inited) {
        haptic_init();
        s_hap_inited = true;
    }
}

static int push_result(lua_State *L, bool ok, haptic_reject_t why)
{
    lua_pushboolean(L, ok);
    lua_pushstring(L, haptic_reject_name(why));
    return 2;
}

/* app.haptic.play(name) -> ok, reason */
static int l_haptic_play(lua_State *L)
{
    hap_ensure();
    const char *name = luaL_checkstring(L, 1);
    haptic_reject_t why = HAPTIC_OK;
    bool ok = haptic_play(name, &why);
    return push_result(L, ok, why);
}

/* app.haptic.sequence{ {strength=0..100, duration=ms}, ... } -> ok, reason */
static int l_haptic_sequence(lua_State *L)
{
    hap_ensure();
    luaL_checktype(L, 1, LUA_TTABLE);

    int len = (int)luaL_len(L, 1);
    if (len <= 0 || len > HAPTIC_MAX_SEGS) return push_result(L, false, HAPTIC_REJECT_ARG);

    haptic_seg_t segs[HAPTIC_MAX_SEGS];
    for (int i = 1; i <= len; i++) {
        lua_rawgeti(L, 1, i);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return push_result(L, false, HAPTIC_REJECT_ARG); }

        lua_getfield(L, -1, "strength");
        int st = (int)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "duration");
        int du = (int)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_pop(L, 1);                      /* 弹出条目 */

        if (st < 0) st = 0;
        if (st > 100) st = 100;
        if (du < 0) du = 0;
        if (du > 5000) du = 5000;           /* 单段上限: 超过 5s 的"一直震"没有意义, 用 pulse */
        segs[i - 1].ms = (uint16_t)du;
        segs[i - 1].strength = (uint8_t)st;
    }

    haptic_reject_t why = HAPTIC_OK;
    bool ok = haptic_sequence(segs, len, &why);
    return push_result(L, ok, why);
}

/* app.haptic.stop() -> 立即停止当前效果并归还马达; 没在播时什么都不做(不会误伤别的振动) */
static int l_haptic_stop(lua_State *L)
{
    hap_ensure();
    haptic_stop();
    (void)L;
    return 0;
}

/* app.haptic.active() -> 是否正在播放效果 (bool) */
static int l_haptic_active(lua_State *L)
{
    lua_pushboolean(L, haptic_active());
    return 1;
}

/* app.haptic.list() -> 数组, 每项 {name, ms, pulses, strength, priority, cooldown} */
static int l_haptic_list(lua_State *L)
{
    hap_ensure();
    int n = haptic_effect_count();
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        const haptic_fx_t *fx = haptic_effect_at(i);
        if (!fx) continue;

        int ms = 0, pulses = 0, peak = 0;
        for (int k = 0; k < fx->n; k++) {
            ms += fx->segs[k].ms;
            if (fx->segs[k].strength) {
                pulses++;
                if (fx->segs[k].strength > peak) peak = fx->segs[k].strength;
            }
        }

        lua_createtable(L, 0, 6);
        lua_pushstring(L, fx->name);        lua_setfield(L, -2, "name");
        lua_pushinteger(L, ms);             lua_setfield(L, -2, "ms");
        lua_pushinteger(L, pulses);         lua_setfield(L, -2, "pulses");
        lua_pushinteger(L, peak);           lua_setfield(L, -2, "strength");
        lua_pushinteger(L, fx->priority);   lua_setfield(L, -2, "priority");
        lua_pushinteger(L, fx->cooldown_ms);lua_setfield(L, -2, "cooldown");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

void lua_mod_haptic_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_haptic_play);     lua_setfield(L, -2, "play");
    lua_pushcfunction(L, l_haptic_sequence); lua_setfield(L, -2, "sequence");
    lua_pushcfunction(L, l_haptic_stop);     lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, l_haptic_active);   lua_setfield(L, -2, "active");
    lua_pushcfunction(L, l_haptic_list);     lua_setfield(L, -2, "list");
    lua_setfield(L, -2, "haptic");
}

/* 应用退出兜底: 停播 + 清冷却表 (否则下一个应用刚进来会被上一个应用的冷却挡住) */
void lua_haptic_reset_app(void)
{
    haptic_reset_app();
}
