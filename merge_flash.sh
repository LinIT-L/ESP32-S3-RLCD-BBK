#!/bin/bash
# merge_flash.sh - 把 bootloader / partition-table / app / system.bin 合并成单个 16MB Flash 镜像
# 烧入地址: 0x0 (一文件搞定, 无需 Espressif Flash Download Tool 配多地址)
# 也可拆开用 Flash Download Tool 多文件烧录:
#   bootloader.bin         @ 0x0000
#   partition-table.bin    @ 0x8000
#   esp32-bbk-emu.bin      @ 0x10000
#   system.bin (8.BIN+E.BIN) @ 0x900000
#
# 布局 (16MB = 0x1000000):
#   0x000000  bootloader           ~ 32KB
#   0x008000  partition-table      ~ 3KB
#   0x009000  nvs                  24KB
#   0x010000  factory app (esp32-bbk-emu.bin)   ≤ 8MB
#   0x900000  system (8.BIN + E.BIN)            4MB
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
APP_BIN="${BUILD_DIR}/esp32-bbk-emu.bin"
SYSTEM_BIN="${SYS_IMG_DIR}/system.bin"

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
echo "esp32-bbk-emu.bin    $(printf '%8d' ${APP_SIZE}) B  @ 0x010000"
echo "system.bin           $(printf '%8d' ${SYS_SIZE}) B  @ 0x900000"
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
dd if="${SYSTEM_BIN}"     of="${OUT_FILE}" bs=1 seek=$((0x900000)) conv=notrunc 2>/dev/null

# V1.0.46: 内置游戏分区 (games.fat @ 0xD00000, 3MB) — 由 tools/pack_games.py 生成
GAMES_FAT="${OUT_DIR}/games.fat"
if [ -f "${GAMES_FAT}" ]; then
    echo "  内置游戏: ${GAMES_FAT} @ 0xD00000 ($(stat -f%z "${GAMES_FAT}") B)"
    dd if="${GAMES_FAT}" of="${OUT_FILE}" bs=1 seek=$((0xD00000)) conv=notrunc 2>/dev/null
else
    echo "  内置游戏: 未打包 (运行: python3 tools/pack_games.py games dist/games.fat)"
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
echo "烧录方式二 (Flash Download Tool 4 文件法):"
echo "  bootloader.bin       @ 0x0       (默认已勾选)"
echo "  partition-table.bin  @ 0x8000"
echo "  esp32-bbk-emu.bin    @ 0x10000"
echo "  system.bin           @ 0x900000"
echo "  (chip: ESP32-S3, SPI Mode: DIO, Flash Size: 16MB, SPI Speed: 80MHz)"
