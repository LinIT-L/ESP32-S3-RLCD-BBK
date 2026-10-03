#!/bin/bash
# flash_safe.sh — 日常迭代用「安全烧录」: 只写 factory + apps, **不碰 nvs**
#
# 为什么需要它 (2026-10-02 教训):
#   整片烧 `0x0 dist/merged_16mb.bin` 会把 **nvs 分区一起写成空白** →
#   蓝牙配对、手柄按键映射、所有设置全部丢失。表现就是"手柄连不上 / 控制不了"。
#   (实测还伴随屏幕日期回到默认 09-01。)
#
# 什么时候用哪个:
#   · 只改了 C 代码 / Lua 应用 / 字库、换图标  → 本脚本 (保留 nvs, 手柄不用重配)
#   · 改了分区表 / bootloader / apps 分区布局  → 才需要整片烧 0x0 (见 merge_flash.sh 末尾提示)
#
# 用法:
#   bash flash_safe.sh            # 自动探测串口
#   bash flash_safe.sh /dev/cu.usbmodem1101
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
PY="/Users/linit/.espressif/python_env/idf5.5_py3.13_env/bin/python3"

PORT="$1"
if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
fi
if [ -z "$PORT" ]; then
  echo "✗ 找不到串口 (/dev/cu.usbmodem*) —— 设备可能关机/深睡, 按 PWR 唤醒后再试" >&2
  exit 1
fi

# 分区偏移从 partitions.csv 读 (单一真相源)
eval "$(${PY} - "$PROJECT_DIR/partitions.csv" <<'PYEOF'
import sys
for ln in open(sys.argv[1]):
    ln = ln.split('#')[0].strip()
    if not ln: continue
    f = [x.strip() for x in ln.split(',')]
    if len(f) >= 5:
        print("%s_OFF=%s" % (f[0].upper(), f[3]))
PYEOF
)"

FACTORY_BIN="${BUILD_DIR}/LinTOS.bin"
APPS_BIN="${BUILD_DIR}/apps_l.bin"

for f in "$FACTORY_BIN" "$APPS_BIN"; do
  [ -f "$f" ] || { echo "✗ 缺文件: $f (先 idf.py build + sync_apps.py)" >&2; exit 1; }
done

echo "=========================================="
echo " 安全烧录 (不动 nvs → 蓝牙/手柄配置保留)"
echo "   串口        : $PORT"
echo "   factory     : $FACTORY_BIN @ 0x$(printf '%X' ${FACTORY_OFF:-0x60000})"
echo "   apps(SPIFFS): $APPS_BIN @ 0x$(printf '%X' ${APPS_OFF:-0xC00000})"
echo "   ⛔ 不写: nvs / phy_init / bootloader / partition-table"
echo "=========================================="

env -u PYTHONPATH -u BASH_ENV -u CODEBUDDY_BROKERED_SHELL_ENV \
  PATH="${PY%/*}:/opt/homebrew/bin:/usr/bin:/bin" \
  "$PY" -m esptool --chip esp32s3 -p "$PORT" -b 921600 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x$(printf '%X' ${FACTORY_OFF:-0x60000}) "$FACTORY_BIN" \
  0x$(printf '%X' ${APPS_OFF:-0xC00000}) "$APPS_BIN"
