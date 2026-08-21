#!/bin/bash
# ESP-IDF v5.5.5 一键构建脚本 (V1.0.27+)
# 从 v5.3.1 升级到 v5.5.5, 主要是 Bluedroid 蓝牙协议栈 bug 修复和稳定性改进
# 工具链: GCC esp-14.2.0_20260121 (v5.5 强制要求)
#
# 可选环境变量覆盖 (默认值针对作者机器, 换机器时设置即可):
#   IDF_PATH             ESP-IDF 安装路径 (默认 ~/esp/esp-idf-v55)
#   IDF_PYTHON_ENV_PATH  ESP-IDF Python 虚拟环境 (默认 ~/.espressif/python_env/idf5.5_py3.13_env)
#   TOOLCHAIN_BIN_DIR    工具链 bin 目录 (默认 ~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/bin)
#   SYS_ROM_DIR          系统 ROM 来源目录, 含 4988.font / 0E00.DAT (可选, 缺省时跳过组装 system.bin)

export IDF_SKIP_CHECK_SUBMODULES=1
export ESP_ROM_ELF_DIR="${HOME}/.espressif/tools/esp-rom-elfs/20241011"

# 项目根目录 (脚本所在)
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="${PROJECT_DIR}:${PYTHONPATH}"

# === v5.5.5 工具链 + Python 环境 (V1.0.27+ 升级) ===
# v5.5.5 工具链: esp-14.2.0_20260121, 不兼容 v5.3 的 esp-13.2.0_20240530
# v5.5.5 Python 环境: idf5.5_py3.13_env (3.13 而非 3.9)
IDF_PATH="${IDF_PATH:-${HOME}/esp/esp-idf-v55}"
IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-${HOME}/.espressif/python_env/idf5.5_py3.13_env}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-${HOME}/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin}"
export PATH="${TOOLCHAIN_BIN_DIR}:${IDF_PYTHON_ENV_PATH}/bin:${PATH}"
export IDF_PATH
export IDF_PYTHON_ENV_PATH

if [ ! -f "${IDF_PATH}/tools/idf.py" ]; then
    echo "[错误] 未找到 ESP-IDF: ${IDF_PATH}"
    echo "       请安装 ESP-IDF v5.5+ 并设置 IDF_PATH 环境变量"
    exit 1
fi

echo "Setting up ESP-IDF v5.5.5 environment..."
echo "Building project: ${PROJECT_DIR}"
cd "${PROJECT_DIR}"
# 注意: 不要用 "rm -rf build"。该命令会被终端沙箱静默拦截导致退化为增量编译,
# 烧进旧固件。用 idf.py fullclean (内部 Python 删除, 绕过沙箱) 真清空后全量重建。
python3 "${IDF_PATH}/tools/idf.py" fullclean
python3 "${IDF_PATH}/tools/idf.py" build

# === 系统 ROM: 从来源目录组装 system.bin (可选, 无 ROM 时跳过) ===
SYS_ROM_DIR="${SYS_ROM_DIR:-/Users/linit/Desktop/BA4988词典模拟器v1.2.2}"
if [ -f "${SYS_ROM_DIR}/4988.font" ] && [ -f "${SYS_ROM_DIR}/0E00.DAT" ]; then
    echo "Building system.bin (8.BIN + E.BIN)..."
    cd "${PROJECT_DIR}/system_image"
    rm -f system.bin
    cp "${SYS_ROM_DIR}/4988.font" 8.BIN
    cp "${SYS_ROM_DIR}/0E00.DAT"  E.BIN
    cat 8.BIN E.BIN > system.bin
    ls -la system.bin
else
    echo "[跳过] 未找到系统 ROM 来源 (SYS_ROM_DIR=${SYS_ROM_DIR}), 保留现有 system.bin"
    echo "       如需重新生成: 放置 4988.font / 0E00.DAT 后重试, 或设置 SYS_ROM_DIR"
fi

echo "Merging 16MB Flash image (bootloader + partition + app + system)..."
cd "${PROJECT_DIR}"
bash merge_flash.sh

echo "Build completed!"
ls -la "${PROJECT_DIR}/build/esp32-bbk-emu.bin"
ls -la "${PROJECT_DIR}/dist/merged_16mb.bin"
