#!/bin/bash
# ESP-IDF v6.0.3 一键构建脚本 (LinTOS 2.0)
# 从 v5.5.5 迁移到 v6.0.3 —— 1.0 正式版基线即 IDF 6 迁移版
# 工具链: GCC esp-15.2.0_20251204 (v6.0 要求)
#
# 可选环境变量覆盖 (默认值针对作者机器, 换机器时设置即可):
#   IDF_PATH             ESP-IDF 安装路径 (默认 ~/esp/esp-idf-v60)
#   IDF_PYTHON_ENV_PATH  ESP-IDF Python 虚拟环境 (默认 ~/.espressif/python_env/idf6.0_py3.11_env)
#   TOOLCHAIN_BIN_DIR    工具链 bin 目录 (默认 ~/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin)
#   SYS_ROM_DIR          系统 ROM 来源目录, 含 4988.font / 0E00.DAT (可选, 缺省时跳过组装 system.bin)

export IDF_SKIP_CHECK_SUBMODULES=1
export ESP_ROM_ELF_DIR="${HOME}/.espressif/tools/esp-rom-elfs/20260528"

# 项目根目录 (脚本所在)
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="${PROJECT_DIR}:${PYTHONPATH}"

# === v6.0.3 工具链 + Python 环境 (LinTOS 2.0) ===
# v6.0.3 工具链: esp-15.2.0_20251204, 与 v5.5 的 esp-14.2.0 不通用
# v6.0.3 Python 环境: idf6.0_py3.11_env (与 v5.5 的 py3.13 环境完全隔离)
IDF_PATH="${IDF_PATH:-${HOME}/esp/esp-idf-v60}"
IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-${HOME}/.espressif/python_env/idf6.0_py3.11_env}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-${HOME}/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin}"
export PATH="${TOOLCHAIN_BIN_DIR}:${IDF_PYTHON_ENV_PATH}/bin:${PATH}"
export IDF_PATH
export IDF_PYTHON_ENV_PATH
# ★ 2026-10-30: 必须显式给版本号 —— 直接设 IDF_PATH(不走 export.sh)时 idf.py 拿不到
#   ESP_IDF_VERSION, idf_component_manager 会在 CLI 初始化处抛
#   `TypeError: expected string or bytes-like object, got 'NoneType'` 然后退出。
#   更坑的是脚本随后仍会用**旧产物**继续合并并打印 "Build completed", 看起来像编译成功。
export ESP_IDF_VERSION="${ESP_IDF_VERSION:-6.0.3}"

# ★ 2026-10-17: 构建前自动把商店索引(store/index.json)编译进固件 ——
#   "离线商店"依赖它; 索引来自各应用 LAPP 头, 不含 Lua 本体(应用仍按需从商店下载)。
#   重建商店索引(若 store/pkgs 有增删)后无需手工操作, 这里自动同步。
# ★ 2026-10-29: store/ 不进仓库(.gitignore), 新克隆/换机器上它必然缺失。
#   原实现的 "if -f ... fi" 在缺失时**什么都不生成** ⇒ 编译期直接
#   fatal error: store_index_builtin.h: No such file or directory, 且报错完全看不出原因。
#   现在缺失时生成一份**空索引**, 并把它到底意味着什么说清楚:
#     · 语义已核实: am_store_json_parse("") 取不到 mirrors ⇒ 返回 -1(am_store_json.c:156),
#       调用方 am_store_net.c:132 只打一条 W 就继续, 随后照常按 os_sources.c 里的
#       内置源联网拉 /index.json ⇒ 只是"离线商店没有内置应用", 不影响开机/联网/其它功能。
#     · 想真正内置离线商店, 必须先准备 store/index.json(tools/store_build.py 生成)。
if [ -f store/index.json ]; then
    python3 - <<'PYEOF' 2>/dev/null || true
src = open("store/index.json", encoding="utf-8").read()
esc = src.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
open("components/apps/pages/store_index_builtin.h", "w", encoding="utf-8").write(
    '/* store_index_builtin.h — 固件内置商店索引(离线可用), 自动生成 */\n'
    '#pragma once\nstatic const char STORE_INDEX_BUILTIN[] =\n"%s";\n' % esc)
