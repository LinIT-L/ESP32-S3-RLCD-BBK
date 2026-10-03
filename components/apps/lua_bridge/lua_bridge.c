/* lua_bridge.c — 桥总调度: 只有一张模块表, 加模块 = 加一行。 */
#include "lua_bridge.h"

/* ⚠ const 要放在 * 右边 (指向函数的指针数组本身只读);
 *   写成 `const void (*k_mods[])()` 时 const 修饰的是返回值 → -Wignored-qualifiers。 */
static void (*const k_mods[])(lua_State *) = {
    lua_mod_consts_register,
    lua_mod_gfx_register,
    lua_mod_input_register,
    lua_mod_sys_register,
    lua_mod_fs_register,
    lua_mod_store_register,
    lua_mod_net_register,
    lua_mod_json_register,
    lua_mod_wifi_register,
    lua_mod_hid_register,
    lua_mod_kbd_register,
    lua_mod_img_register,
};

void lua_bridge_setup(lua_State *L) {
    lua_newtable(L);                       /* app */
    for (unsigned i = 0; i < sizeof(k_mods) / sizeof(k_mods[0]); i++) k_mods[i](L);
    lua_setglobal(L, "app");
}
