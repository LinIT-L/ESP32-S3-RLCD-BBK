/* luat_lib_hid.c — LinTOS Lua 桥 · hid 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"

int l_wifi_clear_saved(lua_State *L) { (void)L; wifi_manager_clear_saved(); return 0; }   /* [modsplit] 共享 */


static int l_hid_cdc_read(lua_State *L) {      /* app.hid.cdc_read([maxlen]) -> string 或 nil */
    int want = (int)luaL_optinteger(L, 1, 256);
    if (want < 1) want = 1;
    if (want > 1024) want = 1024;
    char buf[1025];
    size_t n = os_hid_cdc_read((uint8_t *)buf, (size_t)want);
    if (n == 0) return 0;                        /* nil = 当前无数据 */
    lua_pushlstring(L, buf, n);
    return 1;
}


static int l_hid_start(lua_State *L) {
    lua_pushboolean(L, os_hid_start() == ESP_OK);
    return 1;
}

static int l_hid_stop(lua_State *L) {
    os_hid_key_release(); os_hid_mouse_release(0x03); os_hid_stop();
    return 0;
}

static int l_hid_connected(lua_State *L) {
    lua_pushboolean(L, os_hid_connected());
    return 1;
}

/* app.hid.running(): HID 栈是否已启动 (区别于 connected: 对端是否枚举上来) */
static int l_hid_running(lua_State *L) {
    lua_pushboolean(L, os_hid_is_running());
    return 1;
}

/* app.hid.cdc_connected(): 目标电脑的 CDC 串口是否已连接 (日志诊断页显示 USB 状态用) */
static int l_hid_cdc_connected(lua_State *L) {
    lua_pushboolean(L, os_hid_cdc_connected());
    return 1;
}

static int l_hid_key(lua_State *L) {
    uint8_t mod = (uint8_t)luaL_optinteger(L, 1, 0);
    uint8_t kc = (uint8_t)luaL_checkinteger(L, 2);
    os_hid_key_tap(mod, kc);
    return 0;
}

static int l_hid_key_str(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    for (; *s; s++) {
        char c = *s;
        uint8_t k = lua_hidc(c);
        if (!k) continue;
        uint8_t mod = (c >= 'A' && c <= 'Z') ? 0x02 : 0;
        os_hid_key_tap(mod, k);
    }
    os_hid_key_release();
    return 0;
}

static int l_hid_mouse_move(lua_State *L) {
    int8_t dx = (int8_t)luaL_optinteger(L, 1, 0), dy = (int8_t)luaL_optinteger(L, 2, 0);
    os_hid_mouse_move(dx, dy);
    return 0;
}

static int l_hid_mouse_click(lua_State *L) {
    uint8_t b = (uint8_t)luaL_optinteger(L, 1, 0x01);
    os_hid_mouse_click(b);
    return 0;
}

static int l_hid_shortcut(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    struct { const char *n; uint8_t mod; uint8_t kc; } const t[]={
        {"CTRL+C",0x01,0x06},{"CTRL+V",0x01,0x19},{"CTRL+A",0x01,0x04},
        {"CTRL+X",0x01,0x1B},{"CTRL+Z",0x01,0x11},{"CTRL+Y",0x01,0x1C},
        {"CTRL+S",0x01,0x16},{"CTRL+F",0x01,0x09},{"CTRL+P",0x01,0x13},
        {"CTRL+TAB",0x01,0x2B},{"ALT+TAB",0x04,0x2B},{"WIN+R",0x08,0x15},
        {"WIN+E",0x08,0x08},{"WIN+L",0x08,0x0F},
    };
    for (size_t i=0;i<sizeof(t)/sizeof(t[0]);i++){
        if (strcasecmp(s,t[i].n)==0){ os_hid_key_tap(t[i].mod,t[i].kc); os_hid_key_release(); return 1; }
    }
    return 0;
}

void lua_mod_hid_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_hid_start);           lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_hid_stop);            lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, l_hid_connected);       lua_setfield(L, -2, "connected");
    lua_pushcfunction(L, l_hid_running);         lua_setfield(L, -2, "running");
    lua_pushcfunction(L, l_hid_key);             lua_setfield(L, -2, "key");
    lua_pushcfunction(L, l_hid_key_str);         lua_setfield(L, -2, "key_str");
    lua_pushcfunction(L, l_hid_mouse_move);      lua_setfield(L, -2, "mouse_move");
    lua_pushcfunction(L, l_hid_mouse_click);     lua_setfield(L, -2, "mouse_click");
    lua_pushcfunction(L, l_hid_shortcut);        lua_setfield(L, -2, "shortcut");
    lua_pushcfunction(L, l_hid_cdc_read);        lua_setfield(L, -2, "cdc_read");
    lua_pushcfunction(L, l_hid_cdc_connected);   lua_setfield(L, -2, "cdc_connected");
    lua_setfield(L, -2, "hid");
}
