#!/bin/bash
# merge_flash.sh - 把 bootloader / partition-table / app / ota_0(meshtastic 可选) / system.bin
#                 合并成单个 16MB Flash 镜像
# 烧入地址: 0x0 (一文件搞定, 无需 Espressif Flash Download Tool 配多地址)
# 也可拆开用 Flash Download Tool 多文件烧录:
#   bootloader.bin         @ 0x0000
#   partition-table.bin    @ 0x8000
#   LinTOS.bin      @ 0x10000 (factory)
#   meshtastic.bin         @ 0x6D0000 (ota_0, 可选; 设 MESH_APP_BIN 提供)
#   system.bin (8.BIN+E.BIN) @ 0xBD2000
#
# 布局 (16MB = 0x1000000, 双系统) —— 必须与 partitions.csv 完全一致:
#   0x000000  bootloader              32KB
#   0x009000  nvs                      24KB
#   0x010000  factory app (BBK)     6.75MB  → 0x6D0000
#   0x6D0000  ota_0 app (Meshtastic) 5MB     → 0xBD0000
#   0xBD0000  otadata                  8KB
#   0xBD2000  system (8.BIN + E.BIN)  4MB    → 0xFD2000
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
SYSTEM_BIN="${SYS_IMG_DIR}/system.bin"
MESH_APP_BIN="${MESH_APP_BIN:-${BUILD_DIR}/meshtastic/meshtastic.bin}"  # 可选: Meshtastic 独立固件(ota_0); 缺省尝试 build/meshtastic/meshtastic.bin

mkdir -p "${OUT_DIR}"

# 前置检查
for f in "${BOOTLOADER_BIN}" "${PARTITION_BIN}" "${APP_BIN}" "${SYSTEM_BIN}"; do
    if [ ! -f "${f}" ]; then
        echo "[错误] 缺少文件: ${f}"
        echo "       请先执行 ./build.sh"
        exit 1
    fi
done

BL_SIZE=$(stat -f%z "${BOOTLOADER_BIN}")
PT_SIZE=$(stat -f%z "${PARTITION_BIN}")
APP_SIZE=$(stat -f%z "${APP_BIN}")
SYS_SIZE=$(stat -f%z "${SYSTEM_BIN}")

echo "=========================================="
echo "  合并 16MB Flash 镜像"
echo "=========================================="
echo "bootloader.bin       $(printf '%8d' ${BL_SIZE}) B  @ 0x000000"
echo "partition-table.bin  $(printf '%8d' ${PT_SIZE}) B  @ 0x008000"
echo "LinTOS.bin    $(printf '%8d' ${APP_SIZE}) B  @ 0x010000 (factory)"
echo "system.bin           $(printf '%8d' ${SYS_SIZE}) B  @ 0xBD2000"
if [ -f "${MESH_APP_BIN}" ]; then
    MESH_SIZE=$(stat -f%z "${MESH_APP_BIN}")
    echo "meshtastic.bin       $(printf '%8d' ${MESH_SIZE}) B  @ 0x6D0000 (ota_0)"
else
    echo "meshtastic.bin       (未提供, ota_0 留空)  @ 0x6D0000"
fi
echo "Flash 总大小         ${FLASH_SIZE} B"
echo "=========================================="

# 创建 16MB 全 0xFF 文件 (空 Flash 初始值, 用 Python 可靠生成)
python3 -c "
import os
size = ${FLASH_SIZE}
with open('${OUT_FILE}', 'wb') as f:
    chunk = b'\xff' * (1024 * 1024)   # 1MB / 次
    for _ in range(16):              # 16MB = 16 * 1MB
        f.write(chunk)
print('  已生成', os.path.getsize('${OUT_FILE}'), '字节空白 Flash')
"

# 按偏移写入
dd if="${BOOTLOADER_BIN}" of="${OUT_FILE}" bs=1 seek=0 conv=notrunc 2>/dev/null
dd if="${PARTITION_BIN}"  of="${OUT_FILE}" bs=1 seek=$((0x008000)) conv=notrunc 2>/dev/null
dd if="${APP_BIN}"        of="${OUT_FILE}" bs=1 seek=$((0x010000)) conv=notrunc 2>/dev/null
dd if="${SYSTEM_BIN}"     of="${OUT_FILE}" bs=1 seek=$((0xBD2000)) conv=notrunc 2>/dev/null

# 双系统: Meshtastic 独立固件 → ota_0 (0x6D0000, 与 partitions.csv 一致). 未提供则留空(0xFF),
# BBK 快捷键在 ota_0 未烧时只提示不跳转 (见 boot_switch).
if [ -f "${MESH_APP_BIN}" ]; then
    echo "  Meshtastic 固件: ${MESH_APP_BIN} @ 0x6D0000 ($(stat -f%z "${MESH_APP_BIN}") B)"
    dd if="${MESH_APP_BIN}" of="${OUT_FILE}" bs=1 seek=$((0x6D0000)) conv=notrunc 2>/dev/null
else
    echo "  未提供 Meshtastic 固件, ota_0 分区留空 (设置 MESH_APP_BIN 指向其 .bin)"
fi

# 计算 SHA256, 方便用户校验
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
echo ""
echo "烧录方式二 (Flash Download Tool 多文件法):"
echo "  bootloader.bin       @ 0x0       (默认已勾选)"
echo "  partition-table.bin  @ 0x8000"
echo "  LinTOS.bin    @ 0x10000 (factory)"
echo "  meshtastic.bin       @ 0x6D0000 (ota_0, 可选)"
echo "  system.bin           @ 0xBD2000"
echo "  (chip: ESP32-S3, SPI Mode: DIO, Flash Size: 16MB, SPI Speed: 80MHz)"
