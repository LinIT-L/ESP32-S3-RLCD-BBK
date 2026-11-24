#!/bin/sh
# 应用注册表同步（os_app.c）—— 宿主机行为测试一键复跑
#
# 测的是**生产源码** components/system/os/kernel/os_app.c；
# stub/ 只提供它需要的最小内核接口（NVS / FreeRTOS 信号量 / 日志），
# 不复制任何生产逻辑。
#
# 覆盖: 首次登记 / 重复同步幂等 / 批内去重 / prune 回收 / prune keep 回调 /
#       内置条目永不回收 / 容量上限腾位 / 满表更新。
#
# 用法: sh tests/app_registry/run_all.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/app_registry_test}
cd "$ROOT"
mkdir -p "$WORK"

CFLAGS="-isysroot $SDK -O1 -g -std=c11 -Wall -Wextra -I tests/app_registry/stub"

echo "=========== 1) 编译（生产源码 os_app.c + 测试） ==========="
"$CC" $CFLAGS tests/app_registry/test_registry.c \
      components/system/os/kernel/os_app.c \
      -o "$WORK/test_registry"

echo "=========== 2) 运行断言 ==========="
"$WORK/test_registry"

echo "=========== 3) ASan / UBSan 再跑一遍 ==========="
SANFLAGS="-isysroot $SDK -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -std=c11 -I tests/app_registry/stub"
"$CC" $SANFLAGS tests/app_registry/test_registry.c \
      components/system/os/kernel/os_app.c \
      -o "$WORK/test_registry_san"
"$WORK/test_registry_san" > "$WORK/san.log" 2>&1
tail -3 "$WORK/san.log"

echo
echo "全部通过"
