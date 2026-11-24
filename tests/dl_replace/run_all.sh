#!/bin/sh
# tests/dl_replace/run_all.sh — 落盘替换 / 掉电恢复 主机测试
#
# 编译的**就是生产源码**: netc_fs_replace.c(薄转发) + os_file.c(唯一实现)。
# 因此本套件与 os_file 套件是同一份实现的两种调用视角:
#   os_file   : 直连 API, 含故障注入(写到一半失败 / rename 失败 / 回滚也失败)
#   dl_replace: 走 netc 的命名(.part/.bak)与枚举, 验证薄转发没有语义漂移
#
# 补建入口原因: 原本只有文件头手动编译命令, 顶层入口找不到它。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP(环境缺失)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_dl_replace}

[ -x "$CC" ] || { echo "SKIP: 找不到编译器 $CC"; exit 2; }
if [ -d "$SDK" ]; then SYSROOT="-isysroot $SDK"; else SYSROOT=""; fi

NETC=components/services/net_client
OSI=components/libs/osutil
[ -f "$NETC/netc_fs_replace.c" ] || { echo "SKIP: 缺少 $NETC/netc_fs_replace.c"; exit 2; }
[ -f "$OSI/os_file.c" ]          || { echo "SKIP: 缺少 $OSI/os_file.c"; exit 2; }

mkdir -p "$WORK" || exit 1
BIN="$WORK/dl_replace"

# shellcheck disable=SC2086
if ! $CC $SYSROOT -O2 -std=c11 -Wall -Wextra -I "$NETC" -I "$OSI/include" \
        tests/dl_replace/dl_replace.c "$NETC/netc_fs_replace.c" "$OSI/os_file.c" \
        -o "$BIN" 2>"$WORK/cc.log"; then
    echo "  [FAIL] 编译失败"; head -15 "$WORK/cc.log"; exit 1
fi

"$BIN"
