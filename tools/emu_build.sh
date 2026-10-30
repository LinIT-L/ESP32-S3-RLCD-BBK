#!/bin/bash
# ============================================================================
# emu_build.sh — 编译「模拟器版」LinTOS (给 esp-emu 跑, 不影响真机固件)
#
#   ./tools/emu_build.sh          # 增量
#   ./tools/emu_build.sh clean    # 全量重建
#
# 与 build.sh 的区别 (刻意隔开, 互不污染):
#   build.sh  : build/     + sdkconfig      + USB-Serial/JTAG 控制台 → 真机
#   emu_build : build_emu/ + sdkconfig.emu  + UART0 控制台        → esp-emu
#
# 产物: build_emu/emu_flash.bin (16MB 整镜像, 含字库/ROM 分区)
#
# 为什么必须单独一份 sdkconfig:
#   esp-emu 不模拟 USB-Serial/JTAG 外设, 控制台留在 USB 上 ⇒ 一个字节日志都看不到。
#
# 为什么必须 LINTOS_EMU_MODE=1:
#   esp-emu 没有 SPI 屏 / I2C 触摸 / ADC 模型, 三处代码需要走模拟器分支
#   (见 st7305.c / board_battery.c 里的 #ifdef LINTOS_EMU_MODE)。
# ============================================================================
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "${PROJECT_DIR}"

# ---- ESP-IDF 环境 (与 build.sh 同源, 换机器用环境变量覆盖) ----
export IDF_SKIP_CHECK_SUBMODULES=1
export ESP_ROM_ELF_DIR="${ESP_ROM_ELF_DIR:-${HOME}/.espressif/tools/esp-rom-elfs/20260528}"
export PYTHONPATH="${PROJECT_DIR}:${PYTHONPATH}"
IDF_PATH="${IDF_PATH:-${HOME}/esp/esp-idf-v60}"
IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-${HOME}/.espressif/python_env/idf6.0_py3.11_env}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-${HOME}/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin}"
export PATH="${TOOLCHAIN_BIN_DIR}:${IDF_PYTHON_ENV_PATH}/bin:${PATH}"
export IDF_PATH IDF_PYTHON_ENV_PATH
export ESP_IDF_VERSION="${ESP_IDF_VERSION:-6.0.3}"

if [ ! -f "${IDF_PATH}/tools/idf.py" ]; then
    echo "[错误] 未找到 ESP-IDF: ${IDF_PATH}"
    exit 1
fi

# ---- sdkconfig.emu: 从真机 sdkconfig 派生, 只改控制台 ----
if [ ! -f sdkconfig.emu ]; then
    echo "[生成] sdkconfig.emu (从 sdkconfig 派生, 控制台 → UART0)"
    cp sdkconfig sdkconfig.emu
    python3 - sdkconfig.emu <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
for a, b in [
    ("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y", "# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set"),
    ("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED=y", "# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED is not set"),
    ("# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set", "CONFIG_ESP_CONSOLE_UART_DEFAULT=y"),
    ("CONFIG_ESP_CONSOLE_UART_NUM=-1", "CONFIG_ESP_CONSOLE_UART_NUM=0"),
    ("CONFIG_CONSOLE_UART_NUM=-1", "CONFIG_CONSOLE_UART_NUM=0"),
]:
    s = s.replace(a, b)
open(p, "w", encoding="utf-8").write(s)
PYEOF
fi

# ---- 构建 ----
if [ "$1" = "clean" ]; then
    echo "[clean] 清空 build_emu/"
    python3 "${IDF_PATH}/tools/idf.py" -B build_emu -DSDKCONFIG="${PROJECT_DIR}/sdkconfig.emu" fullclean || true
fi

echo "[build] LINTOS_EMU_MODE=1 → build_emu/"
python3 "${IDF_PATH}/tools/idf.py" -B build_emu \
    -DSDKCONFIG="${PROJECT_DIR}/sdkconfig.emu" \
    -DLINTOS_EMU_MODE=1 build

# ---- 合并 16MB 整镜像 (必须带字库分区, 否则模拟器里只有数字没文字) ----
echo "[merge] build_emu/emu_flash.bin"
BUILD_DIR="${PROJECT_DIR}/build_emu" \
OUT_FILE="${PROJECT_DIR}/build_emu/emu_flash.bin" \
    bash merge_flash.sh

echo
echo "✅ 完成: build_emu/emu_flash.bin"
echo "   运行: ./tools/emu_run.sh          (实时窗口)"
echo "         ./tools/emu_run.sh --png out.png   (30s 存图)"
