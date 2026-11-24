#!/usr/bin/env python3
"""tests/bt_txn/check_bt_txn.py — 蓝牙连接事务不变量静态检查

与 tests/static/*.py 不同, 本脚本是**判据**: 违反任一条即退出码 1。
它守的是 2026-11-23 R1 定下的边界, 都是靠代码形状而非运行时才能保证的:

  1. 连接事务单飞 —— bt_core_connect() 只有一个调用点, 且在 bt_connect_task 里
     (绕过 bt_connect_async 直接开连接 = 并发 open = "Already Connected")
  2. "连接中"状态唯一来源 —— s_connecting = true 只允许出现在 bt_connect_async 内
     (别处预置会在 BUSY 时留下"连接中却没有任务"的假状态)
  3. 调用方必须区分 STARTED / BUSY / ERR —— 不许再把返回值当 bool 用
  4. 拆栈前必须有事务守卫 —— bt_shutdown 里 s_conn_task_alive > 0 的延迟释放判断
     必须出现在任何 bt_core_deinit* 调用之前
  5. UI 取消不碰底层 —— bt_manager_cancel_connect 内不得出现 cancel_pending/disconnect
  6. 事后清理只在 open 返回后 —— bt_core_cancel_pending 的调用受 can_cleanup 控制
  7. 延迟释放有执行点 —— bt_release_flush 存在且被回连任务调用
  8. 释放条件覆盖收尾窗口 —— flush 同时判 alive 与 exiting (否则收尾代码可能被抢先拆栈)
  9. 单飞闸门的 check-then-act 在同一临界区内 (并发调用者不会同时通过)

用法: python3 tests/bt_txn/check_bt_txn.py [项目根]
退出码: 0 全部满足 / 1 有违反 / 2 环境错误(文件缺失)
"""
import os
import re
import sys

ROOT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else \
    os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MGR = os.path.join(ROOT, "components/services/bt_ctl/bt_manager.c")

fails = []
checks = 0


