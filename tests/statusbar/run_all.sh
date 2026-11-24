#!/bin/sh
# 状态栏显隐判据 —— 宿主机测试一键复跑
#
# 测的是**生产头文件** components/system/os/include/statusbar_policy.h
# （os_statusbar_show / os_statusbar_page_req 用的同一份判据, 全 inline、零 ESP 依赖）。
# 覆盖: 未声明按历史行为 / 声明优先于全屏 / 声明按页归属(退出不残留) / 场景回放。
#
# 用法: sh tests/statusbar/run_all.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/statusbar_test}
cd "$ROOT"
mkdir -p "$WORK"

CFLAGS="-isysroot $SDK -O2 -std=c11 -Wall -Wextra -Werror -I components/system/os/include"

echo "=========== 1) 编译（生产判据 + 测试） ==========="
"$CC" $CFLAGS tests/statusbar/test_policy.c -o "$WORK/test_policy"

echo "=========== 2) 运行断言 ==========="
"$WORK/test_policy"

echo "=========== 3) ASan / UBSan 再跑一遍 ==========="
SANFLAGS="-isysroot $SDK -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -std=c11 -Wall -Wextra -I components/system/os/include"
"$CC" $SANFLAGS tests/statusbar/test_policy.c -o "$WORK/test_policy_san"
"$WORK/test_policy_san" > "$WORK/san.log" 2>&1
tail -3 "$WORK/san.log"

echo
echo "全部通过"
