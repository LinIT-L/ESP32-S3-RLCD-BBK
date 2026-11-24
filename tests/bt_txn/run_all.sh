#!/bin/sh
# 蓝牙连接事务不变量 —— 静态回归套件
#
# 守 2026-11-23 R1 定的边界: 单飞 / 状态唯一来源 / 返回值三态 / 拆栈守卫 /
# UI 取消不碰底层 / 事后清理门控 / 延迟释放有执行点。
# 纯静态 + 纯只读: 不编译固件、不改文件。
#
# 用法: sh tests/bt_txn/run_all.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
python3 tests/bt_txn/check_bt_txn.py "$ROOT"
