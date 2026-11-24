#!/bin/sh
# tests/mem_sim/run_all.sh — 内存模型推演 (主机侧算式, 不是 ESP32 模拟器)
#
# ⚠ 边界必须说清: 本程序**不模拟** ESP32-S3 的真实堆布局 / cache / ROM 保留区,
#   也不模拟 PSRAM 访问成本。它校验的是"源码里的尺寸算式"(I2S DMA 描述符数、
#   显示缓冲、任务栈单位)与"分配器碎片模型", 输出的是主机模型数据。
#   真机可用内存只能来自串口 `mem` / `audit` 命令 —— 见 docs/测试/ §4。
#
# 补建入口原因: 原本只有文件头手动编译命令, 顶层入口找不到它。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP(环境缺失)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
SDK=${SDK:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}
WORK=${WORK:-/tmp/lintos_mem_sim}

[ -x "$CC" ] || { echo "SKIP: 找不到编译器 $CC"; exit 2; }
if [ -d "$SDK" ]; then SYSROOT="-isysroot $SDK"; else SYSROOT=""; fi

mkdir -p "$WORK" || exit 1
BIN="$WORK/mem_sim"

# shellcheck disable=SC2086
if ! $CC $SYSROOT -O2 -std=c11 -Wall -Wextra tests/mem_sim/mem_sim.c \
        -o "$BIN" 2>"$WORK/cc.log"; then
    echo "  [FAIL] 编译失败"; head -15 "$WORK/cc.log"; exit 1
fi

"$BIN"
