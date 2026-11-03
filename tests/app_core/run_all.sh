#!/bin/sh
# 统一应用核心（纯逻辑）—— 宿主机测试一键复跑
#
# 测的是**生产源码** components/apps/pages/am_core_logic.c（纯 C，零 ESP 依赖）。
# 覆盖：身份归一 / 类型隔离 / 资源包过滤 / 安装状态（获取·更新·卸载·下架·离线）/
#       应用中心与商店投影 / 端到端场景（旧固件装卡 → 刷机 → 服务器删条目 / 拔卡插回）。
#
# 用法: sh tests/app_core/run_all.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/app_core_test}
cd "$ROOT"
mkdir -p "$WORK"

CFLAGS="-isysroot $SDK -O2 -std=c11 -Wall -Wextra -Werror -I components/apps/pages"

echo "=========== 1) 编译（生产源码 + 测试） ==========="
"$CC" $CFLAGS tests/app_core/test_core.c components/apps/pages/am_core_logic.c \
      components/apps/pages/am_store_state.c \
      -o "$WORK/test_core"

echo "=========== 2) 运行断言 ==========="
"$WORK/test_core"

echo "=========== 3) ASan / UBSan 再跑一遍 ==========="
SANFLAGS="-isysroot $SDK -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -std=c11 -Wall -Wextra -I components/apps/pages"
"$CC" $SANFLAGS tests/app_core/test_core.c components/apps/pages/am_core_logic.c \
      components/apps/pages/am_store_state.c \
      -o "$WORK/test_core_san"
"$WORK/test_core_san" > "$WORK/san.log" 2>&1
tail -3 "$WORK/san.log"

echo
echo "全部通过"
