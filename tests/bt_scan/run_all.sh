#!/bin/sh
# 蓝牙扫描事务 / 档案 / 日志 不变量 —— 静态回归套件 (R2)
#
# 守 2026-11-23 R2 定的边界: 扫描单飞 / 扫描与连接互斥 / 扫描生命周期有界收尾 /
# 拆栈前等扫描退出 / 连接记录只在连成功后写 / 逐条设备日志降级 /
# 主动断开与挂起不被自动回连覆盖 / 停止扫描清回调。
# 纯静态 + 纯只读: 不编译固件、不改文件。
#
# 用法: sh tests/bt_scan/run_all.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
python3 tests/bt_scan/check_bt_scan.py "$ROOT"
