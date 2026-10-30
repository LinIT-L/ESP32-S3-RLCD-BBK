#!/bin/bash
# ============================================================================
# emu_run.sh — 跑「模拟器版」LinTOS (esp-emu + 帧导出渲染)
#
#   ./tools/emu_run.sh                    # 实时窗口 (400x300)
#   ./tools/emu_run.sh --png out.png      # 跑 30s 后存一张图 (无窗口)
#
# 依赖: ~/.local/bin/esp-emu (v0.50.0+), tkinter (实时窗口模式)
# 固件: build_emu/emu_flash.bin —— 先用 ./tools/emu_build.sh 生成
# ============================================================================
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FW="${PROJECT_DIR}/build_emu/emu_flash.bin"

if [ ! -f "${FW}" ]; then
    echo "[错误] 缺固件: ${FW}"
    echo "       先跑 ./tools/emu_build.sh"
    exit 1
fi

if [ ! -x "${HOME}/.local/bin/esp-emu" ]; then
    echo "[错误] 未找到 esp-emu: ~/.local/bin/esp-emu"
    echo "       安装: https://github.com/espressif/esp-emulator"
    exit 1
fi

exec python3 "${PROJECT_DIR}/tools/emu_screen.py" --fw "${FW}" "$@"
