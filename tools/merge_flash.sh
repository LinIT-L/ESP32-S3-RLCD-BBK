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

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"   # ★ 脚本已移入 tools/: 上一级才是项目根
# ★ 2026-10-30: BUILD_DIR / OUT_FILE 允许被环境变量覆盖 —— 模拟器构建用
#   BUILD_DIR=build_emu OUT_FILE=build_emu/emu_flash.bin bash tools/merge_flash.sh
#   不改这两行默认值, 真机构建 (bash tools/merge_flash.sh) 行为完全不变。
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build}"
SYS_IMG_DIR="${PROJECT_DIR}/system_image"
OUT_DIR="${PROJECT_DIR}/dist"
OUT_FILE="${OUT_FILE:-${OUT_DIR}/merged_16mb.bin}"

# ★★ 2026-11-24 可靠性改造 (事故后): 全程只写 ${OUT_FILE}.tmp, 结构检查通过才 mv 到正式路径。
#   事故: 分区表路径断链 → 解析失败 → 偏移为空 → dd seek= 报错 → 正式文件里只剩
#   bootloader+分区表, factory 起全 0xFF, 但脚本照样打印"[完成] SHA-256"。这种镜像
#   大小正好 16MB, 肉眼看不出, 烧进去就是 "image at 0x60000 has invalid magic byte" 无限重启。
#   现在: 失败时正式文件分毫不动, 不可能产出"看着完整"的残缺镜像。
OUT_TMP="${OUT_FILE}.tmp"
APPS_TMPDIR=""
cleanup() {
    rm -f "${OUT_TMP}"
    if [ -n "${APPS_TMPDIR}" ]; then rm -rf "${APPS_TMPDIR}"; fi
}
trap cleanup EXIT

# stat 跨平台: macOS/BSD 用 -f%z, Linux/GNU 用 -c%s。
fsize() {
    stat -f%z "$1" 2>/dev/null || stat -c%s "$1"
}

# IDF 独立 python 环境 (避免 macOS /usr/bin/python3 Xcode 垫片)。
# 2026-10-31: 不再写死作者机器的 idf5.5_py3.13 路径 —— 优先用 build.sh 导出的
#   IDF_PYTHON_ENV_PATH, 退回 PATH 里的 python3。换机器/换 IDF 版本都能用。
PY="${IDF_PYTHON_ENV_PATH:-}/bin/python3"
# ★ 2026-11-24: 与 flash_safe.sh 统一 —— 允许 PYTHON=/path 直接指定解释器。
#   原因: 系统 /usr/bin/python3 是 Xcode 垫片 (许可证未同意时任何脚本都跑不了),
#   而 command -v python3 会优先拿到它 ⇒ partitions.csv 解析失败 ⇒ 残缺镜像。
if [ ! -x "${PY}" ] && [ -n "${PYTHON:-}" ] && [ -x "${PYTHON}" ]; then PY="${PYTHON}"; fi
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

BL_SIZE=$(fsize "${BOOTLOADER_BIN}")
PT_SIZE=$(fsize "${PARTITION_BIN}")
APP_SIZE=$(fsize "${APP_BIN}")

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
# ★ 2026-11-24: 脚本移入 tools/ 后这里仍按项目根找 partitions.csv ⇒ 文件不存在 ⇒
#   解析静默失败 ⇒ 偏移全空 ⇒ dd seek= 报错 ⇒ 镜像里只剩 bootloader+分区表 (硬编码偏移),
#   factory 起全是 0xFF。烧进去就是 "image at 0x60000 has invalid magic byte" 无限重启。
#   现在两处都找 (tools/ 优先), 且解析完必须断言, 绝不允许空偏移继续。
PART_CSV="${PROJECT_DIR}/tools/partitions.csv"
[ -f "${PART_CSV}" ] || PART_CSV="${PROJECT_DIR}/partitions.csv"
if [ ! -f "${PART_CSV}" ]; then
    echo "[错误] 找不到 partitions.csv (已试 tools/ 与项目根): 拒绝生成镜像"
    exit 1
fi
# ★ 解析结果必须"要么全对、要么一声不响地失败" —— 原实现用 eval "$(...)", python 挂了
#   时命令替换返回空串, eval "" 返回 0, set -e 完全不触发, 于是带着空偏移一路往下走。
#   现在先落到变量 (赋值语句的退出码 = python 的退出码), 再 eval。
PARSED="$("${PY}" - "${PART_CSV}" "${FLASH_SIZE}" <<'PYEOF'
import sys

