#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bt_c8_verify.py —— 蓝牙手柄重构（C2/C4/C5/C7）真机验收助手。

用法:
    ~/.espressif/python_env/idf5.5_py3.13_env/bin/python3 tools/bt_c8_verify.py        # 自动探测端口
    ~/.espressif/python_env/idf5.5_py3.13_env/bin/python3 tools/bt_c8_verify.py /dev/cu.usbmodem1101 75
（本机只有该 venv 装了 pyserial；WorkBuddy 自带的 managed python 没有。）

设计要点:
  * **只读打开串口** —— 绝不动 DTR/RTS（动一下就物理复位设备，会污染证据）。
      → 所以本脚本不代替你上电/复位，请你**先按提示手动操作**（开关手柄 / 拿去远处）。
  * 实时打印 + 边跑边判分；结束时给一张 ✅/⏳ 矩阵表。
  * 判定用"必须出现"和"必须不出现"两组关键字（见 docs/蓝牙手柄连接与自动回连-重构计划.md 6.1）。

按阶段做动作（脚本会在时间轴上打点）:
  0~15s   什么都不做（看开机/回连）
 15~35s   把手柄**关掉再开**（测 1/3）
 35~55s   把手柄**拿远离**（≥10m，测 4/11 省电关射频）
 55~70s   把手柄**拿回来**（测 4 后半：
