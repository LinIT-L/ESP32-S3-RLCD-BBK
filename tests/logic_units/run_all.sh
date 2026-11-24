#!/bin/sh
# tests/logic_units/run_all.sh — 三个"零 ESP 依赖"的纯逻辑测试
#
# 这三个目录原本没有入口脚本, 编译命令写在各自的文件头注释里。这里统一起来,
# 让顶层 tests/run_all.sh 能一次跑完。
#
#   battery    : 电池算法 (去极值平均 / 电压查表 / CHRG 去抖)
#   time       : 时间可信度判定 (刷机后 1970 / 离线漂移 / 重复校时)
#   gray_quant : 灰阶量化 (Bayer 阈值完整性 / 极值无噪点 / 坐标稳定)
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_logic_units}

[ -x "$CC" ] || { echo "SKIP: 找不到 $CC"; exit 2; }
if [ -d "$SDK" ]; then SYSROOT="-isysroot $SDK"; else SYSROOT=""; fi

mkdir -p "$WORK" || exit 1
FAILED=0

run_one() {   # $1=名称  $2=编译参数(include)  $3..=源文件
    name=$1; inc=$2; shift 2
    # shellcheck disable=SC2086
    if ! $CC $SYSROOT -O2 -std=c11 -Wall -Wextra $inc "$@" -o "$WORK/$name" 2>"$WORK/$name.cc.log"; then
        echo "  [FAIL] $name 编译失败"; head -5 "$WORK/$name.cc.log"; return 1
    fi
    if "$WORK/$name" > "$WORK/$name.log" 2>&1; then
        echo "  [PASS] $name  $(tail -1 "$WORK/$name.log")"
        return 0
    fi
    echo "  [FAIL] $name 断言未通过"; tail -12 "$WORK/$name.log"; return 1
}

echo "=========== 电池算法 ==========="
run_one battery "-I components/drivers/board_battery/include" \
    tests/battery/battery_model_test.c components/drivers/board_battery/battery_model.c || FAILED=1

echo
echo "=========== 时间可信度 ==========="
run_one time "-I components/services/svc_time/include" \
    tests/time/time_logic_test.c components/services/svc_time/time_logic.c || FAILED=1

echo
echo "=========== 灰阶量化 ==========="
run_one gray_quant "-I components/engines/board_shim/include" \
    tests/gray_quant/gray_quant_test.c components/engines/board_shim/gray_quant.c || FAILED=1

echo
if [ "$FAILED" -ne 0 ]; then
    echo "logic_units: 有失败"
    exit 1
fi
echo "logic_units: 全部通过"
exit 0
