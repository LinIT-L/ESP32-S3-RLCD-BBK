#!/bin/sh
# tests/recorder —— 录音机 Lua 脚本主机测试
#
# 直接加载生产脚本 store/pkgs/录音机.lua, 用 tests/recorder/harness.lua 提供 app.* 桩。
# 覆盖: 状态栏声明/绘制不越界、4 个操作按钮热区、删除确认(确认/取消/未选中)、
#       播放中不可误删、按键路径、退出清理。
#
# 依赖: python3 + lupa（pip install lupa）。缺依赖时 SKIP（退出码 2），绝不算通过。
# 退出码: 0=PASS, 1=FAIL, 2=SKIP
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

python3 -c "import lupa" 2>/dev/null || { echo "SKIP: 缺 lupa —— pip install lupa 后可跑"; exit 2; }
python3 tests/recorder/test_recorder.py