csv_path, flash = sys.argv[1], int(sys.argv[2])
rows, bad = [], []
with open(csv_path) as fh:
    for no, ln in enumerate(fh, 1):
        s = ln.split('#')[0].strip()
        if not s:
            continue
        f = [x.strip() for x in s.split(',')]
        if len(f) < 5:
            bad.append("第%d行 字段不足5个: %s" % (no, s))
            continue
        try:
            off, sz = int(f[3], 0), int(f[4], 0)
        except ValueError:
            bad.append("第%d行 偏移/大小不是整数: %s" % (no, s))
            continue
        rows.append((f[0], off, sz, no))
if bad:
    sys.exit("[错误] partitions.csv 解析失败:\n  " + "\n  ".join(bad))

d = {}
for name, off, sz, no in rows:
    if off < 0 or sz <= 0:
        sys.exit("[错误] partitions.csv 第%d行 偏移/大小非法 (%s: 0x%X/%d)"
                 % (no, name, off, sz))
    if off % 0x1000 or sz % 0x1000:
        sys.exit("[错误] partitions.csv 第%d行 未按 4KB 对齐 (%s: 0x%X/%d)"
                 % (no, name, off, sz))
    if off + sz > flash:
        sys.exit("[错误] partitions.csv 第%d行 超出 Flash 容量 (%s: 0x%X+0x%X > 0x%X)"
                 % (no, name, off, sz, flash))
    if name in d:
        sys.exit("[错误] partitions.csv 分区名重复: %s" % name)
    d[name] = (off, sz)

for need in ("factory", "appdata", "apps"):
    if need not in d:
        sys.exit("[错误] partitions.csv 缺少必需分区: %s" % need)

ordered = sorted(d.items(), key=lambda kv: kv[1][0])
for (n1, (o1, s1)), (n2, (o2, _)) in zip(ordered, ordered[1:]):
    if o1 + s1 > o2:
        sys.exit("[错误] 分区重叠: %s (0x%X..0x%X) 压到 %s (0x%X..)"
                 % (n1, o1, o1 + s1, n2, o2))

print("FACTORY_OFF=%d" % d['factory'][0])
print("FACTORY_SIZE=%d" % d['factory'][1])
print("APP_BASE=%d" % d['appdata'][0])
print("APPDATA_SIZE=%d" % d['appdata'][1])
print("APPS_OFF=%d" % d['apps'][0])
print("APPS_SIZE=%d" % d['apps'][1])
PYEOF
)" || { echo "[错误] partitions.csv 解析失败, 拒绝生成镜像"; exit 1; }

eval "${PARSED}"
for _v in FACTORY_OFF FACTORY_SIZE APP_BASE APPDATA_SIZE APPS_OFF APPS_SIZE; do
    if [ -z "${!_v}" ]; then
        echo "[错误] partitions.csv 解析结果缺 ${_v} → 拒绝生成残缺镜像"
        exit 1
    fi
    if [ "${!_v}" -le 0 ]; then
        echo "[错误] partitions.csv 解析结果非法: ${_v}=${!_v}"
        exit 1
    fi
done
echo "分区 (来自 partitions.csv):"
echo "  appdata 0x$(printf '%X' ${APP_BASE})  $(printf '%.2f' $(echo "${APPDATA_SIZE}/1048576" | bc -l)) MB"
echo "  apps    0x$(printf '%X' ${APPS_OFF})  $(printf '%.2f' $(echo "${APPS_SIZE}/1048576" | bc -l)) MB"
echo "  factory 0x$(printf '%X' ${FACTORY_OFF})"
echo ""

FONT_ZH_ABS=$((APP_BASE + 0x400000))        # appdata+0x400000
FONT_16_ABS=$((APP_BASE + 0x480000))
FONT_LAV_ABS=$((APP_BASE + 0x4C0000))       # lav_font (V1.0.9x: 前移回填空档)
# [2026-11-24] 原 ALT16(+0x5C0000, 256KB) / ALT24(+0x600000, 512KB) 候选字库槽已删除:
#   设置页从来没有字体切换入口, 唯一调用者是启动恢复 NVS, 双档结构无用。
#   appdata 尾部这 768KB 自此不再写入; 槽位偏移与分区表均未改动。

# [2026-09-16] oui_db 已内嵌进 macoui 组件(EMBED_FILES), 不再进 appdata

