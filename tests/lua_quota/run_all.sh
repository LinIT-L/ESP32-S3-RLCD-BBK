#!/bin/sh
# tests/lua_quota/run_all.sh — Lua 内存配额账本 / 时限预算主机测试
#
# 编译生产源码 lua_quota.c (固件里 lua_sandbox_mem_account() 调的就是它)。
# 补建入口原因: 原本只有文件头手动编译命令, 顶层入口找不到 ⇒ 无声腐烂。
#
# 覆盖: 扩容/缩容/释放三种计费、异常入参不产生 size_t 下溢、10 万次申请释放后
#       计数精确归零、remain_ms 的边界(0/到期/超期/INT_MAX 夹紧)。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP(环境缺失)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_lua_quota}

[ -x "$CC" ] || { echo "SKIP: 找不到编译器 $CC"; exit 2; }
if [ -d "$SDK" ]; then SYSROOT="-isysroot $SDK"; else SYSROOT=""; fi

mkdir -p "$WORK" || exit 1
BIN="$WORK/quota_test"

# shellcheck disable=SC2086
if ! $CC $SYSROOT -O2 -std=c11 -Wall -Wextra -I components/apps/lua_bridge \
        tests/lua_quota/quota_test.c components/apps/lua_bridge/lua_quota.c \
        -o "$BIN" 2>"$WORK/cc.log"; then
    echo "  [FAIL] 编译失败"; head -15 "$WORK/cc.log"; exit 1
fi

"$BIN"
