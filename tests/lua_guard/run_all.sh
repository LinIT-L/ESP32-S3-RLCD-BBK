#!/bin/sh
# tests/lua_guard/run_all.sh — Lua 路径守卫主机测试 (直接编译生产源码 lua_path_guard.c)
#
# 补建入口的原因: 该测试原本只有文件头里的手动编译命令, 顶层 tests/run_all.sh
# 找不到它 ⇒ 每次改 os_file/沙箱都可能顺手弄坏它却无人发现。
#
# ⚠ 覆盖边界: 本测试只证明 lua_path_guard 这个**模块自身**的判定正确。
#   不证明"沙箱真的调用了它" —— 2026-11-02 起 lua_sandbox_apply() 已统一放开权限,
#   不再安装路径守卫, 生产代码里当前没有它的调用点 (见 docs/测试/ 文档 §4)。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP(环境缺失)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_lua_guard}

[ -x "$CC" ] || { echo "SKIP: 找不到编译器 $CC"; exit 2; }
if [ -d "$SDK" ]; then SYSROOT="-isysroot $SDK"; else SYSROOT=""; fi

mkdir -p "$WORK" || exit 1
BIN="$WORK/lua_guard_test"

# shellcheck disable=SC2086
if ! $CC $SYSROOT -O2 -std=c11 -Wall -Wextra -I components/apps/lua_bridge \
        tests/lua_guard/lua_guard_test.c components/apps/lua_bridge/lua_path_guard.c \
        -o "$BIN" 2>"$WORK/cc.log"; then
    echo "  [FAIL] 编译失败"; head -15 "$WORK/cc.log"; exit 1
fi

"$BIN"
