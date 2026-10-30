#!/bin/bash
# merge_flash.sh - 把 bootloader / partition-table / app / appdata(8.BIN+E.BIN+字库) 合并成单个 16MB Flash 镜像
# 烧入地址: 0x0 (一文件搞定)
# 也可拆开用 Flash Download Tool 多文件烧录:
#   bootloader.bin         @ 0x0000
#   partition-table.bin    @ 0x8000
#   LinTOS.bin             @ 0x60000 (factory, 7.625MB)
#   8.BIN                  @ 0x520000 (appdata+0,   gam4980 词典字库)
#   E.BIN                  @ 0x720000 (appdata+2MB, gam4980 屏幕点阵)
#
# ============================================================ 
# 布局 (16MB = 0x1000000) —— **权威来源是 partitions.csv**, 本脚本的地址全部从它解析,
# 下面只是给人看的快照 (改动请只改 partitions.csv, 别改这里):
#   0x000000  bootloader                        32KB
#   0x010000  nvs                               256KB → 0x50000
#   0x050000  phy_init                            4KB
#   0x060000  factory app (LinTOS)                5MB → 0x560000
#   0x560000  appdata (8.BIN/E.BIN/字库)       6.625MB → 0xC00000
#   0xC00000  apps (SPIFFS, Lua 单文件应用)       4MB → 0x1000000
#   ⚠ 原注释里的 "factory 4MB @0x060000 / apps 2MB @0xE00000" 是 2026-09-22 分区调整
#     **之前**的旧布局, 已按 partitions.csv 更新。
# ============================================================

set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
# ★ 2026-10-30: BUILD_DIR / OUT_FILE 允许被环境变量覆盖 —— 模拟器构建用
#   BUILD_DIR=build_emu OUT_FILE=build_emu/emu_flash.bin bash merge_flash.sh
#   不改这两行默认值, 真机构建 (bash merge_flash.sh) 行为完全不变。
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build}"
SYS_IMG_DIR="${PROJECT_DIR}/system_image"
OUT_DIR="${PROJECT_DIR}/dist"
OUT_FILE="${OUT_FILE:-${OUT_DIR}/merged_16mb.bin}"

# IDF 独立 python 环境 (避免 macOS /usr/bin/python3 Xcode 垫片)。
# 2026-10-31: 不再写死作者机器的 idf5.5_py3.13 路径 —— 优先用 build.sh 导出的
#   IDF_PYTHON_ENV_PATH, 退回 PATH 里的 python3。换机器/换 IDF 版本都能用。
PY="${IDF_PYTHON_ENV_PATH:-}/bin/python3"
if [ ! -x "${PY}" ]; then PY="$(command -v python3 || true)"; fi
if [ ! -x "${PY}" ]; then
    echo "[错误] 找不到可用的 python3 (请设置 IDF_PYTHON_ENV_PATH 或安装 python3)"
    exit 1
fi

# spiffsgen.py 属于 ESP-IDF 自带工具, 按 IDF_PATH 定位; 兼容老路径做兜底。
SPIFFSGEN="${IDF_PATH:-${HOME}/esp/esp-idf-v60}/components/spiffs/spiffsgen.py"
if [ ! -f "${SPIFFSGEN}" ]; then
    for c in "${HOME}/esp/esp-idf-v60/components/spiffs/spiffsgen.py" \
             "${HOME}/esp/esp-idf-v55/components/spiffs/spiffsgen.py"; do
        if [ -f "${c}" ]; then SPIFFSGEN="${c}"; break; fi
    done
fi
if [ ! -f "${SPIFFSGEN}" ]; then
    echo "[错误] 找不到 spiffsgen.py: ${SPIFFSGEN}"
    echo "       请设置 IDF_PATH 指向 ESP-IDF 安装目录"
    exit 1
fi

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
FONT_ZH_BIN="${PROJECT_DIR}/components/ui/fonts/font_zh.bin"
FONT_16_BIN="${PROJECT_DIR}/components/ui/fonts/font_zh16.bin"
LAV_FONT_SRC="${PROJECT_DIR}/components/engines/lavax/core/source/font.c"   # 文曲星字库源数组
LAV_FONT_TMP="${PROJECT_DIR}/build/lav_font.bin"                    # 提取产物
BP_LUT_BIN="${PROJECT_DIR}/system_image/bp_lut.bin"   # 迁移自固件的 SMS 像素表 (256KB)


