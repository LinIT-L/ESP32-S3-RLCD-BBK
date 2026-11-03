#!/bin/bash
# ============================================================================
# lavax_vm_bench.sh — LavaX VM 自动测试 + 基准（esp-emu 上真实执行）
#
#   ./tools/lavax_vm_bench.sh              # 构建 + 跑 + 打印结果
#   ./tools/lavax_vm_bench.sh 180s         # 指定模拟器超时（默认 120s）
#   ./tools/lavax_vm_bench.sh 120s out.log # 指定超时 + 日志文件
#
# 为什么走 esp-emu 而不是主机编译：
#   本机 /usr/bin/clang 与 Homebrew gcc 都被 Xcode 许可拦住（brew 同样），
#   主机侧无法编译任何 C 代码。esp-emu 是预编译的模拟器，不依赖主机工具链，
#   而且跑的是**真实的 Xtensa 机器码**，比主机 x86 编译更接近真机行为。
#
# 产物：
#   build_emu/emu_flash.bin   带 LINTOS_LAVAX_BENCH=1 的 16MB 镜像
#   build_emu/bench.log       本次串口完整输出
#
# 判定：
#   退出码 0 = 串口出现 "BENCH DONE"；1 = 超时或构建失败。
# ============================================================================
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "${PROJECT_DIR}"

TIMEOUT="${1:-120s}"
LOG="${2:-build_emu/bench.log}"
EMU="${HOME}/.local/bin/esp-emu"

if [ ! -x "${EMU}" ]; then
    echo "[错误] 未找到 esp-emu: ${EMU}"
    echo "       安装: https://github.com/espressif/esp-emulator"
    exit 1
fi

echo "=== [1/2] 构建 (LINTOS_LAVAX_BENCH=1, 不影响正式固件) ==="
LINTOS_EXTRA_CMAKE="-DLINTOS_LAVAX_BENCH=1" bash tools/emu_build.sh > build_emu/bench_build.log 2>&1 \
    || { echo "[错误] 构建失败，见 build_emu/bench_build.log"; tail -20 build_emu/bench_build.log; exit 1; }
echo "    构建完成: build_emu/emu_flash.bin"

echo "=== [2/2] 运行 (esp-emu, 超时 ${TIMEOUT}) ==="
mkdir -p "$(dirname "${LOG}")"
set +e
"${EMU}" --chip esp32s3 --firmware build_emu/emu_flash.bin \
    --exit-on "BENCH DONE" --timeout "${TIMEOUT}" 2>&1 | tee "${LOG}"
RC=${PIPESTATUS[0]}
set -e

echo
echo "=== 结果 (只显示 BENCH 行) ==="
grep -E "BENCH|PASS|FAIL|PERF" "${LOG}" || echo "(无 BENCH 输出 —— 见 ${LOG})"

if grep -q "BENCH DONE" "${LOG}"; then
    echo "=== 完成，日志: ${LOG} ==="
    exit 0
fi
echo "=== 未跑完（超时/崩溃），日志: ${LOG} ==="
exit 1
