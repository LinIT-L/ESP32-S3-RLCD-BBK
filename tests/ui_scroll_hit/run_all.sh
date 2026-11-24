#!/bin/sh
# tests/ui_scroll_hit —— 滚动公共组件主机测试（ui_scroll_row_at / update / set_offset）
#
# 直接编译生产源 components/ui/ui_common/ui_scroll.c (stub 只补 esp_timer/st7305 的
# 绘制与计时符号, 不模拟任何行为):
#   test_row_at : 屏幕 y → 行号的像素反算（网格 54px+2 / 列表 48px, 小数偏移全覆盖）
#   test_update : ui_scroll_update / ui_scroll_set_offset 的标准语义（手势安全）
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_ui_scroll_hit}

[ -x "$CC" ] || { echo "SKIP: 找不到 $CC"; exit 2; }
if [ -d "$SDK" ]; then SYSROOT="-isysroot $SDK"; else SYSROOT=""; fi

mkdir -p "$WORK" || exit 1

build_run() {   # $1=测试名
    # shellcheck disable=SC2086
    if ! $CC $SYSROOT -O2 -std=c11 -Wall -Wextra \
            -I tests/ui_scroll_hit/stub -I components/ui/ui_common \
            "tests/ui_scroll_hit/$1.c" components/ui/ui_common/ui_scroll.c \
            -lm -o "$WORK/$1" 2>"$WORK/$1.cc.log"; then
        echo "FAIL: $1 编译失败"; head -20 "$WORK/$1.cc.log"; return 1
    fi
    "$WORK/$1"
}

FAILED=0
build_run test_row_at || FAILED=1
build_run test_update  || FAILED=1

if [ "$FAILED" -ne 0 ]; then
    echo "ui_scroll_hit: 有断言失败"
    exit 1
fi
echo "ui_scroll_hit: 全部通过"
exit 0