# ---- 分区偏移/大小: 直接从 partitions.csv 解析 (单一真相源, 改布局不必再手改本脚本) ----
# 历史教训: 本脚本曾硬编码 0x460000/0xA60000, 重构后分区表变了却没人改脚本 →
#   合并镜像把数据写到错误地址, 且不报错。现在改为读取 partitions.csv。
eval "$(${PY} - "$PROJECT_DIR/partitions.csv" <<'PYEOF'
import sys, csv
rows = []
for ln in open(sys.argv[1]):
    ln = ln.split('#')[0].strip()
    if not ln: continue
    f = [x.strip() for x in ln.split(',')]
    if len(f) >= 5:
        rows.append((f[0], int(f[3], 0), int(f[4], 0)))
d = {n: (off, sz) for n, off, sz in rows}
print("APP_BASE=%d" % d['appdata'][0])
print("APPDATA_SIZE=%d" % d['appdata'][1])
print("APPS_OFF=%d" % d['apps'][0])
print("APPS_SIZE=%d" % d['apps'][1])
print("FACTORY_OFF=%d" % d['factory'][0])
PYEOF
)"
echo "分区 (来自 partitions.csv):"
echo "  appdata 0x$(printf '%X' ${APP_BASE})  $(printf '%.2f' $(echo "${APPDATA_SIZE}/1048576" | bc -l)) MB"
echo "  apps    0x$(printf '%X' ${APPS_OFF})  $(printf '%.2f' $(echo "${APPS_SIZE}/1048576" | bc -l)) MB"
echo "  factory 0x$(printf '%X' ${FACTORY_OFF})"
echo ""

FONT_ZH_ABS=$((APP_BASE + 0x400000))        # appdata+0x400000
FONT_16_ABS=$((APP_BASE + 0x480000))
FONT_LAV_ABS=$((APP_BASE + 0x4C0000))       # lav_font (V1.0.9x: 前移回填空档)
# 2026-10-30: 设置里字体切换的候选字库 (appdata 末尾空闲区, 不改分区表)
FONT_ALT16_ABS=$((APP_BASE + 0x5C0000))
FONT_ALT24_ABS=$((APP_BASE + 0x600000))
FONT_ALT16_BIN="${PROJECT_DIR}/components/ui/fonts/font_alt16.bin"
FONT_ALT24_BIN="${PROJECT_DIR}/components/ui/fonts/font_alt24.bin"

# [2026-09-16] oui_db 已内嵌进 macoui 组件(EMBED_FILES), 不再进 appdata

echo "=========================================="
echo "  合并 16MB Flash 镜像 (单固件+数据区)"
echo "=========================================="
echo "bootloader.bin       $(printf '%8d' ${BL_SIZE}) B  @ 0x000000"
echo "partition-table.bin  $(printf '%8d' ${PT_SIZE}) B  @ 0x008000"
echo "LinTOS.bin           $(printf '%8d' ${APP_SIZE}) B  @ 0x$(printf '%X' ${FACTORY_OFF}) (factory)"
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
        ${PY} -c "
import re,sys
text=open('${LAV_FONT_SRC}').read()
m=re.search(r'lav_font\[\]\s*=\s*\{(.*?)\};', text, re.S)
if not m: sys.exit('lav_font 数组未找到')
data=bytes(int(x,16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(1)))
open('${LAV_FONT_TMP}','wb').write(data)
" && echo "lav_font.bin         $(printf '%8d' $(stat -f%z "${LAV_FONT_TMP}")) B  @ 0x$(printf '%X' ${FONT_LAV_ABS})"
    fi
else
    echo "[错误] 字库源文件缺失, 拒绝生成镜像:"
    echo "         ${FONT_ZH_BIN}"
    echo "         ${FONT_16_BIN}"
    echo "       合并镜像若不含字库, 设备上中文/电子书/文曲星全部不显示, 且不会有任何报错."
    echo "       (历史事故: 五层重构后路径已变, 旧的 components/fonts 静默跳过 → 烧出无字库镜像)"
    exit 1
fi
echo "Flash 总大小         ${FLASH_SIZE} B"
echo "=========================================="

