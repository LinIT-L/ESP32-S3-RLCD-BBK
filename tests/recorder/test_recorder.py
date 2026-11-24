#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/recorder/test_recorder.py —— 录音机 UI / 长按删除 / 删除交互主机测试

直接加载生产脚本 store/pkgs/录音机.lua, app.* 由 harness.lua 提供桩。
覆盖:
  · 状态栏由脚本声明 (app.statusbar 真实被调用), 且任何绘制都不进 0..23
  · 列表顶到状态栏下、无「录音机 / N 条」标题行、默认不选中
  · 底部只有一个大按钮 (录音/停止) 的热区与行为
  · 长按列表项满 1 秒 → 按住期间立刻弹系统「确认删除」小确认框 (不是列表菜单)
  · 确认框点确定才删, 取消/返回都不删
  · 取消 / 未选中 / 空列表 / 播放中 都不会误删
  · 按键路径 (UP/DOWN 选中、左右键等价长按、CONFIRM 播放、BACK 交固件)
  · 退出时停录音/停播放

运行: python3 tests/recorder/test_recorder.py
退出码: 0=PASS 1=FAIL 2=SKIP(lupa 缺失)
"""
import os
import sys

try:
    import lupa
except ImportError:
    print("SKIP: 缺 lupa (pip install lupa) —— 无法在主机上跑 Lua 脚本")
    sys.exit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "store", "pkgs", "录音机.lua")
HARNESS = os.path.join(ROOT, "tests", "recorder", "harness.lua")

PASS = 0
FAIL = 0
FAILS = []

# 与生产脚本一致的布局常量 (只用于"点哪里", 断言仍走脚本实际绘制/命中)
LIST_Y, ROW_H = 34, 35
BAR_CX, BAR_CY = 200, 266       # 大按钮中心
BTN_L, BTN_R = 40, 360          # 大按钮左右边界


def check(cond, what):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        FAILS.append(what)
        print("  ✗ " + what)


def sec(name):
    print("\n— " + name)


L = lupa.LuaRuntime(unpack_returned_tuples=True)
L.execute(open(HARNESS, encoding="utf-8").read())
L.execute(open(APP, encoding="utf-8").read())
G = L.globals()


def reset(files=(("20261123_101500_0001.wav", 64000), ("20261123_111200_0002.wav", 32000))):
    """重建桩状态 + 重新进入应用（生产 on_enter 会重建列表与选中态）。"""
    L.execute("""
