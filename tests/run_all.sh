#!/bin/sh
# tests/run_all.sh — LinTOS 主机侧测试统一入口
#
# 一次跑完全部可在电脑上执行的测试。设计约束:
#   · 某项缺依赖时**跳过并明确报告**, 绝不算通过;
#   · 任一套件失败 → 整体返回非零;
#   · 每套件输出 PASS / FAIL / SKIP;
#   · 结束时打印 总数、失败清单、跳过清单;
#   · 所有输出落盘到 tests/.logs/<时间戳>/ (不污染仓库, 已由 .gitignore 忽略或手动清理);
#   · 本脚本**只读**生产代码: 不编译固件、不烧录、不改文件、不提交 Git。
#
# 用法:
#   sh tests/run_all.sh              # 跑全部
#   sh tests/run_all.sh os_file lpk  # 只跑指定套件
#   QUIET=1 sh tests/run_all.sh      # 只打印汇总
#
# 退出码: 0=全部通过(可含 SKIP)  1=有失败  2=环境不可用(全跳过)
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

QUIET=${QUIET:-0}
STAMP=$(date +%Y%m%d_%H%M%S)
LOGDIR="$ROOT/tests/.logs/run_$STAMP"
mkdir -p "$LOGDIR" || { echo "无法创建日志目录 $LOGDIR"; exit 2; }

# ---- 套件清单 (顺序 = 由底向上: 公共层 → 服务 → 页面/引擎 → 历史套件) ----
#
# ⛔ 故意**未列入** tests/lua_sandbox: 该套件 55 项里有 29 项断言的是
#   2026-11-02 "统一放开 Lua 应用权限"**之前**的策略(摘库 / 路径白名单 /
#   deny hid|wiki|rec / 夹紧阻塞 API)。这些行为已按 linit 决定移除,
#   跑它只会得到 29 个"过期失败", 误导后来者去"修"一个不该存在的功能。
#   保留源码作为历史证据, 不纳入统一入口。详见 docs/测试/ 文档 §4。
ALL_SUITES="os_file dl_replace mem_sim backup sd_vfs input_guard \
lua_quota lua_guard lua_life emu_save engine_mgr logic_units ui_scroll_hit \
app_core app_registry store_state installer lpk asset wiki wifidict server \
statusbar recorder bt_txn bt_scan"

if [ "$#" -gt 0 ]; then
    SUITES="$*"
else
    SUITES="$ALL_SUITES"
fi

# ---- 环境预检 ----
CC=${CC:-/Library/Developer/CommandLineTools/usr/bin/clang}
CXX=${CXX:-/Library/Developer/CommandLineTools/usr/bin/clang++}
PY=python3
ENV_OK=1
[ -x "$CC" ] || { echo "⚠ 未找到 C 编译器: $CC"; ENV_OK=0; }
[ -x "$CXX" ] || { echo "⚠ 未找到 C++ 编译器: $CXX"; ENV_OK=0; }
command -v "$PY" >/dev/null 2>&1 || { echo "⚠ 未找到 python3"; ENV_OK=0; }

echo "=============================================================="
echo "LinTOS 主机侧测试"
echo "项目根 : $ROOT"
echo "日志   : $LOGDIR"
echo "编译器 : $CC"
[ "$ENV_OK" -eq 1 ] && echo "环境   : OK" || echo "环境   : 不完整 (相关套件将 SKIP)"
echo "=============================================================="

PASS=0; FAIL=0; SKIP=0
FAIL_LIST=""; SKIP_LIST=""

for s in $SUITES; do
    SCRIPT="tests/$s/run_all.sh"
    LOG="$LOGDIR/$s.log"

    if [ ! -f "$SCRIPT" ]; then
        printf "  %-14s SKIP  (无 %s)\n" "$s" "$SCRIPT"
        SKIP=$((SKIP + 1)); SKIP_LIST="$SKIP_LIST $s(无脚本)"
        continue
    fi

    # 先落日志再回显: POSIX sh 没有 PIPESTATUS, 用 tee 管道会丢掉退出码,
    # 所以只跑一次、写文件、再 cat。测试总时长在分钟级, 不追求实时刷屏。
    sh "$SCRIPT" > "$LOG" 2>&1
    rc=$?
    if [ "$QUIET" -eq 0 ]; then
        echo ""
        cat "$LOG"
    fi

    case "$rc" in
        0) printf "  %-14s PASS\n" "$s"; PASS=$((PASS + 1)) ;;
        2) printf "  %-14s SKIP  (环境缺失, 见 %s)\n" "$s" "$LOG"
           SKIP=$((SKIP + 1)); SKIP_LIST="$SKIP_LIST $s(环境)" ;;
        *) printf "  %-14s FAIL  (rc=%d, 见 %s)\n" "$s" "$rc" "$LOG"
           FAIL=$((FAIL + 1)); FAIL_LIST="$FAIL_LIST $s" ;;
    esac
done

# ---- 静态检查 (信息性, 不计入 PASS/FAIL) ----
STATIC_NOTE="未执行"
if [ "$ENV_OK" -eq 1 ]; then
    $PY tests/static/lifecycle_check.py "$ROOT" > "$LOGDIR/static_lifecycle.log" 2>&1
    $PY tests/static/fs_recover_check.py "$ROOT" > "$LOGDIR/static_fs_recover.log" 2>&1
    STATIC_NOTE="$LOGDIR/static_lifecycle.log, $LOGDIR/static_fs_recover.log"
fi

TOTAL=$((PASS + FAIL + SKIP))

echo ""
echo "=============================================================="
echo "汇总: 共 $TOTAL 套, PASS=$PASS  FAIL=$FAIL  SKIP=$SKIP"
if [ -n "$FAIL_LIST" ]; then
    echo "失败  :$FAIL_LIST"
fi
if [ -n "$SKIP_LIST" ]; then
    echo "未执行:$SKIP_LIST"
fi
echo "静态检查(仅提示, 需人工核实): $STATIC_NOTE"
echo "日志  : $LOGDIR"
echo "=============================================================="
echo "说明: 本入口不编译固件 / 不烧录 / 不修改生产代码 / 不提交 Git。"
echo "      真机项目见 docs/测试/LinTOS-主机测试与真机回归.md"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
if [ "$PASS" -eq 0 ] && [ "$SKIP" -gt 0 ]; then
    exit 2
fi
exit 0
