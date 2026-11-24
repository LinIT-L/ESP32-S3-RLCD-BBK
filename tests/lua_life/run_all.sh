#!/bin/sh
# tests/lua_life/run_all.sh — Lua 桥生命周期契约测试
#
# 测的是**生产源码** components/apps/lua_bridge/lua_bridge.c 的两张函数表:
#   k_mods[]       17 个模块注册函数    (lua_bridge_setup)
#   k_app_resets[]  5 个应用退出回收钩子 (lua_bridge_reset_app_state)
#
# lua_bridge.h 会 include 三十多个 ESP/Lua 头。测试只需要类型, 因此在 WORK 目录
# **临时生成**空桩头 (不落仓库), 再编译生产 .c + 测试桩。
#
# 这不是"重写生产逻辑": 表内容和调度循环都是生产代码, 桩只负责记录被调用。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_lualife_test}

[ -x "$CC" ] || { echo "SKIP: 找不到主机编译器 $CC"; exit 2; }
if [ -d "$SDK" ]; then SYSROOT="-isysroot $SDK"; else SYSROOT=""; fi

rm -rf "$WORK"; mkdir -p "$WORK/stub" || exit 1

# ---- 生成空桩头 (内容只有一条 include, 提供类型) ----
# lua.h 需要真实原型 (lua_bridge.c 直接调 lua_newtable/setfield/setglobal),
# 所以从 tests/lua_life/stub 复制, 不进 gen 列表。
REAL_STUB="tests/lua_life/stub"
cp "$REAL_STUB/lua_life_types.h" "$WORK/stub/"
cp "$REAL_STUB/lua.h" "$WORK/stub/"

gen() {
    for h in "$@"; do
        d="$WORK/stub/$(dirname "$h")"
        [ -d "$d" ] || mkdir -p "$d"
        printf '#pragma once\n#include "lua_life_types.h"\n' > "$WORK/stub/$h"
    done
}

gen lualib.h lauxlib.h \
    freertos/FreeRTOS.h freertos/task.h \
    esp_timer.h esp_log.h esp_heap_caps.h esp_system.h esp_attr.h \
    st7305.h ui_common.h font_zh.h font_zh16.h \
    os.h os_pane.h os_notify.h input.h board_battery.h \
    os_hid.h os_wifi.h os_net.h wifi_manager.h \
    esp_http_client.h esp_crt_bundle.h svc_nvs.h \
    esp_wifi.h esp_netif.h esp_netif_ip_addr.h \
    ping/ping_sock.h lwip/inet.h lwip/ip_addr.h lwip/sockets.h \
    keyboard.h netscan.h

echo "=========== 1) 编译（生产 lua_bridge.c + 桩头 + 记录桩） ==========="
# shellcheck disable=SC2086
$CC $SYSROOT -O2 -std=c11 -Wall -Wextra -Werror \
    -I "$WORK/stub" \
    -I components/apps/lua_bridge \
    tests/lua_life/test_lua_life.c \
    components/apps/lua_bridge/lua_bridge.c \
    -o "$WORK/lualife" || { echo "FAIL: 编译失败"; exit 1; }

echo
echo "=========== 2) 运行调度契约断言 ==========="
"$WORK/lualife" | tee "$WORK/run.log" || {
    echo "FAIL: 断言未通过 (详见 $WORK/run.log)"; exit 1; }

echo
echo "=========== 3) 源码级契约检查（各 reset 分支语义） ==========="
python3 tests/lua_life/contract_check.py "$ROOT" | tee "$WORK/contract.log" || {
    echo "FAIL: 契约检查未通过 (详见 $WORK/contract.log)"; exit 1; }

echo
echo "lua_bridge 调度契约 + 源码契约: 全部通过"
echo "⚠ 未覆盖: 各 reset 函数**内部**是否真的释放资源 (属真机/模块级验证)"
exit 0
