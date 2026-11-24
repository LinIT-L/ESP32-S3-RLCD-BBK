#!/bin/sh
# tests/engine_mgr/run_all.sh — 引擎管理器状态机一键复跑
#
# 测的是**生产源码** components/system/engine_manager/engine_manager.c。
# 8 个引擎的 load/unload 用同名桩替代 (它们在生产文件里本来就是 extern),
# 状态机/幂等/失败回滚/unload_all 全部由生产代码执行。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_engmgr_test}

[ -x "$CC" ] || { echo "SKIP: 找不到主机编译器 $CC"; exit 2; }
if [ -d "$SDK" ]; then SYSROOT="-isysroot $SDK"; else SYSROOT=""; fi

mkdir -p "$WORK" || exit 1

echo "=========== 1) 编译（生产源码 + 同名引擎桩） ==========="
# shellcheck disable=SC2086
$CC $SYSROOT -O2 -std=c11 -Wall -Wextra -Werror -DLINTOS_ENABLE_MD=1 \
    -I tests/engine_mgr/stub \
    -I components/system/engine_manager/include \
    tests/engine_mgr/test_engine_mgr.c \
    components/system/engine_manager/engine_manager.c \
    -o "$WORK/engmgr" || { echo "FAIL: 编译失败"; exit 1; }

echo
echo "=========== 2) 运行状态机断言 ==========="
"$WORK/engmgr" | tee "$WORK/run.log" || {
    echo "FAIL: 断言未通过 (详见 $WORK/run.log)"; exit 1; }

echo
echo "=========== 3) ASan / UBSan 复跑 ==========="
# shellcheck disable=SC2086
$CC $SYSROOT -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -g -O1 -std=c11 -Wall -Wextra -DLINTOS_ENABLE_MD=1 \
    -I tests/engine_mgr/stub \
    -I components/system/engine_manager/include \
    tests/engine_mgr/test_engine_mgr.c \
    components/system/engine_manager/engine_manager.c \
    -o "$WORK/engmgr_san" || { echo "FAIL: ASan 编译失败"; exit 1; }

if "$WORK/engmgr_san" > "$WORK/san.log" 2>&1; then
    tail -2 "$WORK/san.log"
else
    echo "FAIL: ASan/UBSan 报错"; tail -30 "$WORK/san.log"; exit 1
fi

echo
echo "⚠ 注意: E14/E15 是**记录缺陷现状**的断言 (管理器与引擎的 unload 语义缺口),"
echo "        它们 PASS 表示缺口仍然存在, 详见 run.log 中的 ★★ 注释。"
echo "engine_manager: 全部通过"
exit 0
