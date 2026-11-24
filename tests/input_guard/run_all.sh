#!/bin/sh
# tests/input_guard/run_all.sh — 按键 pad 自愈/解锁/静音契约
#
# 只跑源码契约检查。输入状态机的**运行时行为**（电平、时序、浅睡唤醒后
# pad 是否真的恢复）依赖真实 GPIO 与电源管理硬件，主机无法替代，
# 那部分只能上真机 —— 见 docs/测试 的真机回归清单。
set -u
cd "$(dirname "$0")" || exit 2

python3 contract_check.py "$@"
rc=$?

case $rc in
0) echo "INPUT_GUARD: PASS" ;;
1) echo "INPUT_GUARD: FAIL" ;;
*) echo "INPUT_GUARD: SKIP (源文件缺失或环境不可用)" ;;
esac
exit $rc
