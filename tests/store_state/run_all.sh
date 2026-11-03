#!/bin/sh
# 商店条目状态机 —— 宿主机测试一键复跑
#
# 测的是**生产源码** components/apps/pages/am_store_state.c（纯 C，零 ESP 依赖）。
# 覆盖：
#   ① 版本比较（build 优先 / 老包迁移 / 老索引退回 ver / 同版本不同构建）
#   ② 状态表（下载/卸载/更新 + 四种进行中态 + 任务结束后回到重判结果）
#   ③ registry 解析（正常/CRLF/值含等号/重复键/损坏记录）
#   ④ 异步结果防覆盖（旧代次结果被丢弃 / 代次回绕）
#   ⑤ 时序场景（进店 → 安装 → 旧查询晚到 → 不闪回"下载"）
#
# 用法: sh tests/store_state/run_all.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/store_state_test}
cd "$ROOT"
mkdir -p "$WORK"

CFLAGS="-isysroot $SDK -O2 -std=c11 -Wall -Wextra -Werror -I components/apps/pages"

echo "=========== 1) 编译（生产源码 + 测试） ==========="
"$CC" $CFLAGS tests/store_state/test_state.c components/apps/pages/am_store_state.c \
      -o "$WORK/test_state"

echo "=========== 2) 运行状态机断言 ==========="
"$WORK/test_state"

echo "=========== 3) ASan / UBSan 再跑一遍 ==========="
SANFLAGS="-isysroot $SDK -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -std=c11 -Wall -Wextra -I components/apps/pages"
"$CC" $SANFLAGS tests/store_state/test_state.c components/apps/pages/am_store_state.c \
      -o "$WORK/test_state_san"
"$WORK/test_state_san" > "$WORK/san.log" 2>&1
tail -3 "$WORK/san.log"

echo
echo "全部通过"
