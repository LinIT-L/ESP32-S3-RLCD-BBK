#!/usr/bin/env python3
"""tests/bt_scan/check_bt_scan.py — 蓝牙扫描事务 / 档案 / 日志 不变量静态检查

与 tests/bt_txn 同风格: 纯静态、纯只读, 违反任一条即退出码 1。
它守的是 2026-11-23 R2 定下的边界 (靠代码形状而非运行时才能保证的部分):

  1. 扫描闸门 check-then-act 在同一临界区 (scan_begin)
  2. 三个扫描入口 (任务式长扫 / 按名字连接 / 按名字短扫) 都过同一个闸门
  3. 连接事务在飞时拒绝起扫 (s_open_tx) —— esp_hidh_dev_open 期间不许抢 GAP
  4. bt_core_stop_scan 不放开闸门 (只清"想要结果"的意图, 否则并发扫描回来)
  5. 扫描任务退出顺序: on_scan_done → scan_release → on_scan_idle
  6. 等扫描完成事件必须有上限 (不得再出现 portMAX_DELAY 的 BLE 等待)
  7. 深拆栈前等扫描事务退出 (bt_core_scan_wait_idle 在 esp_hidh_deinit 之前)
  8. bt_shutdown 的延迟释放条件覆盖扫描事务, 且在任何拆栈之前
  9. bt_release_flush 同样等扫描退出
 10. on_scan_idle 有实现且被注册 (扫描收尾后执行被推迟的释放)
 11. 连接记录只在真正连上后写 (发起连接处不得写)
 12. 扫描逐条日志已降级 (DEVICE 行非 INFO, GAP_DBG_PRINTF 非裸 printf)
 13. 主动断开/挂起/关电 的意图在断开回调与连接回调里都被尊重
 14. 用户点选连接前先等扫描收尾 (from_scan 例外)
 15. 停止扫描时清掉回调 (否则"用户正在扫描"永远为真, 深拆栈永不执行)

用法: python3 tests/bt_scan/check_bt_scan.py [项目根]
退出码: 0 全部满足 / 1 有违反 / 2 环境错误(文件缺失)
"""
import os
import re
import sys

ROOT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else \
    os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BT = os.path.join(ROOT, "components/services/bt_ctl")
MGR = os.path.join(BT, "bt_manager.c")
CORE = os.path.join(BT, "bt_core.c")
GAP = os.path.join(BT, "esp_hid_gap.c")
HDR = os.path.join(BT, "include/bt_core.h")

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
    """返回函数定义 (起, 止) 偏移; 只匹配行首带返回类型、后面跟 { 的定义。"""
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


for p in (MGR, CORE, GAP, HDR):
    if not os.path.isfile(p):
        print(f"环境错误: 找不到 {p}")
        sys.exit(2)

mgr = strip_comments(open(MGR, "r", encoding="utf-8").read())
core = strip_comments(open(CORE, "r", encoding="utf-8").read())
gap = strip_comments(open(GAP, "r", encoding="utf-8").read())
hdr = strip_comments(open(HDR, "r", encoding="utf-8").read())

print("=========== 蓝牙扫描事务 / 档案 / 日志 不变量 (R2) ===========")

# ---- 1. 扫描闸门: 临界区内 check-then-act ----
b = body_of(core, "scan_begin")
crit = b.find("taskENTER_CRITICAL")
busy_chk = b.find("s_scan_busy")
grant = b.find("s_scan_busy = true")
check("scan_begin 在临界区内完成 check-then-act",
      bool(b) and crit >= 0 and busy_chk > crit and grant > busy_chk,
      "缺少临界区, 或'检查→占位'不在同一临界区(并发调用者会同时通过)")

# ---- 2. 三个扫描入口都过同一闸门 ----
entries = ["bt_core_scan", "bt_core_connect_by_name", "bt_core_scan_find_by_name"]
bad = [e for e in entries if "scan_begin(" not in body_of(core, e)]
check("三个扫描入口都过扫描闸门", not bad, "未过闸门的入口: " + ", ".join(bad))

# ---- 3. 连接事务在飞时拒扫 ----
check("闸门拒绝'连接事务在飞'时的扫描",
      "s_open_tx" in body_of(core, "scan_begin"),
      "scan_begin 未检查 s_open_tx ⇒ open 期间仍可能起扫(抢 GAP 状态机)")
b_conn = body_of(core, "bt_core_connect")
check("bt_core_connect 登记连接事务区间",
      "open_tx_enter(" in b_conn and "open_tx_leave(" in b_conn,
      "没有 open_tx 计数 ⇒ 闸门无法判断连接事务是否在飞")

# ---- 4. stop_scan 不放开闸门 ----
b_stop = body_of(core, "bt_core_stop_scan")
check("bt_core_stop_scan 不放开扫描闸门",
      bool(b_stop) and "s_scan_busy = false" not in b_stop,
      "stop_scan 里清了 s_scan_busy ⇒ 第二个扫描线程可立刻进 esp_hid_scan (并发回归)")

# ---- 5. 扫描任务退出顺序 ----
b_task = body_of(core, "scan_task")
i_done = b_task.find("on_scan_done")
i_rel = b_task.find("scan_release()")
i_idle = b_task.find("on_scan_idle")
check("扫描任务: 回调 → 放闸门 → 通知空闲",
      i_done > 0 and i_rel > i_done and i_idle > i_rel,
      "顺序错 ⇒ 上层还在读结果链表时闸门已放开(可被下一轮 free), 或释放无人通知")

