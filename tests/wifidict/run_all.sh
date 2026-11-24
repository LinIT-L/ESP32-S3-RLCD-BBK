#!/bin/sh
# tests/wifidict/run_all.sh — Wi-Fi 字典 LPK 的主机侧功能自测
#
# 用 lupa 跑**真实的 apps/lua/lpk/wifidict/app/entry.lua**, 把 app.* 桥 mock 掉
# (fs 用内存 FAT)。覆盖: 内置字典/分批条目、TXT 导入(UTF-8 BOM / LF / CRLF /
# 空行 / 去重 / 中文 / 末尾无换行)、大字典分批读写、随机生成的字符集与长度、
# 预览分页的行号连续性与末尾、以及"源码不得调用任何网络/WiFi 接口"的审计。
#
# 依赖: python3 + lupa (pip install lupa)。缺 lupa 时脚本自身 exit 2 ⇒ SKIP。
#
# 退出码: 0=PASS, 1=FAIL, 2=SKIP(环境缺失)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1

PY=${PY:-python3}
PY_SCRIPT=tests/wifidict/host_check.py
ENTRY=apps/lua/lpk/wifidict/app/entry.lua

command -v "$PY" >/dev/null 2>&1 || { echo "SKIP: 找不到 python3"; exit 2; }
[ -f "$PY_SCRIPT" ] || { echo "SKIP: 缺少 $PY_SCRIPT"; exit 2; }
[ -f "$ENTRY" ] || { echo "SKIP: 缺少被测 LPK $ENTRY (LPK 未生成时不伪称通过)"; exit 2; }

# lupa 缺失时 host_check.py 自己会 exit 2, 这里先探一次给出更清楚的原因
if ! "$PY" -c "import lupa" >/dev/null 2>&1; then
    echo "SKIP: 缺 lupa (pip install lupa) —— 本套件需要能跑 Lua 的 Python 绑定"
    exit 2
fi

exec "$PY" "$PY_SCRIPT" "$ENTRY"
