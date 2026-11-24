idf_component_register(
    SRCS
        "lua/lapi.c"
        "lua/lauxlib.c"
        "lua/lbaselib.c"
        "lua/lcode.c"
        "lua/lctype.c"
        "lua/lcorolib.c"
        "lua/ldblib.c"
        "lua/ldebug.c"
        "lua/ldo.c"
        "lua/lfunc.c"
        "lua/lgc.c"
        "lua/linit.c"
        "lua/liolib.c"
        "lua/llex.c"
        "lua/lmathlib.c"
        "lua/lmem.c"
        "lua/lopcodes.c"
        "lua/loadlib.c"
        "lua/loslib.c"
        "lua/lstate.c"
        "lua/ltable.c"
        "lua/ltm.c"
        "lua/lvm.c"
        "lua/lobject.c"
        "lua/lparser.c"
        "lua/lstrlib.c"
        "lua/lstring.c"
        "lua/ltablib.c"
        "lua/ldump.c"
        "lua/lundump.c"
        "lua/lutf8lib.c"
        "lua/lzio.c"
    INCLUDE_DIRS
        "port/include"
        "lua"
)

# Enable 32-bit mode for Lua (required for ESP32 and Xtensa/RISC-V architectures)
#
# ⚠ 2026-10-02 总根因修复: 这里**必须是 PUBLIC**(原来写的是 PRIVATE)。
#   LUA_32BITS 决定 lua_Integer / lua_Number 的宽度(32 位 int / float vs 64 位 long long / double),
#   而 lua/lua.h 里的 `#include "luaconf.h"` 会优先命中**上游 lua/luaconf.h**
#   (那份里 LUA_32BITS 是注释掉的) → 只对 Lua 库自己 PRIVATE 定义的话:
#       库 = 32 位 lua_Integer, 消费方(apps/pages/page_lua.c 等) = 64 位
#     ⇒ lua_pushinteger / luaL_checkinteger / lua_tointeger 的 ABI 不一致,
#       **C 送出的数字到脚本侧全变成垃圾**(实测 app.key() 返回 PSRAM 地址,
#       app.UP 读成 -2, action(a) 的数字入参失效)。改成 PUBLIC 后两边同宽度, 数字才可靠。
target_compile_definitions(${COMPONENT_LIB} PUBLIC LUA_32BITS=1)

# Suppress compiler warnings from upstream Lua code
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)

