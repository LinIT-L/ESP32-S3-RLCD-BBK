#!/bin/bash
# merge_flash.sh - 把 bootloader / partition-table / app / appdata(8.BIN+E.BIN+字库) 合并成单个 16MB Flash 镜像
# 烧入地址: 0x0 (一文件搞定)
# 也可拆开用 Flash Download Tool 多文件烧录:
#   bootloader.bin         @ 0x0000
#   partition-table.bin    @ 0x8000
#   LinTOS.bin             @ 0x20000 (factory, 4MB)
#   8.BIN                  @ 0x520000 (appdata+0,   gam4980 词典字库)
#   E.BIN                  @ 0x720000 (appdata+2MB, gam4980 屏幕点阵)
#
# 布局 (16MB = 0x1000000, 单固件+数据区) —— 必须与 partitions.csv 完全一致:
#   0x000000  bootloader              32KB
#   0x009000  nvs                      32KB
#   0x011000  phy_init                  4KB
#   0x020000  factory app (LinTOS)    5MB    → 0x520000
#   0x520000  appdata (8.BIN+E.BIN+字库)  ~10.875MB → 0x1000000
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

# 字库区 (appdata 分区内 4MB 起, 固定槽位, 与 font_part.h 完全一致)
#   [V1.0.9x 精简后仅含]
#   槽1 0x400000 font_zh.bin / 槽2 0x480000 font_zh16.bin / 槽3 0x4C0000 lav_font.bin
FONT_ZH_BIN="${PROJECT_DIR}/components/fonts/font_zh.bin"
FONT_16_BIN="${PROJECT_DIR}/components/fonts/font_zh16.bin"
LAV_FONT_SRC="${PROJECT_DIR}/components/lavax/core/source/font.c"   # 文曲星字库源数组
LAV_FONT_TMP="${PROJECT_DIR}/build/lav_font.bin"                    # 提取产物

APP_BASE=$((0x520000))
FONT_ZH_ABS=$((APP_BASE + 0x400000))        # appdata+0x400000
FONT_16_ABS=$((APP_BASE + 0x480000))
FONT_LAV_ABS=$((APP_BASE + 0x4C0000))       # lav_font (V1.0.9x: 前移回填空档)

# os_db 区 (appdata+0x540000 起 = 字库区结束后, macoui 字典等运行时 mmap 只读数据)
# [V1.0.9x: 电子书 fnt 及细宋槽移除、lav_font 前移, os_db 迁至字库区后]
OUI_DB_BIN="${PROJECT_DIR}/components/macoui/oui_db.bin"
OUI_DB_ABS=$((APP_BASE + 0x540000))

echo "=========================================="
echo "  合并 16MB Flash 镜像 (单固件+数据区)"
echo "=========================================="
echo "bootloader.bin       $(printf '%8d' ${BL_SIZE}) B  @ 0x000000"
echo "partition-table.bin  $(printf '%8d' ${PT_SIZE}) B  @ 0x008000"
echo "LinTOS.bin           $(printf '%8d' ${APP_SIZE}) B  @ 0x020000 (factory)"
if [ -f "${ROM8_BIN}" ] && [ -f "${ROME_BIN}" ]; then
    ROM8_SIZE=$(stat -f%z "${ROM8_BIN}")
    ROME_SIZE=$(stat -f%z "${ROME_BIN}")
    echo "8.BIN                $(printf '%8d' ${ROM8_SIZE}) B  @ 0x$(printf '%X' ${APP_BASE}) (appdata+0)"
    echo "E.BIN                $(printf '%8d' ${ROME_SIZE}) B  @ 0x$(printf '%X' $((APP_BASE + 0x200000))) (appdata+2MB)"
else
    echo "8.BIN/E.BIN          (未提供, appdata 留空)"
fi
if [ -f "${FONT_ZH_BIN}" ] && [ -f "${FONT_16_BIN}" ]; then
    echo "font_zh.bin          $(printf '%8d' $(stat -f%z "${FONT_ZH_BIN}")) B  @ 0x$(printf '%X' ${FONT_ZH_ABS}) (appdata+4MB)"
    echo "font_zh16.bin        $(printf '%8d' $(stat -f%z "${FONT_16_BIN}")) B  @ 0x$(printf '%X' ${FONT_16_ABS})"
    if [ -f "${LAV_FONT_SRC}" ]; then
        python3 -c "
import re,sys
text=open('${LAV_FONT_SRC}').read()
m=re.search(r'lav_font\[\]\s*=\s*\{(.*?)\};', text, re.S)
if not m: sys.exit('lav_font 数组未找到')
data=bytes(int(x,16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(1)))
open('${LAV_FONT_TMP}','wb').write(data)
" && echo "lav_font.bin         $(printf '%8d' $(stat -f%z "${LAV_FONT_TMP}")) B  @ 0x$(printf '%X' ${FONT_LAV_ABS})"
    fi
else
    echo "字库区               (未提供, appdata 字库区留空, 中文/电子书/文曲星将不显示)"
fi
if [ -f "${OUI_DB_BIN}" ]; then
    echo "oui_db.bin           $(printf '%8d' $(stat -f%z "${OUI_DB_BIN}")) B  @ 0x$(printf '%X' ${OUI_DB_ABS}) (os_db)"
else
    echo "oui_db.bin           (未提供, 厂商字典缺失)"
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
    dd if="${ROM8_BIN}" of="${OUT_FILE}" bs=1 seek=${APP_BASE} conv=notrunc 2>/dev/null
    dd if="${ROME_BIN}" of="${OUT_FILE}" bs=1 seek=$((APP_BASE + 0x200000)) conv=notrunc 2>/dev/null
fi
# 字库区写 appdata (V1.0.89+ 从固件迁出, 运行时 esp_partition_mmap 读取)
if [ -f "${FONT_ZH_BIN}" ] && [ -f "${FONT_16_BIN}" ]; then
    dd if="${FONT_ZH_BIN}"    of="${OUT_FILE}" bs=1 seek=${FONT_ZH_ABS}    conv=notrunc 2>/dev/null
    dd if="${FONT_16_BIN}"    of="${OUT_FILE}" bs=1 seek=${FONT_16_ABS}    conv=notrunc 2>/dev/null
    if [ -f "${LAV_FONT_TMP}" ]; then
        dd if="${LAV_FONT_TMP}" of="${OUT_FILE}" bs=1 seek=${FONT_LAV_ABS} conv=notrunc 2>/dev/null
    fi
fi
# os_db 区: OUI 字典 (macoui 运行时 mmap)
if [ -f "${OUI_DB_BIN}" ]; then
    dd if="${OUI_DB_BIN}" of="${OUT_FILE}" bs=1 seek=${OUI_DB_ABS} conv=notrunc 2>/dev/null
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
