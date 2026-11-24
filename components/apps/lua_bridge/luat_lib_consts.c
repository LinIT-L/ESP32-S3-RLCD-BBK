/* luat_lib_consts.c — 常量模块: app.W/H/BLACK/WHITE/UP..HOME
 * ⚠ 动作常量必须与 os.h 的 os_action_t 一致, 用 _Static_assert 钉住(改歪了直接编译失败)。 */
#include "lua_bridge.h"

_Static_assert(OS_ACTION_UP == 1 && OS_ACTION_DOWN == 2 && OS_ACTION_LEFT == 3 &&
               OS_ACTION_RIGHT == 4 && OS_ACTION_CONFIRM == 5 && OS_ACTION_BACK == 6 &&
               OS_ACTION_HOME == 7, "os_action_t 与 app.UP..HOME 的约定变了, 同步更新文档");

void lua_mod_consts_register(lua_State *L) {
    lua_pushinteger(L, 400); lua_setfield(L, -2, "W");
    lua_pushinteger(L, 300); lua_setfield(L, -2, "H");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "BLACK");
    lua_pushinteger(L, 1); lua_setfield(L, -2, "WHITE");
    lua_pushinteger(L, 1); lua_setfield(L, -2, "UP");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "DOWN");
    lua_pushinteger(L, 3); lua_setfield(L, -2, "LEFT");
    lua_pushinteger(L, 4); lua_setfield(L, -2, "RIGHT");
    lua_pushinteger(L, 5); lua_setfield(L, -2, "OK");
    lua_pushinteger(L, 5); lua_setfield(L, -2, "CONFIRM");
    lua_pushinteger(L, 6); lua_setfield(L, -2, "BACK");
    lua_pushinteger(L, 7); lua_setfield(L, -2, "HOME");
}
