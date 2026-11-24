/* lua.h 桩 —— lua_bridge.c 只调这三个函数。 */
#pragma once
#include "lua_life_types.h"

void lua_newtable(lua_State *L);
void lua_setfield(lua_State *L, int idx, const char *k);
void lua_setglobal(lua_State *L, const char *name);