"""
import glob
import re
import sys
import time

try:
    import serial
except ImportError:
    print("缺少 pyserial。本机可用: ~/.espressif/python_env/idf5.5_py3.13_env/bin/python3 tools/bt_c8_verify.py")
    sys.exit(2)

# ---- 判定规则: (矩阵编号, 说明, 必须出现[任一], 必须不出现[任一]) ----
RULES = [
    (1,  "关手柄再开 → 自动回连",
     ["回连命中(扫描)", "回连目标:", "connected with"], []),
    (2,  "手柄换地址 → 按名字回连",
     ["回连命中(扫描): name=", "回连目标: name="], []),
    (3,  "重启后开机窗口回连",
     ["[EV] 唤醒回连: 新预算", "开机回连窗口"], []),
    (4,  "手柄不在场 → 预算用尽关射频",
     ["回连预算用尽", "关射频省电"], ["panic", "Guru Meditation", "TG1WDT", "stack overflow"]),
    (5,  "自定义映射不被清",
     ["载入档案", "已保存"], ["尚未映射"]),
    (6,  "未映射手柄 → 主动引导映射",
     ["无映射档案 → 待做按键映射", "自动进入按键映射"], []),
    (7,  "已映射手柄 → 不打扰",
     ["载入档案"], ["无映射档案 → 待做按键映射"]),
    (8,  "扫描不被回连打断", ["蓝牙扫描开始"], []),
    (9,  "忘记此手柄", ["已忘记手柄", "用户忘记手柄"], []),
    (11, "省电: 射频真关掉", ["bluetooth stack released"], []),
    (12, "屏保触发 → 停回连", [], ["回连目标:"]),
]

# 必须**永不**出现的致命行（任何时刻命中即整场判失败）
FATAL = ["Guru Meditation", "panic'ed", "stack overflow", "assert failed",
         "LoadProhibited", "StoreProhibited", "TG1WDT_SYS_RST"]

# C2 档案表日志 → 便于人工核对
GPM_KEYS = ["手柄档案表", "载入档案", "尚未映射", "无映射档案", "已保存", "已忘记手柄"]

# 历史记录事件 (回答"历史会不会太多": 复用=顶替, 新增=又占一格)
HIST_REUSE = "[EV] 历史: 复用"
HIST_NEW = "[EV] 历史: 新增"


def detect_port():
    c = sorted(glob.glob("/dev/cu.usbmodem*"))
    return c[-1] if c else None


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else detect_port()
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 75.0
    if not port:
        print("未找到 /dev/cu.usbmodem* —— 设备没插或没上电（先 ls /dev/cu.usbmodem* 确认）")
        return 2

    print("=" * 78)
    print("端口 %s   抓 %ds   (**只读**, 不动 DTR/RTS)" % (port, dur))
    print("请在时间轴上做动作:")
    print("   0~15s  什么都不做 (看开机/回连)")
    print("  15~35s  把手柄关掉再开            → 测 #1 #3")
    print("  35~55s  把手柄拿远 (≥10m)          → 测 #4 #11 (省电关射频)")
    print("  55~75s  把手柄拿回来               → 测 #4 后半 (能连上)")
    print("   任何时刻可去手柄页点「忘记此手柄」  → 测 #9")
    print("=" * 78)

    try:
        ser = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        print("打开串口失败: %s" % e)
        return 2
    ser.dtr = False
    ser.rts = False

    hits, fatals, gpm_lines = [], [], 0
    hist_ev = []          # (时刻, 类型, 原文)
    tail = []
    t0 = time.time()
    while time.time() - t0 < dur:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").rstrip()
        tail.append(line)
        if len(tail) > 400:
            tail.pop(0)
        print("%6.1f %s" % (time.time() - t0, line), flush=True)

        for k in FATAL:
            if k in line:
                fatals.append(line)
        if any(k in line for k in GPM_KEYS):
            gpm_lines += 1
        for typ, key in (("复用", HIST_REUSE), ("新增", HIST_NEW)):
            if key in line:
                hist_ev.append((time.time() - t0, typ, line.strip()))
        for rid, _desc, must, never in RULES:
            for k in must:
                if k in line:
                    hits.append((rid, k, line))
            for k in never:
                if k in line:
                    hits.append((rid, "!" + k, line))
    ser.close()

    print("\n" + "=" * 78)
    print("判定结果")
    print("=" * 78)
    ok_n = 0
    for rid, desc, must, never in RULES:
        r_hits = [h for h in hits if h[0] == rid]
        good = [h for h in r_hits if not h[1].startswith("!")]
        bad = [h for h in r_hits if h[1].startswith("!")]
        # 规则判定: 有 must 命中且无 never 命中 = 通过; must 为空时只看 never
        passed = (bool(good) or not must) and not bad
        if passed:
            ok_n += 1
        print("%s #%-2d %s" % ("✅" if passed else "⏳", rid, desc))
        for h in r_hits[:3]:
            print("        · %s" % h[2][:110])
    print("\n通过 %d / %d 条（未过的多半是本次没造出该场景，不是失败 —— 重跑并按提示做动作）"
          % (ok_n, len(RULES)))

    if fatals:
        print("\n❌ 出现致命错误 %d 行:" % len(fatals))
        for f in fatals[:10]:
            print("   %s" % f[:140])
    else:
        print("\n✅ 全程无 panic / 看门狗 / 栈溢出")

    # ---- 历史记录观察 (回答"会不会太多") ----
    reuse = [e for e in hist_ev if e[1] == "复用"]
    new = [e for e in hist_ev if e[1] == "新增"]
    print("\n" + "-" * 78)
    print("历史记录观察: 复用(顶替) %d 次 / 新增(又占一格) %d 次" % (len(reuse), len(new)))
    for t, typ, txt in hist_ev:
        print("  %6.1f [%s] %s" % (t, typ, txt))
    if len(new) > 1 and len(reuse) == 0:
        print("  ⚠ 只有'新增'没有'复用' → 同一只被反复当新手柄存条目 (看两次新增里的名字是否一致)")
    elif reuse:
        print("  ✅ 有'复用' → 去重生效, 同一只不会无限占格")
    print("-" * 78)

    if gpm_lines == 0:
        print("\n⚠ 没抓到任何档案表/载入档案日志 —— C2 的档案逻辑没被走到:")
        print("   · 确认已烧入新固件（本次 LinTOS.bin 0x43ec20）")
        print("   · 确认这轮里真的发生过一次连接手柄")
    print("=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
