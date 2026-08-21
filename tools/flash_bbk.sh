#!/usr/bin/env bash
# 固定烧录脚本: ESP32-S3 @115200, 直接调用 esptool, 让 1%~100% 进度实时流式打印(不打到 tail/stderr 之外)
# 用法:  cd 项目根目录 && bash tools/flash_bbk.sh [port]
set -e

IDF_PATH="${IDF_PATH:-/Users/linit/esp/esp-idf-v55}"
ENV_BIN="/Users/linit/.espressif/python_env/idf5.5_py3.13_env/bin"
PORT="${1:-/dev/cu.usbmodem1101}"
BAUD=115200
# 自动定位当前项目根目录 (脚本位于 PROJECT/tools/ 下)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$SCRIPT_DIR/.." && pwd)"
BLD="$PROJ/build"
echo "== 项目目录: $PROJ =="

echo "== 烧录到 $PORT @ ${BAUD} =="
# flash_args 内为相对 build 目录的路径, 先 cd 到 build 再执行 esptool.
cd "$BLD"
# -b 批量不加 --no-stub, 让 esptool 输出 Write at ... (%) 逐块进度
"$ENV_BIN/python3" -m esptool \
    --chip esp32s3 \
    -p "$PORT" \
    -b "$BAUD" \
    --before default_reset \
    --after hard_reset \
    write_flash \
    --flash_mode dio --flash_size 16MB --flash_freq 80m \
    "@$BLD/flash_args"
echo "== 烧录完成 =="