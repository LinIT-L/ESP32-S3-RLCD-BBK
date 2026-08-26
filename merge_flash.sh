#!/bin/bash
# merge_flash.sh - 把 bootloader / partition-table / app / appdata(8.BIN+E.BIN) 合并成单个 16MB Flash 镜像
# 烧入地址: 0x0 (一文件搞定)
# 也可拆开用 Flash Download Tool 多文件烧录:
#   bootloader.bin         @ 0x0000
#   partition-table.bin    @ 0x8000
#   LinTOS.bin             @ 0x20000 (factory, 6MB)
#   8.BIN                  @ 0x620000 (appdata+0,   gam4980 词典字库)
#   E.BIN                  @ 0x820000 (appdata+2MB, gam4980 屏幕点阵)
#
# 布局 (16MB = 0x1000000, 单固件+统一数据区) —— 必须与 partitions.csv 完全一致:
#   0x000000  bootloader              32KB
#   0x009000  nvs                      32KB
#   0x011000  phy_init                  4KB
#   0x020000  factory app (LinTOS)    6MB    → 0x620000
#   0x620000  appdata (8.BIN+E.BIN)  ~9.9MB  → 0x1000000
#   其余 0xFF 填充

set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
SYS_IMG_DIR="${PROJECT_DIR}/system_image"
OUT_DIR="${PROJECT_DIR}/dist"
OUT_FILE="${OUT_DIR}/merged_16mb.bin"

FLASH_SIZE=$((16 * 1024 * 1024))   # 16MB
BOOTLOADER_BIN="${BUILD_DIR}/bootloader/bootloader.bin"
PARTITION_BIN="${BUILD_DIR}/partition_table/partition-table.bin"
APP_BIN="${BUILD_DIR}/LinTOS.bin"
ROM8_BIN="${SYS_IMG_DIR}/8.BIN"
ROME_BIN="${SYS_IMG_DIR}/E.BIN"

mkdir -p "${OUT_DIR}"

# 前置检查 (ROM 可选: 缺省时 appdata 留空, 不影响系统启动)
for f in "${BOOTLOADER_BIN}" "${PARTITION_BIN}" "${APP_BIN}"; do
    if [ ! -f "${f}" ]; then
        echo "[错误] 缺少文件: ${f}"
        echo "       请先执行 build 完成"
        exit 1
    fi
done

BL_SIZE=$(stat -f%z "${BOOTLOADER_BIN}")
PT_SIZE=$(stat -f%z "${PARTITION_BIN}")
APP_SIZE=$(stat -f%z "${APP_BIN}")

echo "=========================================="
echo "  合并 16MB Flash 镜像 (单固件+数据区)"
echo "=========================================="
echo "bootloader.bin       $(printf '%8d' ${BL_SIZE}) B  @ 0x000000"
echo "partition-table.bin  $(printf '%8d' ${PT_SIZE}) B  @ 0x008000"
echo "LinTOS.bin           $(printf '%8d' ${APP_SIZE}) B  @ 0x020000 (factory)"
if [ -f "${ROM8_BIN}" ] && [ -f "${ROME_BIN}" ]; then
    ROM8_SIZE=$(stat -f%z "${ROM8_BIN}")
    ROME_SIZE=$(stat -f%z "${ROME_BIN}")
    echo "8.BIN                $(printf '%8d' ${ROM8_SIZE}) B  @ 0x620000 (appdata+0)"
    echo "E.BIN                $(printf '%8d' ${ROME_SIZE}) B  @ 0x820000 (appdata+2MB)"
else
    echo "8.BIN/E.BIN          (未提供, appdata 留空)"
fi
echo "Flash 总大小         ${FLASH_SIZE} B"
echo "=========================================="

# 创建 16MB 全 0xFF 文件
python3 -c "
import os
size = ${FLASH_SIZE}
with open('${OUT_FILE}', 'wb') as f:
    chunk = b'\xff' * (1024 * 1024)
    for _ in range(16):
        f.write(chunk)
print('  已生成', os.path.getsize('${OUT_FILE}'), '字节空白 Flash')
"

# 按偏移写入
dd if="${BOOTLOADER_BIN}" of="${OUT_FILE}" bs=1 seek=0 conv=notrunc 2>/dev/null
dd if="${PARTITION_BIN}"  of="${OUT_FILE}" bs=1 seek=$((0x008000)) conv=notrunc 2>/dev/null
dd if="${APP_BIN}"        of="${OUT_FILE}" bs=1 seek=$((0x020000)) conv=notrunc 2>/dev/null
if [ -f "${ROM8_BIN}" ] && [ -f "${ROME_BIN}" ]; then
    dd if="${ROM8_BIN}" of="${OUT_FILE}" bs=1 seek=$((0x620000)) conv=notrunc 2>/dev/null
    dd if="${ROME_BIN}" of="${OUT_FILE}" bs=1 seek=$((0x820000)) conv=notrunc 2>/dev/null
fi

SHA=$(shasum -a 256 "${OUT_FILE}" | awk '{print $1}')
OUT_SIZE=$(stat -f%z "${OUT_FILE}")
echo ""
echo "[完成] 合并镜像: ${OUT_FILE}"
echo "  大小:     ${OUT_SIZE} 字节 (16 MB)"
echo "  SHA-256:  ${SHA}"
echo ""
echo "烧录方式一 (esptool.py, 推荐):"
echo "  esptool.py --chip esp32s3 -b 460800 --before default_reset --after hard_reset \\"
echo "    write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \\"
echo "    0x0 ${OUT_FILE}"