REC.fills={} REC.texts={} REC.dlg={} REC.removed={} REC.played={}
REC.rec_started=0 REC.stopped=0 REC.stopped_play=0 REC.redraws=0
REC.kbd_draw=0 REC.statusbar=nil REC.exit=false REC.toast=""
REC.dlgkind=nil REC.items={}
TOUCH_Q={} DLG_Q={}
REC_STATE.active=false REC_STATE.playing=false
CLOCK=0
HOLD_MS=0
FS_LIST={} FS_SIZE={} FS_DATA={}
""")
    for name, dl in files:
        L.globals().H_add_wav(name, dl)
    G.on_enter()
    G.render()


def tick(n=1):
    for _ in range(n):
        L.execute("CLOCK = CLOCK + 100")
        G.poll()


def click(x, y):
    """短按: 一帧按下、一帧松手（<480ms, 不会升级成长按）。"""
    L.execute("TOUCH_Q[#TOUCH_Q+1] = {%d,%d}" % (x, y))
    tick(2)


def hold_start(x, y, hold_ms=1100):
    """按住不放: 持续喂同一坐标并推进时钟, 到 1000ms 脚本应**当帧**弹窗。

    只按下、不松手 —— 用来验证"不用等松手"。
    """
    for k in range(1, hold_ms // 100 + 1):
        L.execute("HOLD_MS = %d" % (k * 100))
        L.execute("TOUCH_Q[#TOUCH_Q+1] = {%d,%d}" % (x, y))
        L.execute("CLOCK = CLOCK + 100")
        G.poll()


def hold_release():
    """松手: 队列空 ⇒ P.on=false。长按已弹窗, 松手这一下不再算点击。"""
    L.execute("HOLD_MS = 0")
    tick()


def long_click(x, y, hold_ms=1100):
    """完整手势: 按住到阈值 → 松手。"""
    hold_start(x, y, hold_ms)
    hold_release()


def row_cy(i):
    """第 i 条(1-based，从列表顶部数)的 y 中心。"""
    return LIST_Y + (i - 1) * ROW_H + ROW_H // 2


def fills():
    t = G.REC.fills
    return [(t[i].x0, t[i].y0, t[i].x1, t[i].y1) for i in range(1, len(t) + 1)]


def texts():
    t = G.REC.texts
    return [t[i].s for i in range(1, len(t) + 1)]


def lst(name):
    t = G.REC[name]
    return [t[i] for i in range(1, len(t) + 1)]


def selected_rows():
    """选中行的反白矩形 (draw_row 里 fill_rect(8, y, W-9, y+ROW_H-2))。"""
    return [f for f in fills() if f[0] == 8 and f[2] == 391 and (f[3] - f[1]) == ROW_H - 2]


# ─────────────────────────────────────────── 1 状态栏 / 布局 / 默认选中态
sec("状态栏声明 / 布局 / 默认不选中")
reset()
check(G.REC.statusbar is True, "on_enter 里调用了 app.statusbar(true)")
bad = [f for f in fills() if f[1] < 24]
check(not bad, "列表页所有绘制起点都 ≥ 24（不覆盖 OS 状态栏），越界 %d 处" % len(bad))
check(not [f for f in fills() if f[3] > 300 or f[1] > 299], "绘制不超出屏幕下边界")
check("录音机" not in texts(), "列表页不再有「录音机」标题行")
check(not [t for t in texts() if "条" in t], "列表页不再有「N 条」汇总行")
check(not selected_rows(), "进入列表默认不选中任何条目（无整行反白）")
# 列表第一行必须顶着状态栏下沿（24），不能留标题栏空白
check(any(f[1] == LIST_Y for f in fills()), "列表从 y=34 起（离状态栏留 10px，无标题栏）")

# ─────────────────────────────────────────── 2 底部大按钮
sec("底部大按钮（触摸）")
reset()
click(BAR_CX, BAR_CY)
check(G.REC.rec_started == 1, "点大按钮 → 开始录音")
G.render()
check(G.REC.kbd_draw == 0, "录音页不是改名页")
check(not [f for f in fills() if f[1] < 24], "录音页绘制不进状态栏")

reset()
click(BAR_CX, BAR_CY)          # 开始
click(BAR_CX, BAR_CY)          # 再点 = 停止
check(G.REC.stopped >= 1, "录音中再点大按钮 → 停止录音")

reset()
click(BTN_L - 12, BAR_CY)      # 按钮左侧留白处
check(G.REC.rec_started == 0, "大按钮左侧留白点击无副作用")
click(200, 296)                # 屏幕最底边（按钮下方）
check(G.REC.rec_started == 0, "大按钮下方空白点击无副作用")

# ─────────────────────────────────────────── 3 长按满 1 秒 → 确认删除小弹窗
sec("长按满 1 秒 → 按住期间立刻弹确认删除框")
reset()
hold_start(60, row_cy(1))       # 只按住, 不松手
check(len(lst("dlg")) == 1, "按住满 1 秒就弹窗（不用等松手）")
check(G.REC.dlgkind == "confirm", "弹的是系统确认框（小窗），不是列表菜单")
check(lst("dlg")[0] == "删除这条录音?", "确认文案为「删除这条录音?」")
check(len(lst("removed")) == 0, "只弹窗，不会立刻删")
hold_release()                  # 松手: 不应删、不应再触发别的动作
check(len(lst("removed")) == 0, "松手本身不会删除文件")
check(len(lst("dlg")) == 1, "松手不会重复弹窗，也不会把弹窗点掉")

# 不到 1 秒不弹窗
reset()
long_click(60, row_cy(1), hold_ms=600)
check(len(lst("dlg")) == 0, "按住 0.6 秒（未到 1 秒）不弹窗")

# ─────────────────────────────────────────── 4 确认框: 确定才删
sec("确认框：确定 / 取消")
reset()
long_click(60, row_cy(1)); L.execute("DLG_Q[#DLG_Q+1] = 0"); tick()   # 确定
check(len(lst("removed")) == 1, "点确定 → 真正删除文件")
check(lst("removed")[0].endswith(".wav"), "删除的是 wav 文件本身")
G.render()
check("1 条" not in texts(), "删除后不再显示条数行")
check(any("0001" in t or "0002" in t for t in texts()), "删除后列表仍有另一条")

reset()
long_click(60, row_cy(1)); L.execute("DLG_Q[#DLG_Q+1] = 1"); tick()   # 取消
check(len(lst("removed")) == 0, "点取消 → 文件保持不变")

reset()
long_click(60, row_cy(1)); L.execute("DLG_Q[#DLG_Q+1] = -1"); tick()  # 返回
check(len(lst("removed")) == 0, "返回（-1）→ 文件保持不变")

# 确认框没结果前一直等, 不会自己删
reset()
long_click(60, row_cy(1)); tick(3)
check(len(lst("removed")) == 0, "确认框未作答时不删（等 result）")

# 长按第 2 行 → 删的是第 2 条(列表按名字倒序: 第 1 行=111200_0002, 第 2 行=101500_0001)
reset()
long_click(60, row_cy(2)); L.execute("DLG_Q[#DLG_Q+1] = 0"); tick()
check(len(lst("removed")) == 1, "长按第 2 行点确定 → 删除一条")
check("0001" in lst("removed")[0], "删的是被长按的那一条（不是默认第一条）")

# ─────────────────────────────────────────── 5 空列表 / 未选中
sec("空列表与未选中保护")
reset(files=())
long_click(60, row_cy(1))
check(len(lst("dlg")) == 0, "空列表长按不弹确认框")
check(any("还没有录音" in t for t in texts()), "空列表给出「还没有录音」提示")
click(BAR_CX, BAR_CY)
check(G.REC.rec_started == 1, "空列表也能直接开始录音")

# ─────────────────────────────────────────── 6 播放中不会误删
sec("播放中删除保护")
reset()
click(60, row_cy(1))            # 第一次点 = 选中
G.render()
check(len(selected_rows()) >= 1, "点一行 → 该行反白（选中）")
click(60, row_cy(1))            # 第二次点同一行 = 播放
check(len(lst("played")) == 1, "连点同一行进入播放")
G.render()
click(BAR_CX, BAR_CY)           # 播放页该位置是停止键，不是删除
check(len(lst("dlg")) == 0, "播放页上点大按钮位置不会弹删除确认（不可能误删）")
check(len(lst("removed")) == 0, "播放中文件未被删除")

# ─────────────────────────────────────────── 7 按键路径
sec("按键路径")
reset()
G.action(2)                     # DOWN → 选中第 1 条
G.render()
check(len(selected_rows()) >= 1, "DOWN 选中第一条")
check(G.REC.rec_started == 0, "DOWN 只移动选中，不启动录音")

G.action(3)                     # LEFT → 等价长按 → 确认删除框
check(G.REC.dlgkind == "confirm", "按键 LEFT 弹出确认删除框")
L.execute("DLG_Q[#DLG_Q+1] = 0"); tick()
check(len(lst("removed")) == 1, "按键路径点确定 → 删除")

reset()
G.action(2)                     # DOWN 选中第一条（默认不选中）
G.action(4)                     # RIGHT → 同样是确认删除框
check(G.REC.dlgkind == "confirm", "按键 RIGHT 弹出确认删除框")
L.execute("DLG_Q[#DLG_Q+1] = -1"); tick()
check(len(lst("removed")) == 0, "按键路径取消 → 不删")

reset()
G.action(4)                     # 未选中时 RIGHT 只提示，不弹窗
check(G.REC.dlgkind != "confirm", "未选中时 RIGHT 不弹窗（先提示选一条）")

reset()
# CONFIRM 在未选中时 = 开始录音（主操作）
G.action(5)
check(G.REC.rec_started == 1, "未选中时 CONFIRM = 开始录音")

reset()
G.action(2)                     # 选中第一条
G.action(5)                     # CONFIRM → 播放
check(len(lst("played")) == 1, "选中后 CONFIRM = 播放该条")

# ─────────────────────────────────────────── 8 退出保护
sec("退出清理")
reset()
click(BAR_CX, BAR_CY)           # 开始录音
G.on_exit()
check(G.REC.stopped >= 1, "退出时停掉正在进行的录音")

reset()
check(G.REC.exit is False, "进入应用不会自动退出")
G.action(6)                     # BACK → 走固件统一退出确认
check(G.REC.exit is True, "BACK 交给固件统一退出流程（不自造弹窗）")

# ─────────────────────────────────────────── 汇总
print("\n录音机 UI / 长按删除 / 删除交互：" + str(PASS) + " PASS / " + str(FAIL) + " FAIL")
if FAIL:
    print("失败项：")
    for f in FAILS:
        print("  - " + f)
    sys.exit(1)
sys.exit(0)