# ---- 6. 等完成事件必须有上限 ----
check("BLE 扫描等待不再用 portMAX_DELAY",
      "xSemaphoreTake(ble_hidh_cb_semaphore, portMAX_DELAY)" not in gap
      and "wait_ble_cb(" in gap,
      "无限等待 ⇒ 完成事件一丢, 扫描任务永久卡住并占着信号量与栈")

# ---- 7. 深拆栈前等扫描退出 ----
b_deep = body_of(core, "bt_core_full_deinit")
i_wait = b_deep.find("bt_core_scan_wait_idle")
i_hidh = b_deep.find("esp_hidh_deinit")
check("深拆栈前等扫描事务退出",
      i_wait >= 0 and i_hidh > i_wait,
      "扫描任务还卡在 esp_hid_scan 里就 deinit ⇒ 醒来即 use-after-free")

# ---- 8. bt_shutdown 延迟释放覆盖扫描 ----
b_shut = body_of(mgr, "bt_shutdown")
guard = b_shut.find("bt_core_scan_busy()")
deinits = [p for p in (b_shut.find("bt_core_deinit_deep("),
                       b_shut.find("bt_core_deinit(")) if p >= 0]
check("bt_shutdown: 扫描未收尾时推迟释放, 且守卫先于拆栈",
      guard >= 0 and deinits and guard < min(deinits),
      "缺少 bt_core_scan_busy() 守卫, 或它出现在拆栈之后")

# ---- 9. release_flush 也等扫描 ----
check("bt_release_flush 等扫描事务退出",
      "bt_core_scan_busy()" in body_of(mgr, "bt_release_flush"),
      "只看连接事务 ⇒ 扫描还在跑时就把栈拆了")

# ---- 10. on_scan_idle 实现并注册 ----
check("on_scan_idle 有实现且被注册",
      func_span(mgr, "on_scan_idle_cb") is not None
      and ".on_scan_idle" in mgr and "on_scan_idle" in hdr,
      "扫描收尾后没人唤醒被推迟的释放 ⇒ 内存/射频迟迟不归还")

# ---- 11. 连接记录只在真正连上后写 ----
b_cd = body_of(mgr, "bt_manager_connect_device")
b_conn_cb = body_of(mgr, "on_connected_cb")
check("发起连接时不写连接记录",
      "bt_manager_add_to_history" not in b_cd,
      "连接失败也会新增/顶替记录, 还会立刻成为自动回连目标")
check("连接记录在 on_connected_cb 里写",
      b_conn_cb.count("bt_manager_add_to_history") == 1,
      f"on_connected_cb 内出现 {b_conn_cb.count('bt_manager_add_to_history')} 次(期望 1 次)")

# ---- 12. 逐条设备日志已降级 ----
bad_dev = []
for m in re.finditer(r"(?m)^.*DEVICE:.*$", gap):
    if "ESP_LOGI" in m.group(0):
        bad_dev.append(line_of(gap, m.start()))
check("扫描逐条 DEVICE 日志不是 INFO", not bad_dev,
      "仍在 INFO 级: 行 " + str(bad_dev))
mdbg = re.search(r"#define[ \t]+GAP_DBG_PRINTF\(\.\.\.\)[ \t]+([A-Za-z_]+)", gap)
check("GAP_DBG_PRINTF 不是裸 printf",
      mdbg is not None and mdbg.group(1) != "printf",
      "裸 printf 不受日志级别控制, 会按设备刷屏")

# ---- 13. 主动断开意图被尊重 ----
need = {
    "bt_manager_disconnect": "s_user_disconnect",
    "on_disconnected_cb": "s_user_disconnect",
    "on_connected_cb": "s_user_disconnect",
}
bad = [f"{k}" for k, v in need.items() if v not in body_of(mgr, k)]
check("主动断开/挂起/关电意图在三处都被尊重", not bad,
      "未处理: " + ", ".join(bad) + " (会出现'断开后又自己连上'或'连上不该连的设备')")

# ---- 14. 用户点选连接前等扫描收尾 ----
b_async = body_of(mgr, "bt_connect_async2")
check("连接入口先等扫描事务收尾 (扫描自身命中的例外)",
      "bt_core_scan_wait_idle" in b_async and "!from_scan" in b_async,
      "扫描用着 GAP 状态机时直接 open ⇒ 两边都容易失败")

# ---- 15. 停止扫描时清回调 ----
b_stopm = body_of(mgr, "bt_manager_stop_scan")
check("bt_manager_stop_scan 清掉扫描回调",
      "s_scan_cb = NULL" in b_stopm and "s_cont_cb = NULL" in b_stopm,
      "回调残留 ⇒ '用户正在用蓝牙'永远为真, 开机窗口超时不拆栈、连败也不重建")

# ---- 附加: 扫描启动返回值被检查 ----
check("bt_core_scan 返回 bool 且调用点检查结果",
      "bool bt_core_scan(" in hdr and "scan_start_common" in mgr
      and "if (!bt_core_scan(5))" in mgr,
      "core 拒绝起扫时管理器仍置'正在扫描' ⇒ UI 只能等 15s 看门狗自愈")

print()
if fails:
    print(f"结论: {checks - len(fails)}/{checks} 项通过, {len(fails)} 项违反")
    for f in fails:
        print("  - " + f)
    sys.exit(1)
print(f"结论: {checks}/{checks} 项全部通过")