PYEOF
    if [ ! -f components/apps/pages/store_index_builtin.h ]; then
        echo "[错误] store/index.json 存在, 但内置索引生成失败(见上面的 python 报错)"
        echo "       缺该头文件 → 编译必在 am_store_net.c 处 fatal error, 故此处直接中止。"
        exit 1
    fi
else
    echo "[警告] 未找到 store/index.json —— 生成**空**内置商店索引"
    echo "       影响: 固件可正常编译/运行; 但离线商店没有内置应用, 首次开机需联网拉索引。"
    echo "       要内置离线商店: 先准备 store/index.json (tools/store_build.py 生成) 再构建。"
    mkdir -p components/apps/pages
    cat > components/apps/pages/store_index_builtin.h <<'HEOF'
/* store_index_builtin.h — 固件内置商店索引(离线可用), 自动生成 */
/* ★ 空索引: 构建时未找到 store/index.json。
 *   运行时 am_store_json_parse("") 返回 -1(无 mirrors), 调用方只打一条 W,
 *   随后照常按 os_sources.c 的内置源联网拉索引 —— 不崩、不阻塞开机。 */
#pragma once
static const char STORE_INDEX_BUILTIN[] = "";
HEOF
fi

if [ ! -f "${IDF_PATH}/tools/idf.py" ]; then
    echo "[错误] 未找到 ESP-IDF: ${IDF_PATH}"
    echo "       请安装 ESP-IDF v5.5+ 并设置 IDF_PATH 环境变量"
    exit 1
fi

echo "Building with ESP-IDF ${ESP_IDF_VERSION} (${IDF_PATH})"
echo "Building project: ${PROJECT_DIR}"
cd "${PROJECT_DIR}"

# === 清理: 必须可重复执行 (2026-10-31 修) ===================================
# 历史 bug: 原实现无条件 `idf.py fullclean`。它在**已经清理过**的目录上会失败,
#   失败后留在半损状态: build/CMakeFiles 已被删、build.ninja 还在 ⇒ 下一次 build 报
#     ninja: error: loading 'build.ninja': ... CMakeFiles/rules.ninja: No such file
#   而脚本当时**不检查返回码**, 继续往下走并合并旧产物, 最后还打印
#   "Build completed!" —— 看起来像成功, 实际是上一次的固件。发版前必踩, 已修掉。
#
# 现在只做**最小必要清理**: CMake 三个状态文件缺任何一个就重置配置。
#   注意 CMakeFiles/ 消失 = 所有 .o 也没了 ⇒ 重新配置等价于全量重编,
#   不需要再画蛇添足跑 fullclean (它才是坏状态的来源)。
#   要真正从头来一遍: 手动 `rm -rf build` 后重跑本脚本。
NEED_CFG=0
[ -f build/CMakeCache.txt ]         || NEED_CFG=1
[ -f build/build.ninja ]            || NEED_CFG=1
[ -f build/CMakeFiles/rules.ninja ] || NEED_CFG=1
if [ "${NEED_CFG}" = "1" ] && [ -d build ]; then
    echo "[清理] CMake/ninja 状态不完整 → 重置配置 (将全量重编)"
    rm -f build/CMakeCache.txt build/build.ninja
fi

if ! python3 "${IDF_PATH}/tools/idf.py" build; then
    echo ""
    echo "[错误] 编译失败 —— 已中止, **不会**合并产物。"
    echo "       请修掉上面的编译错误后重试; 不要用上一次的 build/LinTOS.bin 发版。"
    exit 1
fi

if [ ! -f build/LinTOS.bin ]; then
    echo "[错误] 编译流程结束但 build/LinTOS.bin 不存在, 中止。"
    exit 1
fi

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
ls -la "${PROJECT_DIR}/build/LinTOS.bin"
ls -la "${PROJECT_DIR}/dist/merged_16mb.bin"