echo "=========================================="
echo "  合并 16MB Flash 镜像 (单固件+数据区)"
echo "=========================================="
echo "bootloader.bin       $(printf '%8d' ${BL_SIZE}) B  @ 0x000000"
echo "partition-table.bin  $(printf '%8d' ${PT_SIZE}) B  @ 0x008000"
echo "LinTOS.bin           $(printf '%8d' ${APP_SIZE}) B  @ 0x$(printf '%X' ${FACTORY_OFF}) (factory)"
if [ -f "${ROM8_BIN}" ] && [ -f "${ROME_BIN}" ]; then
    ROM8_SIZE=$(fsize "${ROM8_BIN}")
    ROME_SIZE=$(fsize "${ROME_BIN}")
    echo "8.BIN                $(printf '%8d' ${ROM8_SIZE}) B  @ 0x$(printf '%X' ${APP_BASE}) (appdata+0)"
    echo "E.BIN                $(printf '%8d' ${ROME_SIZE}) B  @ 0x$(printf '%X' $((APP_BASE + 0x200000))) (appdata+2MB)"
else
    echo "8.BIN/E.BIN          (未提供, appdata 留空)"
    if [ -f "${ROM8_BIN}" ] || [ -f "${ROME_BIN}" ]; then
        echo "[警告] 8.BIN / E.BIN 只存在一个 (两者都写才生效) → BBK 词典/点阵数据不完整"
    fi
fi
if [ -f "${FONT_ZH_BIN}" ] && [ -f "${FONT_16_BIN}" ]; then
    echo "font_zh.bin          $(printf '%8d' $(fsize "${FONT_ZH_BIN}")) B  @ 0x$(printf '%X' ${FONT_ZH_ABS}) (appdata+4MB)"
    echo "font_zh16.bin        $(printf '%8d' $(fsize "${FONT_16_BIN}")) B  @ 0x$(printf '%X' ${FONT_16_ABS})"
    if [ -f "${LAV_FONT_SRC}" ]; then
        ${PY} -c "
import re,sys
text=open('${LAV_FONT_SRC}').read()
m=re.search(r'lav_font\[\]\s*=\s*\{(.*?)\};', text, re.S)
if not m: sys.exit('lav_font 数组未找到')
data=bytes(int(x,16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(1)))
open('${LAV_FONT_TMP}','wb').write(data)
" && echo "lav_font.bin         $(printf '%8d' $(fsize "${LAV_FONT_TMP}")) B  @ 0x$(printf '%X' ${FONT_LAV_ABS})"
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

# ---- 统一的段写入: put_at <源文件> <目标偏移> <段名> <该段可写的上限偏移> ----
# ★★ 2026-11-24 用 python 的 seek+write 取代 `dd bs=1`:
#   ① 性能: bs=1 写 10MB 要 34 秒 (逐字节一次系统调用); seek+write 一次完成, 亚秒级。
#   ② 正确性: 想用 bs=4096 提速就得把偏移换算成块号, 一旦某段偏移不是 4096 整数倍,
#      dd 会**静默错位**写入 —— 比慢可怕得多。这里直接按字节 seek, 不存在换算。
#   ③ 可观测: 原 dd 全部带 `2>/dev/null`, 出错时用户只看到脚本莫名退出。现在越界/
#      空文件/短写都打印段名和偏移, 并且写完立即回读比对。
put_at() {
    "${PY}" - "${OUT_TMP}" "$1" "$2" "$3" "$4" "${FLASH_SIZE}" <<'PYEOF'
import sys
out, src, name = sys.argv[1], sys.argv[2], sys.argv[4]
off, seg_end, flash = int(sys.argv[3]), int(sys.argv[5]), int(sys.argv[6])
try:
    data = open(src, 'rb').read()
except OSError as e:
    sys.exit("[错误] %s 读取失败: %s" % (name, e))
if not data:
    sys.exit("[错误] %s 源文件为空: %s" % (name, src))
if off < 0 or off + len(data) > flash:
    sys.exit("[错误] %s 超出 16MB Flash: 0x%X+%d > 0x%X"
             % (name, off, len(data), flash))
if off + len(data) > seg_end:
    sys.exit("[错误] %s 越界, 会压到下一段: 0x%X+%d > 上限 0x%X"
             % (name, off, len(data), seg_end))
with open(out, 'r+b') as f:
    f.seek(off)
    n = f.write(data)
    f.flush()          # 显式落缓冲, 保证回读看到的就是落盘内容
    f.seek(off)
    back = f.read(len(data))
if n != len(data) or back != data:
    sys.exit("[错误] %s 写入/回读校验失败 @0x%X (写 %d, 读回 %d)"
             % (name, off, n, len(back)))
print("  ok  %s  %d B @ 0x%07X" % (name, len(data), off))
PYEOF
}

# 创建 16MB 全 0xFF 临时文件 (正式文件在校验通过前不碰)
"${PY}" -c "
import os
size = ${FLASH_SIZE}
chunk = b'\xff' * (1024 * 1024)
with open('${OUT_TMP}', 'wb') as f:
    left = size
    while left > 0:
        n = min(left, len(chunk))
        f.write(chunk[:n]); left -= n
