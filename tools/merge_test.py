#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""merge_test.py — 《求合体》主机端到端测试。

用 lupa 跑与固件同版本的 Lua 5.5，给 app.* 建最小桩（绘制为空、NVS 进内存、
触摸可注入），断言分三层：

  1. 算法层（Python 侧独立实现 Mulberry32 与 Lua 逐值比对）
  2. 引擎层（调用 Lua 里的 _selftest：合成 / 连锁 / 石头 / 计分 / 存档）
  3. 交互层（真触摸事件：落子、拒绝、菜单、结算、帮助，全部走 poll/on_tap）

用法: python3 tools/merge_test.py [-v]
"""
import os
import sys

import lupa

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "apps", "lua", "apps", "app", "求合体.lua")

W, H = 400, 300
CELL, BX, BY = 44, 6, 12
BS = CELL * 6
PX = BX + BS + 4

M32 = 0xFFFFFFFF


def mulberry32(seed, n):
    """参考实现 (triple-town-ish gameEngine.ts rngNext) 的 Python 版。"""
    st = seed & M32
    out = []
    for _ in range(n):
        st = (st + 0x6D2B79F5) & M32
        t = st
        t = ((t ^ (t >> 15)) * (t | 1)) & M32
        t = (t ^ (t + (((t ^ (t >> 7)) * (t | 61)) & M32))) & M32
        out.append(((t ^ (t >> 14)) & M32) / 4294967296)
    return out


def dec(v):
    return v.decode("utf-8", "replace") if isinstance(v, bytes) else v


class Sim:
    def __init__(self):
        self.lua = lupa.LuaRuntime(unpack_returned_tuples=True)
        self.nvs = {}
        self.now = 5000
        self.touch_xy = None
        self.logs = []
        self.dlgs = []
        self.tones = []
        self._dlg_result = None
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
                     "tone", "melody", "exit", "exit_confirm", "vk"):
            app[name] = lambda *a, **k: None
        app["tone_async"] = lambda f, ms=40, amp=40: self.tones.append((f, ms))
        app["vibrate"] = lambda ms=30: None
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
        app["touch_hold"] = lambda: 0
        app["log"] = lambda *a: self.logs.append(" ".join(str(dec(x)) for x in a))

        nvs = L.table()
        nvs["get_str"] = lambda k: self.nvs.get(dec(k))
        nvs["set_str"] = lambda k, v: (self.nvs.__setitem__(dec(k), dec(v)), True)[1]
        nvs["get_int"] = lambda k: self.nvs.get(dec(k), 0)
        nvs["set_int"] = lambda k, v: (self.nvs.__setitem__(dec(k), int(v)), True)[1]
        app["nvs"] = nvs

        dlg = L.table()
        dlg["toast"] = lambda msg, ms=0: None
        dlg["confirm"] = lambda msg: None
        dlg["list"] = lambda t, items, sel=0, corner=None: (
            self.dlgs.append((dec(t), len(items), sel, None if corner is None else dec(corner))), 0)[1]
        dlg["result"] = lambda: self._dlg_result
        app["dlg"] = dlg

        app["fs"] = L.table(exists=lambda p: False, read=lambda p: None,
                            write=lambda p, d: True, mkdir=lambda p: True,
                            size=lambda p: 0, listdir=lambda p: L.table())
        app["net"] = L.table(is_connected=lambda: False)
        app["util"] = L.table()
        L.globals()["app"] = app

    # -------------------------------------------------------------- 工具
    def exec_file(self):
        self.lua.execute(open(APP, encoding="utf-8").read())

    def call(self, name, *args):
        fn = self.lua.globals()[name]
        if fn is None:
            raise RuntimeError("Lua 全局函数不存在: %s" % name)
        return fn(*args)

    def probe(self):
        t = self.call("_probe")
        return {k: dec(v) for k, v in t.items()}

    def tap(self, x, y, dwell=60):
        self.now += 60
        self.touch_xy = (x, y)
        self.call("poll")
        self.now += dwell
        self.touch_xy = None
        self.call("poll")

    def render(self):
        self.call("render")

    def dlg_choose(self, idx):
        """模拟用户点了系统弹窗的第 idx 项 (1-based)。"""
        self._dlg_result = idx
        self.call("poll")
        self._dlg_result = None


CHECKS = []


def ck(cond, msg):
    CHECKS.append((bool(cond), msg))
    return bool(cond)


def cell_center(i):
    r, c = (i - 1) // 6, (i - 1) % 6
    return BX + c * CELL + CELL // 2, BY + r * CELL + CELL // 2


def rng_check(sim):
    """Lua 与 Python 各自算 Mulberry32，逐值比对。"""
    for seed in (0, 1, 12345, 0xDEADBEEF, 4294967295):
        lua_vals = dec(sim.call("_rng_seq", seed, 8)).split(",")
        py_vals = ["%.9f" % v for v in mulberry32(seed, 8)]
        ck(lua_vals == py_vals, "RNG 与参考实现不一致 (seed=%d)\n    lua=%s\n    py =%s"
           % (seed, lua_vals[:4], py_vals[:4]))


def engine_check(sim):
    res = sim.call("_selftest")
    fails = [dec(x) for x in res["fail"].values()] if res["fail"] is not None else []
    ck(res["ok"], "引擎自检: %s" % ("; ".join(fails) if fails else "通过"))
    print("  · _selftest 断言 %d 条, 失败 %d 条" % (res["count"], len(fails)))


def interaction_check(sim):
    sim.call("on_enter")
    sim.render()
    p = sim.probe()

    ck(p["page"] == "game", "启动直接进入棋盘")
    ck(p["bx"] == BX and p["by"] == BY, "棋盘起点 %s,%s" % (p["bx"], p["by"]))
    ck(p["bs"] == BS and p["cell"] == CELL, "棋盘 6x6 * 44 = 264")
    ck(BX + BS <= PX, "棋盘不压右栏")
    ck(PX + p["pw"] <= W, "右栏不越界")
    ck(BY + BS <= H, "棋盘不越界")
    ck(p["next"] >= -1 and p["next"] <= 9, "下一个物品取值合法")

    # 落子
    board0 = p["board"]
    empties = [i for i in range(36) if board0[i] == "0"]
    ck(len(empties) > 0, "开局有空格")
    i0 = empties[0]
    x, y = cell_center(i0 + 1)
    sim.tap(x, y)
    p1 = sim.probe()
    ck(p1["turn"] == 1, "点空格放下一个物品 (turn=%s)" % p1["turn"])
    ck(p1["board"] != board0, "棋盘已变化")

    # 占位格再点: 应被拒绝且不增加回合
    t_before = p1["turn"]
    occ = [i for i in range(36) if p1["board"][i] != "0"]
    if occ:
        x, y = cell_center(occ[0] + 1)
        sim.tap(x, y)
        ck(sim.probe()["turn"] == t_before, "点在已占格不消耗回合")

    # 点右栏空白不落子
    t_before = sim.probe()["turn"]
    sim.tap(PX + 20, 100)
    ck(sim.probe()["turn"] == t_before, "点右栏不落子")

    # 菜单弹窗
    sim.tap(PX + 20, 230)
    ck(sim.dlgs and sim.dlgs[-1][0] == "暂停", "菜单按钮弹出系统弹窗")
    ck(sim.dlgs[-1][1] == 3, "菜单 3 项")
    # 选"玩法说明"
    sim.dlg_choose(3)
    ck(sim.probe()["page"] == "help", "菜单可进帮助页")
    sim.render()
    # 帮助页点任意处返回
    sim.tap(W // 2, 150)
    ck(sim.probe()["page"] == "game", "帮助页点击返回棋盘")

    # 键盘 BACK: 棋盘页不消费 (交给框架退出)
    ck(sim.call("action", 6) is not True, "棋盘页 BACK 不吞掉")
    sim.call("action", 3)  # 无副作用

    # 存档: on_exit 写入, on_enter 恢复
    before = sim.probe()
    sim.call("on_exit")
    ck("tt.game" in sim.nvs, "on_exit 写入 tt.game")
    sim.now += 1000
    sim.call("on_enter")
    after = sim.probe()
    ck(after["board"] == before["board"], "重进恢复棋盘")
    ck(after["score"] == before["score"] and after["turn"] == before["turn"], "重进恢复分数/回合")

    # 结算页: 把棋盘填满再落最后一子
    # 铺满只留一格, 再走真实触摸完成最后一手 -> 结算页
    sim.call("_fill_for_over")
    p = sim.probe()
    emp = [i for i in range(36) if p["board"][i] == "0"]
    ck(len(emp) == 1, "铺盘后只剩 1 空格")
    x, y = cell_center(emp[0] + 1)
    sim.tap(x, y)
    p = sim.probe()
    ck(p["over"] == 1, "填满后判定结束")
    ck(p["page"] == "game", "结算页延后 0.5s 再切")
    sim.now += 600
    sim.render()          # 切页在 render 里做 (每帧都跑)
    p = sim.probe()
    ck(p["page"] == "over", "结算页按时出现")
    sim.render()
    # 再来一局
    sim.tap(100, 240)
    p = sim.probe()
    ck(p["page"] == "game" and p["turn"] == 0 and p["score"] == 0, "再来一局重置")


def main():
    verbose = "-v" in sys.argv
    sim = Sim()
    sim.exec_file()

    rng_check(sim)
    engine_check(sim)
    sim.call("on_enter")
    interaction_check(sim)

    errs = [m for m in sim.logs if "error" in m.lower() or "attempt to" in m]
    ck(not errs, "运行期无 Lua 错误: %s" % errs[:2])

    bad = [m for ok, m in CHECKS if not ok]
    print("\n共 %d 项检查, 通过 %d, 失败 %d" % (len(CHECKS), len(CHECKS) - len(bad), len(bad)))
    if bad:
        for m in bad:
            print("  ✗ " + m)
        if verbose:
            for m in sim.logs[-20:]:
                print("  " + m)
        return 1
    print("全部通过 ✅")
    return 0


if __name__ == "__main__":
    sys.exit(main())
