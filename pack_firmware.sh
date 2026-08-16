#!/bin/bash
# pack_firmware.sh - 把构建产物打包成可发布的固件包
# 输出在 dist/ 目录下:
#   - esp32-bbk-emu_Vx.y.z_merged_YYYYMMDD.zip   单文件 16MB 镜像, 烧到 0x0
#   - esp32-bbk-emu_Vx.y.z_flashdl_YYYYMMDD.zip  4 文件, 配 Espressif Flash Download Tool
#                                                     或 esptool.py 多地址烧录

set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
SYS_IMG_DIR="${PROJECT_DIR}/system_image"
DIST_DIR="${PROJECT_DIR}/dist"
VERSION="${1:-V1.0.17}"
DATE="$(date +%Y%m%d)"

mkdir -p "${DIST_DIR}"

BL_BIN="${BUILD_DIR}/bootloader/bootloader.bin"
PT_BIN="${BUILD_DIR}/partition_table/partition-table.bin"
APP_BIN="${BUILD_DIR}/esp32-bbk-emu.bin"
SYS_BIN="${SYS_IMG_DIR}/system.bin"
MERGED_BIN="${DIST_DIR}/merged_16mb.bin"

# 前置检查
for f in "${BL_BIN}" "${PT_BIN}" "${APP_BIN}" "${SYS_BIN}" "${MERGED_BIN}"; do
    if [ ! -f "${f}" ]; then
        echo "[错误] 缺少文件: ${f}"
        echo "       请先执行 ./build.sh"
        exit 1
    fi
done

# ============ 打包 1: 合并镜像 (单文件) ============
MERGED_DIR="${DIST_DIR}/esp32-bbk-emu_${VERSION}_merged_${DATE}"
rm -rf "${MERGED_DIR}"
mkdir -p "${MERGED_DIR}"
cp "${MERGED_BIN}" "${MERGED_DIR}/esp32-bbk-emu_16mb.bin"

# 烧录说明
cat > "${MERGED_DIR}/README.txt" <<EOF
esp32-bbk-emu 合并固件镜像 ${VERSION} (${DATE})
=========================================

本包内仅含 1 个 16MB 的完整 Flash 镜像, 一文件烧入即可.

文件清单
--------
- esp32-bbk-emu_16mb.bin   完整 16MB Flash 镜像
                              bootloader         @ 0x000000  (32KB)
                              partition-table    @ 0x008000  (3KB)
                              nvs                @ 0x009000  (24KB)
                              app (esp32-bbk-emu)@ 0x010000  (~1.5MB)
                              system (8.BIN+E.BIN)@ 0x900000 (4MB, gam4980 词典固件)
                              其余 0xFF 填充     @ 其余空间

烧录方法 (esptool.py, 推荐)
----------------------------
  esptool.py --chip esp32s3 -b 460800 \\
    --before default_reset --after hard_reset \\
    write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \\
    0x0 esp32-bbk-emu_16mb.bin

烧录方法 (Espressif Flash Download Tool 1 文件法)
-------------------------------------------------
  Chip:      ESP32-S3
  SPI Mode:  DIO
  SPI Speed: 80MHz
  Flash Size: 16MB
  |------------------------|
  | [x] | esp32-bbk-emu_16mb.bin | 0x0 |
  |------------------------|
  点击 START 开始烧录.
  烧完后按一下 RST 按钮重启设备.
EOF

MERGED_ZIP="${DIST_DIR}/esp32-bbk-emu_${VERSION}_merged_${DATE}.zip"
cd "${DIST_DIR}"
rm -f "${MERGED_ZIP}"
zip -r "${MERGED_ZIP}" "$(basename "${MERGED_DIR}")" -j >/dev/null
rm -rf "${MERGED_DIR}"

# ============ 打包 2: Flash Download Tool 4 文件 ============
FLASH_DIR="${DIST_DIR}/esp32-bbk-emu_${VERSION}_flashdl_${DATE}"
rm -rf "${FLASH_DIR}"
mkdir -p "${FLASH_DIR}"
cp "${BL_BIN}"  "${FLASH_DIR}/"
cp "${PT_BIN}"  "${FLASH_DIR}/"
cp "${APP_BIN}" "${FLASH_DIR}/"
cp "${SYS_BIN}" "${FLASH_DIR}/"

cat > "${FLASH_DIR}/README.txt" <<EOF
esp32-bbk-emu 多文件烧录包 ${VERSION} (${DATE})
=========================================

本包内含 4 个 .bin 文件, 需在 Espressif Flash Download Tool 中按地址配置.

文件清单
--------
- bootloader.bin         @ 0x0000    (22KB)
- partition-table.bin    @ 0x8000    (3KB)
- esp32-bbk-emu.bin      @ 0x10000   (~1.5MB, 主应用)
- system.bin             @ 0x900000  (4MB, 8.BIN+E.BIN 词典固件)

烧录方法 (Espressif Flash Download Tool)
----------------------------------------
1. 打开 ESP Flash Download Tool
2. 选择芯片: ESP32-S3
3. SPI 配置:
     SPI Mode:    DIO
     SPI Speed:   80MHz
     Flash Size:  16MB
4. 按下表添加 4 个文件 (全部勾选):
     | 文件名                | 烧入地址 |
     | bootloader.bin        | 0x0      |
     | partition-table.bin   | 0x8000   |
     | esp32-bbk-emu.bin     | 0x10000  |
     | system.bin            | 0x900000 |
5. 选好串口 (例如 /dev/cu.usbmodem1101) 和波特率 (115200 或 460800)
6. 点击 START 开始烧录
7. 烧完后按 RST 按钮重启设备

说明: system.bin 包含 gam4980 词典模拟器所需的 8.BIN 和 E.BIN
      (8.BIN 词典字库, E.BIN 屏幕点阵), 启动时自动从 flash 部署到 SD 卡.
EOF

FLASH_ZIP="${DIST_DIR}/esp32-bbk-emu_${VERSION}_flashdl_${DATE}.zip"
cd "${DIST_DIR}"
rm -f "${FLASH_ZIP}"
zip -r "${FLASH_ZIP}" "$(basename "${FLASH_DIR}")" -j >/dev/null
rm -rf "${FLASH_DIR}"

echo "=========================================="
echo "  固件打包完成"
echo "=========================================="
echo "版本: ${VERSION}"
echo ""
echo "[1] 单文件合并镜像 (推荐):"
echo "    ${MERGED_ZIP}"
ls -la "${MERGED_ZIP}" | awk '{print "    "$NF"  ("$5" 字节)"}'
echo ""
echo "[2] Flash Download Tool 4 文件:"
echo "    ${FLASH_ZIP}"
ls -la "${FLASH_ZIP}" | awk '{print "    "$NF"  ("$5" 字节)"}'
echo ""
echo "固件包已就绪, 可直接发给其他用户烧录."