# 创建 16MB 全 0xFF 文件
${PY} -c "
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
dd if="${APP_BIN}"        of="${OUT_FILE}" bs=1 seek=${FACTORY_OFF} conv=notrunc 2>/dev/null
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
# 候选 UI 字库 (字体切换用; 缺文件则槽保持 0xFF, 设备端切换会失败并回退默认)
if [ -f "${FONT_ALT16_BIN}" ]; then
    dd if="${FONT_ALT16_BIN}" of="${OUT_FILE}" bs=1 seek=${FONT_ALT16_ABS} conv=notrunc 2>/dev/null
    echo "font_alt16.bin      $(printf '%8d' $(stat -f%z "${FONT_ALT16_BIN}")) B  @ 0x$(printf '%X' ${FONT_ALT16_ABS})"
fi
if [ -f "${FONT_ALT24_BIN}" ]; then
    dd if="${FONT_ALT24_BIN}" of="${OUT_FILE}" bs=1 seek=${FONT_ALT24_ABS} conv=notrunc 2>/dev/null
    echo "font_alt24.bin      $(printf '%8d' $(stat -f%z "${FONT_ALT24_BIN}")) B  @ 0x$(printf '%X' ${FONT_ALT24_ABS})"
fi
# 迁移自固件的常量表槽位 (appdata 分区内, 64KB 对齐, 与 render_bp_lut.c 一致)
if [ -f "${BP_LUT_BIN}" ]; then
    dd if="${BP_LUT_BIN}" of="${OUT_FILE}" bs=1 seek=$((APP_BASE + 0x540000)) conv=notrunc 2>/dev/null
fi
SZ_ADD_BIN="${PROJECT_DIR}/system_image/SZHVC_add.bin"
SZ_SUB_BIN="${PROJECT_DIR}/system_image/SZHVC_sub.bin"
if [ -f "${SZ_ADD_BIN}" ]; then
    dd if="${SZ_ADD_BIN}" of="${OUT_FILE}" bs=1 seek=$((APP_BASE + 0x580000)) conv=notrunc 2>/dev/null
fi
if [ -f "${SZ_SUB_BIN}" ]; then
    dd if="${SZ_SUB_BIN}" of="${OUT_FILE}" bs=1 seek=$((APP_BASE + 0x5A0000)) conv=notrunc 2>/dev/null
fi
SPIFFS_BIN="${BUILD_DIR}/apps_l.bin"
APPS_SRC="${PROJECT_DIR}/apps/lua"
# APPS_SIZE / APPS_OFF 已由上方 partitions.csv 解析得出
# ★★ 2026-10-14 linit: **默认不把 Lua 应用打进 16MB 固件** ——
#   Lua 应用走 TF 卡 / 应用商店**后装**, 固件里 apps 分区保持空 SPIFFS。
#   需要出"预置演示机/测试机"固件时才显式开:  LINTOS_BAKE_LUA_APPS=1 ./build.sh
# ⚠ 陷阱 (2026-09-29 排查发现): 源目录不在时**不能默默沿用上一版 apps_l.bin** ——
#   那会把早已下架的应用/旧脚本一起烧回设备 (应用管家会多出"C版+Lua版"重复条目), 而且没有提示。
if [ "${LINTOS_BAKE_LUA_APPS:-0}" = "1" ] && [ -d "${APPS_SRC}" ]; then
    echo "[提示] LINTOS_BAKE_LUA_APPS=1 → 预置 Lua 应用进 apps 分区"
else
    echo "[提示] 默认不预置 Lua 应用 (要预置: LINTOS_BAKE_LUA_APPS=1 ./build.sh) → 生成空 apps 镜像"
    APPS_SRC="$(mktemp -d)"
    trap 'rm -rf "${APPS_SRC}"' EXIT
fi
${PY} "${SPIFFSGEN}" \
    --page-size 256 --block-size 4096 --meta-len 4 --obj-name-len 64 --use-magic \
    ${APPS_SIZE} "${APPS_SRC}" "${SPIFFS_BIN}" \
  && echo "apps_l.bin        $(printf '%6d' $(stat -f%z "${SPIFFS_BIN}")) B  (从 $(basename "${APPS_SRC}") 生成)"
if [ -f "${SPIFFS_BIN}" ]; then
    dd if="${SPIFFS_BIN}" of="${OUT_FILE}" bs=1 seek=${APPS_OFF} conv=notrunc 2>/dev/null
    echo "apps (SPIFFS)     $(printf '%8d' $(stat -f%z "${SPIFFS_BIN}")) B  @ 0x$(printf '%X' ${APPS_OFF})"
fi
# app.db 走 nvs(core_db 命名空间), 无独立 raw 分区; apps(spiffs) 用 apps_l.bin 预置应用/测试包
# oui_db 已内嵌 macoui, 无 os_db 写入

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
