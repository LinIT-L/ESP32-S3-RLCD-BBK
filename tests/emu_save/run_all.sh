#!/bin/sh
# tests/emu_save/run_all.sh — 模拟器存档一键复跑
#
# 分层覆盖 (按"能测到什么"分层, 不硬凑):
#   · MD / SMS  ← 完整主机测试: 生产源码 md_savestate.cpp / sms_cbridge.cpp,
#                  内核读写用桩 (它们本来就是 extern)。覆盖 .tmp → 512B 门槛 →
#                  os_file_replace 的每个分支、旧档保护、recover、格式透传。
#   · NES / GBC / gam4980 ← 与内核上下文强耦合 (nes_context / gw / sys_flash),
#                  造假上下文就不是"测生产逻辑"了 ⇒ 改用源码契约检查。
#                  这**不是**说它们没被测 —— 它们最终都调 os_file_write_atomic,
#                  该接口已由 tests/os_file 的 195 项断言覆盖。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
CXX=${CXX:-/Library/Developer/CommandLineTools/usr/bin/clang++}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_emusave_test}

[ -x "$CC" ] || { echo "SKIP: 找不到 $CC"; exit 2; }
[ -x "$CXX" ] || { echo "SKIP: 找不到 $CXX"; exit 2; }
if [ -d "$SDK" ]; then SYSROOT="-isysroot $SDK"; else SYSROOT=""; fi

rm -rf "$WORK"; mkdir -p "$WORK" || exit 1

CFLAGS="-O2 -std=c11 -Wall -Wextra -Werror -D_DARWIN_C_SOURCE=1"
CXXFLAGS="-O2 -std=c++20 -Wall -Wextra -Werror -D_DARWIN_C_SOURCE=1"
OSI="-I components/libs/osutil/include"

echo "=========== 0) 编译公共依赖 (os_file, C) ==========="
# shellcheck disable=SC2086
$CC $SYSROOT $CFLAGS $OSI -c components/libs/osutil/os_file.c -o "$WORK/os_file.o" || {
    echo "FAIL: os_file.c 编译失败"; exit 1; }

echo
echo "=========== 1) MD 存档包装层 ==========="
# shellcheck disable=SC2086
$CXX $SYSROOT $CXXFLAGS $OSI \
    -c components/engines/md_emu/md_savestate.cpp -o "$WORK/md.o" || {
    echo "FAIL: md_savestate.cpp 编译失败"; exit 1; }
# shellcheck disable=SC2086
$CXX $SYSROOT $CXXFLAGS $OSI \
    -c tests/emu_save/test_md_savestate.cpp -o "$WORK/md_test.o" || {
    echo "FAIL: MD 测试编译失败"; exit 1; }
# shellcheck disable=SC2086
$CXX $SYSROOT "$WORK/md_test.o" "$WORK/md.o" "$WORK/os_file.o" -o "$WORK/md_test" || {
    echo "FAIL: MD 链接失败"; exit 1; }
"$WORK/md_test" 2>&1 | grep -vE '^\[MD\]' | tee "$WORK/md.log" | tail -4 || {
    echo "FAIL: MD 存档断言未通过"; exit 1; }
grep -q "ALL PASS" "$WORK/md.log" || { echo "FAIL: MD 未全通过"; exit 1; }

echo
echo "=========== 2) SMS 存档包装层 ==========="
# shellcheck disable=SC2086
$CXX $SYSROOT $CXXFLAGS -I tests/emu_save/stub $OSI \
    -c components/engines/sms_emu/sms_cbridge.cpp -o "$WORK/sms.o" || {
    echo "FAIL: sms_cbridge.cpp 编译失败"; exit 1; }
# shellcheck disable=SC2086
$CXX $SYSROOT $CXXFLAGS -I tests/emu_save/stub $OSI \
    -c tests/emu_save/test_sms_savestate.cpp -o "$WORK/sms_test.o" || {
    echo "FAIL: SMS 测试编译失败"; exit 1; }
# shellcheck disable=SC2086
$CXX $SYSROOT "$WORK/sms_test.o" "$WORK/sms.o" "$WORK/os_file.o" -o "$WORK/sms_test" || {
    echo "FAIL: SMS 链接失败"; exit 1; }
"$WORK/sms_test" 2>&1 | grep -vE '^\[SMS\]' | tee "$WORK/sms.log" | tail -4 || {
    echo "FAIL: SMS 存档断言未通过"; exit 1; }
grep -q "ALL PASS" "$WORK/sms.log" || { echo "FAIL: SMS 未全通过"; exit 1; }

echo
echo "=========== 3) NES / GBC / gam4980 源码契约 ==========="
python3 tests/emu_save/contract_check.py "$ROOT" | tee "$WORK/contract.log" | \
    grep -E "FAIL|INFO|PASS=|结果" || true
grep -q "ALL PASS" "$WORK/contract.log" || { echo "FAIL: 契约检查未通过"; exit 1; }

echo
echo "=========== 4) MD / SMS ASan 复跑 ==========="
# shellcheck disable=SC2086
$CC $SYSROOT -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
    -std=c11 -Wall -Wextra -D_DARWIN_C_SOURCE=1 $OSI \
    -c components/libs/osutil/os_file.c -o "$WORK/os_file_san.o" || exit 1
# shellcheck disable=SC2086
$CXX $SYSROOT -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
    -std=c++20 -Wall -Wextra -D_DARWIN_C_SOURCE=1 $OSI \
    -c components/engines/md_emu/md_savestate.cpp -o "$WORK/md_san.o" || exit 1
# shellcheck disable=SC2086
$CXX $SYSROOT -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
    -std=c++20 -Wall -Wextra -D_DARWIN_C_SOURCE=1 $OSI \
    -c tests/emu_save/test_md_savestate.cpp -o "$WORK/md_test_san.o" || exit 1
# shellcheck disable=SC2086
$CXX $SYSROOT -fsanitize=address,undefined \
    "$WORK/md_test_san.o" "$WORK/md_san.o" "$WORK/os_file_san.o" -o "$WORK/md_san" || exit 1
if ! "$WORK/md_san" > "$WORK/md_san.log" 2>&1; then
    echo "FAIL: MD ASan 报错"; tail -20 "$WORK/md_san.log"; exit 1
fi
echo "  MD ASan OK"

# shellcheck disable=SC2086
$CXX $SYSROOT -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
    -std=c++20 -Wall -Wextra -D_DARWIN_C_SOURCE=1 -I tests/emu_save/stub $OSI \
    -c components/engines/sms_emu/sms_cbridge.cpp -o "$WORK/sms_san.o" || exit 1
# shellcheck disable=SC2086
$CXX $SYSROOT -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 \
    -std=c++20 -Wall -Wextra -D_DARWIN_C_SOURCE=1 -I tests/emu_save/stub $OSI \
    -c tests/emu_save/test_sms_savestate.cpp -o "$WORK/sms_test_san.o" || exit 1
# shellcheck disable=SC2086
$CXX $SYSROOT -fsanitize=address,undefined \
    "$WORK/sms_test_san.o" "$WORK/sms_san.o" "$WORK/os_file_san.o" -o "$WORK/sms_san" || exit 1
if ! "$WORK/sms_san" > "$WORK/sms_san.log" 2>&1; then
    echo "FAIL: SMS ASan 报错"; tail -20 "$WORK/sms_san.log"; exit 1
fi
echo "  SMS ASan OK"

echo
echo "emu_save: 全部通过"
echo "⚠ 未覆盖 (需真机): 内核写入字节的正确性 / FatFs rename 语义 / 真实 SD 卡满载与拔卡"
exit 0