def strip_comments(src):
    """去掉 // 与 /* */ 注释, 保留换行(行号不变), 避免注释里的示例代码误命中。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                if src[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def line_of(src, pos):
    return src.count("\n", 0, pos) + 1


DEF_RE = r"(?m)^[ \t]*(?:static[ \t]+)?[A-Za-z_][A-Za-z0-9_]*[ \t\*]+"


def func_span(src, name):
    """返回 (起, 止) 偏移, 仅匹配**函数定义**(行首带返回类型 + 后面是 {)。
    调用点/前置声明都不算。找不到返回 None。"""
    m = re.search(DEF_RE + re.escape(name) + r"[ \t]*\([^;{]*\)[ \t\r\n]*\{", src)
    if not m:
        return None
    start = src.index("{", m.start())
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return (m.start(), i + 1)
    return None


def body_of(src, name):
    sp = func_span(src, name)
    return src[sp[0]:sp[1]] if sp else ""


def check(label, ok, detail=""):
    global checks
    checks += 1
    if not ok:
        fails.append(f"{label}: {detail}")
    print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + (f" — {detail}" if not ok else ""))


if not os.path.isfile(MGR):
    print(f"环境错误: 找不到 {MGR}")
    sys.exit(2)

src = strip_comments(open(MGR, "r", encoding="utf-8").read())

print("=========== bt_manager.c 连接事务不变量 ===========")

# ---- 1. bt_core_connect 只有一个调用点, 且在 bt_connect_task 内 ----
span_task = func_span(src, "bt_connect_task")
calls = list(re.finditer(r"\bbt_core_connect\s*\(", src))
in_task = [c for c in calls if span_task and span_task[0] <= c.start() < span_task[1]]
check("bt_core_connect 调用点唯一", len(calls) == 1,
      f"发现 {len(calls)} 处: 行 " + str([line_of(src, c.start()) for c in calls]))
check("bt_core_connect 只在 bt_connect_task 内", len(in_task) == 1,
      "有调用点跑到了任务之外(会阻塞调用者 / 绕过单飞闸门)")

# ---- 2. s_connecting = true 只出现在连接入口内, 且仅一次 ----
# ★ 2026-11-23 R2: 入口改名 bt_connect_async → bt_connect_async2 (扫描/连接互斥需要
#   区分"扫描自身命中"与"用户/回连发起"两种来源)。外面保留两个薄包装
#   bt_connect_async() / bt_connect_from_scan(), 真正的状态置位仍在入口本体里。
ENTRY = "bt_connect_async2" if func_span(src, "bt_connect_async2") else "bt_connect_async"
span_async = func_span(src, ENTRY)
sets = list(re.finditer(r"\bs_connecting\s*=\s*true\s*;", src))
outside = [s for s in sets if not (span_async and span_async[0] <= s.start() < span_async[1])]
check(f"s_connecting = true 只在 {ENTRY} 内",
      span_async is not None and len(sets) == 1 and not outside,
      ("行 " + str([line_of(src, s.start()) for s in outside])) if outside
      else f"出现 {len(sets)} 次(期望 1 次)")

# ---- 3. 每个 bt_connect_async 调用都检查了返回值 ----
bad = []
for m in re.finditer(r"\b(bt_connect_async|bt_connect_from_scan)\s*\(", src):
    pos = m.start()
    # 定义行 / 前置声明行: 行首带返回类型
    lstart = src.rfind("\n", 0, pos) + 1
    line = src[lstart:src.find("\n", pos)]
    if re.match(r"^[ \t]*(?:static[ \t]+)?int[ \t]+(?:bt_connect_async2?|bt_connect_from_scan)[ \t]*\(", line):
        continue
    # 该调用所在语句: 往前后各取一小段
    semi = src.find(";", pos)
    tail = src[pos:semi + 1] if semi > 0 else ""
    head = src[max(0, lstart - 60):pos]
    if "BT_CONN_STARTED" in tail or "BT_CONN_BUSY" in tail or re.search(r"=[ \t]*$", head):
        continue
    bad.append(f"行 {line_of(src, pos)}")
check("bt_connect_async 调用检查了三种结果", not bad, ", ".join(bad))

# ---- 4. bt_shutdown 的延迟释放守卫在 deinit 之前 ----
body_shut = body_of(src, "bt_shutdown")
guard = body_shut.find("s_conn_task_alive > 0")
deinits = [p for p in (body_shut.find("bt_core_deinit_deep("),
                       body_shut.find("bt_core_deinit(")) if p >= 0]
check("bt_shutdown: 事务守卫先于任何拆栈",
      guard >= 0 and deinits and guard < min(deinits),
      "缺少 s_conn_task_alive > 0 的延迟释放判断, 或它出现在 deinit 之后")

# ---- 5. UI 取消不再碰底层 ----
body_cancel = body_of(src, "bt_manager_cancel_connect")
check("bt_manager_cancel_connect 不碰底层",
      bool(body_cancel) and "cancel_pending" not in body_cancel
      and "bt_core_disconnect" not in body_cancel,
      "仍在调用 cancel_pending/disconnect (建立中取消无效, 还会打断正常收敛)")

# ---- 6. 事后清理受 can_cleanup 控制 ----
idx = src.find("bt_core_cancel_pending(")
pre = src[max(0, idx - 1200):idx] if idx >= 0 else ""
check("bt_core_cancel_pending 受 can_cleanup 门控",
      idx >= 0 and "can_cleanup" in pre,
      "清理调用不在 can_cleanup 分支内(可能在事务进行中动链路)")

# ---- 7. 延迟释放有执行点 ----
span_rec = func_span(src, "bt_reconnect_task")
check("存在 bt_release_flush 执行点",
      func_span(src, "bt_release_flush") is not None
      and span_rec is not None
      and "bt_release_flush();" in src[span_rec[0]:span_rec[1]],
      "挂起的关蓝牙/拆栈没有执行点 ⇒ 内存永不归还")

# ---- 8. 释放条件覆盖"任务已退出但收尾未完成"的窗口 ----
body_flush = body_of(src, "bt_release_flush")
check("bt_release_flush 同时判 alive 与 exiting",
      "s_conn_task_alive > 0" in body_flush and "s_conn_task_exiting" in body_flush,
      "只看 alive ⇒ 收尾代码还在跑时可能被抢先拆栈(UAF)")

# ---- 9. 单飞闸门在临界区内完成 check-then-act ----
body_async2 = body_of(src, ENTRY)
gate = body_async2.find("s_conn_task_alive > 0")
crit = body_async2.find("taskENTER_CRITICAL")
check("单飞闸门在临界区内",
      gate > 0 and crit > 0 and crit < gate and "taskENTER_CRITICAL" in body_async2[:gate],
      "检查与占位不在同一临界区 ⇒ 并发调用者可能同时通过")

print()
if fails:
    print(f"结论: {checks - len(fails)}/{checks} 项通过, {len(fails)} 项违反")
    for f in fails:
        print("  - " + f)
    sys.exit(1)
print(f"结论: {checks}/{checks} 项全部通过")