got = os.path.getsize('${OUT_TMP}')
if got != size:
    raise SystemExit('[错误] 空白镜像大小 %d != %d' % (got, size))
print('  已生成 %d 字节空白 Flash (临时文件)' % got)
"

# 按偏移写入 (每段都做越界 + 回读校验, 任一失败立即中止)
put_at "${BOOTLOADER_BIN}" 0                    "bootloader.bin"      $((0x8000))
put_at "${PARTITION_BIN}"  $((0x8000))          "partition-table.bin" $((0x9000))
put_at "${APP_BIN}"        "${FACTORY_OFF}"     "LinTOS.bin"          $((FACTORY_OFF + FACTORY_SIZE))
if [ -f "${ROM8_BIN}" ] && [ -f "${ROME_BIN}" ]; then
    put_at "${ROM8_BIN}" "${APP_BASE}"               "8.BIN" $((APP_BASE + 0x200000))
    put_at "${ROME_BIN}" $((APP_BASE + 0x200000))    "E.BIN" $((APP_BASE + 0x400000))
fi
# 字库区写 appdata (V1.0.89+ 从固件迁出, 运行时 esp_partition_mmap 读取)
put_at "${FONT_ZH_BIN}" "${FONT_ZH_ABS}" "font_zh.bin"   $((FONT_ZH_ABS + 0x80000))
put_at "${FONT_16_BIN}" "${FONT_16_ABS}" "font_zh16.bin" $((FONT_16_ABS + 0x40000))
if [ -f "${LAV_FONT_TMP}" ]; then
    put_at "${LAV_FONT_TMP}" "${FONT_LAV_ABS}" "lav_font.bin" $((FONT_LAV_ABS + 0x80000))
fi
# 迁移自固件的常量表槽位 (appdata 分区内, 64KB 对齐, 与 render_bp_lut.c 一致)
if [ -f "${BP_LUT_BIN}" ]; then
    put_at "${BP_LUT_BIN}" $((APP_BASE + 0x540000)) "bp_lut.bin" $((APP_BASE + 0x580000))
else
    echo "[警告] 缺少 bp_lut.bin, SMS 像素表槽留空: ${BP_LUT_BIN}"
fi
SZ_ADD_BIN="${PROJECT_DIR}/system_image/SZHVC_add.bin"
SZ_SUB_BIN="${PROJECT_DIR}/system_image/SZHVC_sub.bin"
if [ -f "${SZ_ADD_BIN}" ]; then
    put_at "${SZ_ADD_BIN}" $((APP_BASE + 0x580000)) "SZHVC_add.bin" $((APP_BASE + 0x5A0000))
else
    echo "[警告] 缺少 SZHVC_add.bin, 槽位留空: ${SZ_ADD_BIN}"
fi
if [ -f "${SZ_SUB_BIN}" ]; then
    put_at "${SZ_SUB_BIN}" $((APP_BASE + 0x5A0000)) "SZHVC_sub.bin" $((APP_BASE + 0x5C0000))
else
    echo "[警告] 缺少 SZHVC_sub.bin, 槽位留空: ${SZ_SUB_BIN}"
fi
SPIFFS_BIN="${BUILD_DIR}/apps_l.bin"
APPS_SRC="${PROJECT_DIR}/apps/lua"
# APPS_SIZE / APPS_OFF 已由上方 partitions.csv 解析得出
# ★★ 2026-10-14 linit: **默认不把 Lua 应用打进 16MB 固件** ——
#   Lua 应用走 TF 卡 / 应用商店**后装**, 固件里 apps 分区保持空 SPIFFS。
#   需要出"预置演示机/测试机"固件时才显式开:  LINTOS_BAKE_LUA_APPS=1 ./tools/build.sh
# ⚠ 陷阱 (2026-09-29 排查发现): 源目录不在时**不能默默沿用上一版 apps_l.bin** ——
#   那会把早已下架的应用/旧脚本一起烧回设备 (应用管家会多出"C版+Lua版"重复条目), 而且没有提示。
if [ "${LINTOS_BAKE_LUA_APPS:-0}" = "1" ] && [ -d "${APPS_SRC}" ]; then
    echo "[提示] LINTOS_BAKE_LUA_APPS=1 → 预置 Lua 应用进 apps 分区"
