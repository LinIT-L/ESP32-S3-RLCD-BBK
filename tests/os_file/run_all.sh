#!/bin/sh
# tests/os_file/run_all.sh — os_file 主机测试一键复跑
#
# 测的是**生产源码** components/libs/osutil/os_file.c，不依赖 ESP32 硬件。
#
#   1) os_file_host_test.c   冒烟: 基本写入/覆盖/三种掉电现场/边界
#   2) os_file_fault_test.c  完整: 正常 13 组 · 故障注入 13 组 · 恢复矩阵 14 组 ·
#                             十阶段中断 12 组 (195 项断言)
#   3) ASan/UBSan 复跑完整测试
#
# 故障注入方式: -include stub/fault_inject.h 用宏把 libc 调用改道到 fi_*，
#   **不修改生产代码**。fault_inject.c 自己 FI_NO_REDIRECT，内部仍用真实 libc。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP(缺编译环境)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_osfile_test}

if [ ! -x "$CC" ]; then
    echo "SKIP: 找不到主机编译器 $CC (可用 CC=... 指定)"
    exit 2
fi
if [ ! -d "$SDK" ]; then
    SDK=""
    SYSROOT=""
else
    SYSROOT="-isysroot $SDK"
fi

mkdir -p "$WORK" || exit 1

INC="-I components/libs/osutil/include -I tests/os_file"
SRC=components/libs/osutil/os_file.c
BASE_FLAGS="-O2 -std=c11 -Wall -Wextra -Werror -D_DARWIN_C_SOURCE=1"

echo "=========== 1) 冒烟测试 (os_file_host_test) ==========="
# shellcheck disable=SC2086
$CC $SYSROOT $BASE_FLAGS $INC \
    tests/os_file/os_file_host_test.c "$SRC" -o "$WORK/smoke" || {
    echo "FAIL: 冒烟测试编译失败"; exit 1; }
"$WORK/smoke" || { echo "FAIL: 冒烟测试断言未通过"; exit 1; }

echo
echo "=========== 2) 完整测试 (os_file_fault_test, 注入生产源码) ==========="
# shellcheck disable=SC2086
$CC $SYSROOT $BASE_FLAGS $INC -include stub/fault_inject.h \
    -c "$SRC" -o "$WORK/os_file_fi.o" || { echo "FAIL: 生产源码(注入版)编译失败"; exit 1; }
# shellcheck disable=SC2086
$CC $SYSROOT $BASE_FLAGS -I tests/os_file \
    -c tests/os_file/stub/fault_inject.c -o "$WORK/fault_inject.o" || {
    echo "FAIL: 注入层编译失败"; exit 1; }
# shellcheck disable=SC2086
$CC $SYSROOT $BASE_FLAGS $INC \
    -c tests/os_file/os_file_fault_test.c -o "$WORK/fault_test.o" || {
    echo "FAIL: 测试主体编译失败"; exit 1; }
# shellcheck disable=SC2086
$CC $SYSROOT "$WORK/fault_test.o" "$WORK/os_file_fi.o" "$WORK/fault_inject.o" \
    -o "$WORK/fault_test" || { echo "FAIL: 链接失败"; exit 1; }

"$WORK/fault_test" | tee "$WORK/fault_test.log" || {
    echo "FAIL: 完整测试断言未通过 (详见 $WORK/fault_test.log)"; exit 1; }

echo
echo "=========== 3) ASan / UBSan 复跑 ==========="
# shellcheck disable=SC2086
$CC $SYSROOT -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -g -O1 -std=c11 -Wall -Wextra -D_DARWIN_C_SOURCE=1 $INC \
    -include stub/fault_inject.h -c "$SRC" -o "$WORK/os_file_san.o" || {
    echo "FAIL: ASan 版生产源码编译失败"; exit 1; }
# shellcheck disable=SC2086
$CC $SYSROOT -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
    -std=c11 -Wall -Wextra -D_DARWIN_C_SOURCE=1 -I tests/os_file \
    -c tests/os_file/stub/fault_inject.c -o "$WORK/fi_san.o" || exit 1
# shellcheck disable=SC2086
$CC $SYSROOT -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
    -std=c11 -Wall -Wextra -D_DARWIN_C_SOURCE=1 $INC \
    -c tests/os_file/os_file_fault_test.c -o "$WORK/test_san.o" || exit 1
# shellcheck disable=SC2086
$CC $SYSROOT -fsanitize=address,undefined \
    "$WORK/test_san.o" "$WORK/os_file_san.o" "$WORK/fi_san.o" -o "$WORK/t_san" || exit 1

if "$WORK/t_san" > "$WORK/san.log" 2>&1; then
    tail -2 "$WORK/san.log"
else
    echo "FAIL: ASan/UBSan 报错"
    tail -30 "$WORK/san.log"
    exit 1
fi

echo
echo "os_file: 全部通过"
exit 0
