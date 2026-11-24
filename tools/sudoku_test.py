#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sudoku_test.py — 《数独大师》主机端到端测试。

用 lupa 跑与固件同版本的 Lua 5.5，给 app.* 建**最小桩**（绘制为空、NVS 进内存、
触摸可注入），然后调用主 Lua 文件里的 _selftest() / _probe() 做断言。

覆盖:
  · 布局: 棋盘 270x270 正方形, 不越界, 左右栏不重叠
  · 交互: 首页->闯关->点数字->点格子填数 / 笔记 / 擦除 / 长按候选菜单
  · 计分: 0/1/2/3 档, 重玩补差额, 不重复发放 (在 _selftest 内)
  · 技能: 购买扣费 / 章节门禁 / 排除与定位等级生效
  · 技巧: 结果来自真实算法, 等级不超过已开放上限
  · 存档: 写入 -> 破坏内存 -> on_enter 恢复
  · 渲染: 10 个页面各渲染一次不抛错

用法: python3 tools/sudoku_test.py [-v]
"""
import os
import sys
import traceback

import lupa

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "apps", "lua", "apps", "app", "数独大师.lua")

W, H = 400, 300
RB_X, RB_W, RB_H = 332, 64, 30
RB_Y = [4, 37, 70, 103, 136]
LEFT_W = 50
RIGHT_X = 350
CELL, BX, BY = 33, 50, 0
NUM_BOT = 300           # 左栏铺满 300 (无底部状态行)
BOX = 30


def dec(v):
    if isinstance(v, bytes):
        return v.decode("utf-8", "replace")
    return v


class Sim:
    def __init__(self):
        self.lua = lupa.LuaRuntime(unpack_returned_tuples=True)
        self.nvs = {}
        self.now = 1000
        self.touch_xy = None
        self.hold = 0
        self.logs = []
        self.toasts = []
        self.dlgs = []          # 系统弹窗调用记录 (title, 项数, 选中, 右下角小字)
        self._build()

    # ------------------------------------------------------------------ 桩
    def _build(self):
        L, app = self.lua, self.lua.table()

        def mixw(s):
            s = dec(s)
            return sum(16 if ord(ch) > 127 else 8 for ch in s)

        app["W"], app["H"] = W, H
        app["BLACK"], app["WHITE"] = 0, 1
        app["UP"], app["DOWN"], app["LEFT"], app["RIGHT"] = 1, 2, 3, 4
        app["OK"], app["CONFIRM"], app["BACK"], app["HOME"] = 5, 5, 6, 7

        for name in ("clear", "px", "fill", "rect", "store_rect", "hline", "vline",
                     "text", "textc", "texts", "textm", "textsb", "textt", "zh", "zh24",
                     "zhs", "zhsm", "zhsb", "mix", "mixc", "big", "bigc",
                     "bmp", "bmpr", "poly", "statusbar", "redraw", "sbar",
                     "tone", "tone_async", "melody", "vibrate", "exit",
                     "exit_confirm", "vk"):
            app[name] = lambda *a, **k: None
        app["textw"] = mixw
        app["mixw"] = mixw
        app["millis"] = lambda: self.now
        app["delay"] = lambda ms: None
        app["key"] = lambda: None
        app["key_down"] = lambda a: False
        app["keys_mask"] = lambda: 0
        app["key_repeat"] = lambda a, first=400, rate=90: False
        app["touch"] = lambda: self.touch_xy
        app["touch_start"] = lambda: self.touch_xy
        app["touch_hold"] = lambda: self.hold
        app["log"] = lambda *a: self.logs.append(" ".join(str(dec(x)) for x in a))

        nvs = L.table()
        nvs["get_str"] = lambda k: self.nvs.get(dec(k))
        nvs["set_str"] = lambda k, v: (self.nvs.__setitem__(dec(k), v), True)[1]
        nvs["get_int"] = lambda k: 0
        nvs["set_int"] = lambda k, v: True
        app["nvs"] = nvs

        db = L.table()
        db["set"] = lambda k, v: True
        db["get"] = lambda k: None
        db["del"] = lambda k: True
        db["seti"] = lambda k, v: True
        db["geti"] = lambda k, d=0: d
        app["db"] = db

        dlg = L.table()
        dlg["toast"] = lambda msg, ms=0: self.toasts.append(dec(msg))
        dlg["confirm"] = lambda msg: None
        # ← 第 4 参 corner (2026-11-08 新增, 弹窗右下角小字, 如 "积分 32")
        dlg["list"] = lambda t, items, sel=0, corner=None: (
            self.dlgs.append((dec(t), len(items), sel, None if corner is None else dec(corner))), 0)[1]
        dlg["result"] = lambda: None
        app["dlg"] = dlg

        sys_t = L.table()
        sys_t["battery"] = lambda: 4000
        sys_t["heap_free"] = lambda: 50000
        sys_t["time_ms"] = lambda: self.now
        sys_t["clock"] = lambda: (12, 0, 0)
        sys_t["date"] = lambda: (11, 8, 1)
        app["sys"] = sys_t

        app["json"] = L.table(pick=lambda *a: L.table(), num=lambda *a: 0,
                             str=lambda *a: None, bool=lambda *a: False)
        app["fs"] = L.table(exists=lambda p: True, read=lambda p: None,
                            write=lambda p, d: True, mkdir=lambda p: True,
                            size=lambda p: 0, listdir=lambda p: L.table())
        app["net"] = L.table(is_connected=lambda: False)
        app["util"] = L.table()          # 主应用刻意不用 app.util
        L.globals()["app"] = app
        # app.bmpr 的第 5 参是 1bpp 二进制串 (含非法 UTF-8 字节)。lupa 会在进入
        # Python 之前就 UTF-8 解码参数并抛错, 所以在 Lua 侧先转 hex 再过来。
        L.execute("""
            local _raw = app.bmpr
            app.bmpr = function(x, y, w, h, d)
              local hex = d:gsub(".", function(c) return string.format("%02x", string.byte(c)) end)
              return _raw(x, y, w, h, hex)
            end
        """)

    # -------------------------------------------------------------- 工具
    def exec_file(self):
        src = open(APP, encoding="utf-8").read()
        self.lua.execute(src)

    def call(self, name, *args):
        fn = self.lua.globals()[name]
        if fn is None:
            raise RuntimeError("Lua 全局函数不存在: %s" % name)
        return fn(*args)

    def probe(self):
        t = self.call("_probe")
        return {k: dec(v) if not isinstance(v, (int, float)) else v for k, v in t.items()}

    def tap(self, x, y, hold=0, dwell=60):
        """按下沿 + 抬起沿各跑一次 poll, 与固件轮询语义一致。"""
        self.now += 100
        self.hold = hold
        self.touch_xy = (x, y)
        self.call("poll")
        self.now += dwell
        self.touch_xy = None
        self.call("poll")

    def hold_tap(self, x, y, ms=600):
        self.tap(x, y, hold=ms, dwell=20)

    def key(self, a):
        return self.call("action", a)

    def render(self):
        self.call("render")


CHECKS = []


def ck(cond, msg):
    CHECKS.append((bool(cond), msg))
    return bool(cond)


EDG = [int(k * 300 / 9 + 0.5) for k in range(10)]


def board_cell_xy(idx):
    r, c = (idx - 1) // 9, (idx - 1) % 9
    cx = BX + EDG[c] + (EDG[c + 1] - EDG[c]) // 2
    cy = BY + EDG[r] + (EDG[r + 1] - EDG[r]) // 2
    return cx, cy


def digit_xy(probe, d):
    """左侧数字方块中心 (方块 30x30 在 x=4..33, 行高按 300 均分)"""
    cnt = [int(x) for x in probe["cnt"].split(",")]
    vis = [k for k in range(1, 10) if cnt[k - 1] < 9]
    if d not in vis:
        return None, None
    step = NUM_BOT / len(vis)
    cy = vis.index(d) * step + step / 2
    return 4 + BOX // 2, int(cy)


def items_geom(n):
    return 34, min(44, (H - 6 - 34) // max(n, 1))


def digit_y(probe, d):
    """左侧可见数字按钮的纵向中心 (数量满 9 的按钮隐藏, 布局重排)"""
    cnt = [int(x) for x in probe["cnt"].split(",")]
    vis = [k for k in range(1, 10) if cnt[k - 1] < 9]
    if d not in vis:
        return None
    step = H / len(vis)
    return int(vis.index(d) * step + step / 2)


def back_home(sim, depth=4):
    """连续 BACK 直到首页 (子页返回父页, 一次 BACK 不一定回首页)"""
    for _ in range(depth):
        if sim.probe()["page"] == "home":
            return True
        sim.key(6)
    return sim.probe()["page"] == "home"


CARD_W, CARD_H = 124, 96


def home_card_xy(idx, n):
    """与 Lua home_rect 同公式: 列数随卡片数量变, 每行居中"""
    cols = 3 if n >= 5 else 2
    rows = (n + cols - 1) // cols
    row, col = idx // cols, idx % cols
    in_row = min(cols, n - row * cols)
    gx, gy = 8, 14
    tw = in_row * CARD_W + (in_row - 1) * gx
    th = rows * CARD_H + (rows - 1) * gy
    x0 = (W - tw) // 2 + col * (CARD_W + gx)
    y0 = (H - th) // 2 + row * (CARD_H + gy)
    return x0 + CARD_W // 2, y0 + CARD_H // 2


def clear_pend(sim):
    """连续填写是刻意保留的行为: 左侧数字仍选中时点空格会直接落子。
    要测"先选格再点数字"这条路径, 必须先把待填数字取消掉。"""
    pen = sim.probe()["pend"]
    if pen:
        xx, yy = digit_xy(sim.probe(), pen)
        if yy is not None:
            sim.tap(xx, yy)
    return sim.probe()["pend"]


def goto(sim, want, max_item=12):
    """首页点卡片 / 子页点列表, 直到进入目标页"""
    back_home(sim)
    if sim.probe()["page"] == "home":
        # 首页是横排拖动菜单: 焦点卡恒在屏幕正中, 先把焦点移到第 k 张再点中间
        ids = sim.probe()["cards"].split(",")
        for k in range(1, len(ids) + 1):
            for _ in range(len(ids) + 2):
                cur = sim.probe()["hsel"]
                if cur == k:
                    break
                sim.call("action", 4 if cur < k else 3)   # RIGHT / LEFT
            sim.tap(200, 150)
            if sim.probe()["page"] == want:
                return True
            if "-v" in sys.argv:
                print("    goto %s: %s -> %s" % (want, ids[k-1], sim.probe()["page"]))
            back_home(sim)
        return False
    n = sim.probe()["items"]
    y0, rh = items_geom(n)
    for k in range(1, min(n, max_item) + 1):
        sim.tap(200, y0 + (k - 1) * rh + rh // 2)
        if sim.probe()["page"] == want:
            return True
        back_home(sim)
    return False


def main():
    verbose = "-v" in sys.argv
    sim = Sim()
    sim.exec_file()

    # ---- 1 启动 (先 on_enter, 与设备运行顺序一致)
    sim.now = 5000
    sim.call("on_enter")
    sim.render()

    # ---- 2 自检: 题库 / 计分 / 存档 / 技巧 / 技能 / 完整通关结算
    res = sim.call("_selftest")
    fails = [dec(x) for x in res["fail"].values()] if res["fail"] is not None else []
    ck(res["ok"], "自检: %s" % ("; ".join(fails) if fails else "通过"))
    print("  · _selftest 断言 %d 条, 失败 %d 条" % (res["count"], len(fails)))

    # 复位到干净状态
    sim.call("on_enter")
    p = sim.probe()
    ck(p["page"] == "home", "启动进入首页")
    ck(p["items"] >= 4, "首页卡片 >= 4")
    ck(p["bs"] == 300, "棋盘 300x300 正方形")
    ck(p["bx"] == p["left_w"] == 50, "棋盘紧贴左栏")
    ck(p["bx"] + p["bs"] == p["right_x"], "棋盘右侧接右栏")
    ck(p["left_w"] == W - p["right_x"], "左右栏等宽 (棋盘居中)")
    ck(p["by"] == 0 and p["by"] + p["bs"] == H, "棋盘上下贴边, 无白缝")
    ck(p["bx"] + p["bs"] <= W, "棋盘不超屏宽")
    ck(p["right_x"] == 350 and p["right_w"] == 50, "右栏 50px")
    ck(p["by"] >= 0 and p["by"] + p["bs"] <= H, "棋盘在 300 高内完整显示")
    ck(p["bs"] == H, "棋盘正好等于屏高")

    # ---- 3 首页 -> 章节 -> 第1关
    ck(goto(sim, "chapters"), "首页可进入章节页")
    sim.tap(100, 72)                           # 第 1 章
    ck(sim.probe()["page"] == "levels", "章节可进入关卡页")
    sim.tap(100, 55)                           # 第 1 关
    if sim.probe()["tut"]:
        sim.tap(250, 180)                      # 关掉启蒙浮层
    ck(sim.probe()["page"] == "game", "关卡可进入游戏页")
    p = sim.probe()
    ck(p["lv"] >= 1, "进入关卡")
    ck(p["board"] != "", "棋盘已载入")
    sim.render()

    before_cnt = [int(x) for x in p["cnt"].split(",")]
    ck(all(0 <= x <= 9 for x in before_cnt), "剩余数量在 0..9")

    # ---- 4 先点数字 -> 再点空格
    sol, board = p["sol"], p["board"]
    empty1 = next(i for i in range(81) if board[i] == "0")
    d1 = int(sol[empty1])
    xx, yy = digit_xy(p, d1)
    ck(yy is not None, "正确数字按钮可见")
    sim.tap(xx, yy)
    ck(sim.probe()["pend"] == d1, "点左侧数字进入待填状态")
    x, y = board_cell_xy(empty1 + 1)
    sim.tap(x, y)
    p2 = sim.probe()
    ck(p2["board"][empty1] in "ABCDEFGHI", "点数字 -> 点空格 填入正确数字")
    ck(p2["err"] == 0, "正确填写不增加错误")
    ck(int(p2["cnt"].split(",")[d1 - 1]) == before_cnt[d1 - 1] + 1, "剩余数量减少")
    sim.render()

    # ---- 5 先点空格 -> 再点数字
    ck(clear_pend(sim) == 0, "再次点同一数字可取消待填")
    p3 = sim.probe()
    empty2 = next((i for i in range(81) if p3["board"][i] == "0"), None)
    ck(empty2 is not None, "还有空格可测先选格后填数")
    if empty2 is not None:
        x, y = board_cell_xy(empty2 + 1)
        sim.tap(x, y)
        ck(sim.probe()["sel"] == empty2 + 1, "点棋盘选中空格")
        d2 = int(p3["sol"][empty2])
        xx, yy = digit_xy(sim.probe(), d2)
        if yy is not None:
            sim.tap(xx, yy)
        p4 = sim.probe()
        ck(p4["board"][empty2] in "ABCDEFGHI", "先选格 -> 点数字 填入正确数字")
        ck(p4["err"] == 0, "第二种填法也不计错")

    # ---- 6 错误数字: 现在**直接拒绝写入**(不留小写标记), 每错一次 -1 分
    clear_pend(sim)
    p5 = sim.probe()
    empty3 = next((i for i in range(81) if p5["board"][i] == "0"), None)
    ck(empty3 is not None, "还有空格可测错误")
    if empty3 is not None:
        right = int(p5["sol"][empty3])
        cnt = [int(x) for x in p5["cnt"].split(",")]
        bad = next((d for d in range(1, 10) if d != right and cnt[d - 1] < 9), None)
        ck(bad is not None, "存在可点的错误数字")
        if bad is not None:
            x, y = board_cell_xy(empty3 + 1)
            sim.tap(x, y)                       # 先选中格子
            xx, yy = digit_xy(sim.probe(), bad)
            sim.tap(xx, yy)
            p6 = sim.probe()
            ck(p6["board"][empty3] == "0", "错误数字被拒绝, 格子仍为空")
            ck(p6["err"] == 1, "错误次数 +1")
            # 同一格再点同一个错误数字: 继续计错 (本关最多扣满 3 分)
            clear_pend(sim)
            sim.tap(x, y)
            xx, yy = digit_xy(sim.probe(), bad)
            sim.tap(xx, yy)
            ck(sim.probe()["err"] == 2, "重复点同一错误数字继续计错")
            sim.render()

    # ---- 7 笔记模式 / 候选 / 长按候选菜单
    sim.tap(RIGHT_X + 25, 212)
    ck(sim.probe()["mode"] == "note", "右侧笔记组开启笔记模式")
    p7 = sim.probe()
    e3 = next((i for i in range(81) if p7["board"][i] == "0"), None)
    if e3 is not None:
        x, y = board_cell_xy(e3 + 1)
        sim.tap(x, y)
        xx, yy = digit_xy(sim.probe(), 3)
        if yy is not None:
            sim.tap(xx, yy)
    ck(sim.probe()["notes_count"] >= 1, "候选笔记写入")
    sim.render()
    sim.hold_tap(RIGHT_X + 25, 212, 600)
    ck(sim.probe()["page"] == "notemenu", "长按笔记组进入候选菜单")
    sim.render()
    sim.key(6)
    ck(sim.probe()["page"] == "game", "候选菜单可返回棋盘")

    # ---- 8 长按格子擦除
    p8 = sim.probe()
    e4 = next((i for i in range(81) if p8["board"][i] in "ABCDEFGHI"), None)
    if e4 is not None:
        x, y = board_cell_xy(e4 + 1)
        sim.hold_tap(x, y, 600)
        ck(sim.probe()["board"][e4] == "0", "长按格子擦除玩家数字")
    sim.render()

    # ---- 9 技能: 新档从 0 级起, 只走商店升级 (棋盘页不再有技能按钮)
    p_sk = sim.probe()
    # 技能全部靠积分在商店升级: 等级必须落在 0..上限 (自检里已经跑过购买扣费/门禁)
    ck(0 <= p_sk["skill_fill"] <= 6 and 0 <= p_sk["skill_tip"] <= 4
       and 0 <= p_sk["skill_auto"] <= 4, "技能等级在 0..上限")
    ck(sim.probe()["page"] == "game", "棋盘页无技能按钮也不崩")

    # ---- 10 存档: on_exit 写入, on_enter 恢复
    p9 = sim.probe()
    keep_lv, keep_board, keep_err = p9["lv"], p9["board"], p9["err"]
    sim.call("on_exit")
    sim.now += 1000
    sim.call("on_enter")
    p10 = sim.probe()
    ck(p10["lv"] == keep_lv, "存档恢复当前关卡")
    ck(p10["board"] == keep_board, "存档恢复棋盘")
    ck(p10["err"] == keep_err, "存档恢复错误次数")

    # ---- 11 各页面可达且能渲染
    ck(goto(sim, "chapters"), "章节页可达")
    sim.render()
    sim.tap(100, 72)                            # 第 1 章卡片
    ck(sim.probe()["page"] == "levels", "关卡页可达")
    sim.render()
    sim.key(6)
    sim.key(6)
    ck(sim.probe()["page"] == "home", "关卡/章节可逐级返回")

    # 底部台阶: 左=设置弹窗, 右=技能商店弹窗 (都不再是独立页面)
    sim.dlgs.clear()
    sim.tap(20, H - 14)
    ck(bool(sim.dlgs) and sim.dlgs[-1][0] == "设置", "底栏左键打开设置弹窗")
    sim.key(6)
    sim.dlgs.clear()
    sim.tap(W - 30, H - 14)
    ck(bool(sim.dlgs) and sim.dlgs[-1][0] == "技能商店", "底栏右键打开技能商店弹窗")
    ck(sim.dlgs and (sim.dlgs[-1][3] or "").startswith("积分"),
       "商店弹窗右下角显示积分: %s" % (sim.dlgs[-1][3] if sim.dlgs else None))
    sim.key(6)

    # ---- 12 运行期无 Lua 错误
    errs = [m for m in sim.logs if "error" in m.lower() or "attempt to" in m]
    ck(not errs, "运行期无 Lua 错误: %s" % errs[:3])

    # ---------------------------------------------------------------- 汇总
    bad = [m for ok, m in CHECKS if not ok]
    print("\n共 %d 项检查, 通过 %d, 失败 %d" % (len(CHECKS), len(CHECKS) - len(bad), len(bad)))
    if bad:
        for m in bad:
            print("  ✗ " + m)
        if verbose:
            print("\n日志:")
            for m in sim.logs[-30:]:
                print("  " + m)
        return 1
    print("全部通过 ✅")
    return 0


if __name__ == "__main__":
    sys.exit(main())