else
    echo "[提示] 默认不预置 Lua 应用 (要预置: LINTOS_BAKE_LUA_APPS=1 ./tools/build.sh) → 生成空 apps 镜像"
    APPS_SRC="$(mktemp -d)"
    APPS_TMPDIR="${APPS_SRC}"   # 交给顶部 cleanup trap 统一清理 (单独 trap 会覆盖掉临时镜像清理)
fi
# ★ 先删旧镜像: spiffsgen 失败时绝不能沿用上一版 apps_l.bin (会把已下架应用烧回设备)
rm -f "${SPIFFS_BIN}"
if ! "${PY}" "${SPIFFSGEN}" \
    --page-size 256 --block-size 4096 --meta-len 4 --obj-name-len 64 --use-magic \
    ${APPS_SIZE} "${APPS_SRC}" "${SPIFFS_BIN}"; then
    echo "[错误] spiffsgen 生成 apps 镜像失败: ${SPIFFS_BIN}"
    exit 1
fi
if [ ! -f "${SPIFFS_BIN}" ]; then
    echo "[错误] apps 镜像未生成: ${SPIFFS_BIN}"
    exit 1
fi
echo "apps_l.bin        $(printf '%6d' $(fsize "${SPIFFS_BIN}")) B  (从 $(basename "${APPS_SRC}") 生成)"
put_at "${SPIFFS_BIN}" "${APPS_OFF}" "apps(SPIFFS)" $((APPS_OFF + APPS_SIZE))
# app.db 走 nvs(core_db 命名空间), 无独立 raw 分区; apps(spiffs) 用 apps_l.bin 预置应用/测试包
# oui_db 已内嵌 macoui, 无 os_db 写入

# ---- 合并后静态结构检查 (2026-11-24) ----
# 只写进 bootloader + 分区表的残缺镜像, 大小同样是 16MB、SHA-256 也照常算得出来,
# 肉眼和"构建成功"的日志都分辨不出 → 必须由脚本自己验。四项硬指标:
#   ① 文件大小 == 16MB            ② 0x0 是 ESP 镜像 (0xE9)
#   ③ 0x8000 是分区表 (magic 0x50AA)  ④ factory 起点是 ESP 镜像 + 有效字节数不少于固件大小
"${PY}" - "${OUT_TMP}" "${FLASH_SIZE}" "${FACTORY_OFF}" "${APP_SIZE}" <<'PYEOF'
import sys
path = sys.argv[1]
flash, foff, asz = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
with open(path, 'rb') as f:
    d = f.read()
errs = []
if len(d) != flash:
    errs.append("镜像大小 %d != %d" % (len(d), flash))
if len(d) < 0x9002:
    errs.append("镜像过短 (%d B), 容不下 bootloader + 分区表" % len(d))
else:
    if d[0] != 0xE9:
        errs.append("0x0 不是 ESP 镜像 (首字节 0x%02X)" % d[0])
    magic = d[0x8000] | (d[0x8001] << 8)
    if magic != 0x50AA:
        errs.append("0x8000 不是分区表 (magic 0x%04X, 期望 0x50AA)" % magic)
    if foff >= len(d):
        errs.append("镜像短于 factory 偏移 (0x%X >= %d), 固件区根本不存在"
                    % (foff, len(d)))
    elif d[foff] != 0xE9:
        errs.append("0x%X 处没有固件 (magic 0x%02X) —— 这正是本次事故的特征"
                    % (foff, d[foff]))
used = len(d) - d.count(0xFF)
if used < asz:
    errs.append("有效字节 %d < 固件大小 %d → 数据没有真正落盘" % (used, asz))
if errs:
    for e in errs:
        print("[错误] 结构检查: " + e)
    sys.exit(1)
print("  结构检查通过: %d B, 有效字节 %d B" % (len(d), used))
PYEOF

# 只有全部检查通过才落到正式路径; 失败时上面任一步都已 exit, 正式镜像保持原样
mv -f "${OUT_TMP}" "${OUT_FILE}"
OUT_SIZE=$(fsize "${OUT_FILE}")
if command -v shasum >/dev/null 2>&1; then
    SHA=$(shasum -a 256 "${OUT_FILE}" | awk '{print $1}')
else
    SHA=$(sha256sum "${OUT_FILE}" | awk '{print $1}')
fi
echo ""
echo "[完成] 合并镜像: ${OUT_FILE}"
echo "  大小:     ${OUT_SIZE} 字节 ($((OUT_SIZE / 1048576)) MB)"
echo "  SHA-256:  ${SHA}"
echo ""
echo "烧录方式一 (esptool.py, 推荐):"
echo "  esptool.py --chip esp32s3 -b 460800 --before default_reset --after hard_reset \\"
echo "    write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \\"
echo "    0x0 ${OUT_FILE}"
