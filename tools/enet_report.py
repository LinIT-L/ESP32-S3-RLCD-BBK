#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
enet_report.py — 立创EDA专业版 (EasyEDA Pro) 网表 .enet 分析器

用途: 把导出的网表 JSON 变成能看的东西, 并自动挑出常见设计问题。
     不需要打开 EDA 软件, 适合做接线复核 / 改板前体检。

用法
----
    # 全量报表 (元件表 + 网络表 + 问题清单)
    python3 tools/enet_report.py ~/Desktop/xxx.enet

    # 只看问题 (改板前必看)
    python3 tools/enet_report.py ~/Desktop/xxx.enet --issues

    # 追一条网络到所有引脚
    python3 tools/enet_report.py ~/Desktop/xxx.enet --net VSYS

    # 按位号前缀过滤 (只看电源相关器件)
    python3 tools/enet_report.py ~/Desktop/xxx.enet --filter U,R,C,L,D,Q

    # 导出成 CSV 便于对比两版网表
    python3 tools/enet_report.py a.enet --csv a.csv

自动检查
--------
  [E] 位号重复 (同一 Designator 出现多次)
  [E] 同一个器件型号+封装被重复放置且网络完全相同 (疑似画重了, 会硬并联)
  [E] 只有 1 个引脚的网络 (悬空/未真正连通)
  [W] 未贴装器件 (Name 里含 NC / DNP / 不贴)
  [W] 电源网络上没有任何电容 (去耦缺失)
  [i] 网络按引脚数排序, 一眼看出主电源节点
"""

import argparse
import csv
import json
import os
import re
import sys
from collections import defaultdict

# 名字里出现这些词 = 不贴装
NC_PAT = re.compile(r"(NC|DNP|不贴|NOT\s*PLACE)", re.I)
# 认为是"电源类"网络名
PWR_PAT = re.compile(r"^(V|_V|VSYS|VBAT|VBUS|VCC|AVDD|DVDD|3V3|5V|1V8|GND|VDD)", re.I)


def des_key(des: str):
    """R10 排在 R9 后面 (按前缀 + 数字排序), 而不是字符串序。"""
    m = re.match(r"^([A-Za-z_]+)(\d+)$", des or "")
    return (m.group(1), int(m.group(2))) if m else (des or "", 0)


def load(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    if "components" not in d:
        raise SystemExit("[错误] %s 里没有 components 段, 不是 EasyEDA Pro .enet 网表?" % path)
    return d


def parse(enet):
    """→ (comps, net2pins) ; comps = [ {des,name,fp,pins:{num:net},nc:bool} ]"""
    comps = []
    net2pins = defaultdict(list)
    for uid, c in enet["components"].items():
        p = c.get("props", {})
        des = p.get("Designator", "?")
        pins = {}
        for _pn, pv in (c.get("pinInfoMap") or {}).items():
            num = pv.get("number", "")
            net = (pv.get("net") or "").strip()
            if net in ("", "-"):
                net = ""
            pins[num] = net
            if net:
                net2pins[net].append("%s.%s" % (des, num))
        name = p.get("Name", "") or ""
        comps.append({
            "des": des, "name": name,
            "fp": p.get("FootprintName") or p.get("Origin Footprint") or "",
            "pins": pins, "nc": bool(NC_PAT.search(name)),
            "uid": p.get("Unique ID", uid),
        })
    comps.sort(key=lambda c: des_key(c["des"]))
    return comps, net2pins


def issues(comps, net2pins):
    out = []
    # 位号重复
    seen = defaultdict(list)
    for c in comps:
        seen[c["des"]].append(c)
    for des, cs in seen.items():
        if len(cs) > 1:
            out.append(("E", "位号重复: %s 出现 %d 次" % (des, len(cs))))

    # 同型号同封装同网络的重复放置 (会硬并联)
    # 排除噪声: 安装孔/纯 GND 的接地针/结构件 —— 这些"重复"是正常的
    STRUCT_PAT = re.compile(r"(HOLE|M3|螺丝|安装孔|梅花)", re.I)
    sig = defaultdict(list)
    for c in comps:
        if STRUCT_PAT.search(c["des"]) or STRUCT_PAT.search(c["name"]):
            continue
        nets = set(n for n in c["pins"].values() if n)
        if not nets or nets == {"GND"}:
            continue          # 纯接地件 (如全 GND 的排针) 不算重复
        sig[(c["name"], c["fp"], tuple(sorted(c["pins"].items())))].append(c["des"])
    for (name, fp, _), des_list in sig.items():
        if len(des_list) > 1:
            out.append(("E", "疑似重复放置(型号+封装+网络完全相同, 若都贴装会硬并联): %s → %s"
                        % (name or "?", ", ".join(des_list))))

    # 单引脚网络
    for n, ps in sorted(net2pins.items()):
        if len(ps) == 1:
            out.append(("E", "只有 1 个引脚的网络(=悬空/漏连): %s ← %s" % (n, ps[0])))

    # 未贴装
    ncs = [c["des"] for c in comps if c["nc"]]
    if ncs:
        out.append(("W", "未贴装器件 (Name 含 NC/DNP): %s" % ", ".join(ncs)))

    # 电源网络无电容
    caps_on = defaultdict(int)
    for c in comps:
        if re.match(r"^C", c["des"]) and not c["nc"]:
            for net in c["pins"].values():
                if net:
                    caps_on[net] += 1
    for n, ps in net2pins.items():
        if not PWR_PAT.match(n):
            continue
        if n.upper().startswith("GND"):
            continue
        if caps_on.get(n, 0) == 0 and len(ps) >= 2:
            out.append(("W", "电源网络 %s 上没有任何电容 (去耦/储能缺失) → %s"
                        % (n, " ".join(sorted(ps)))))
    return out


def main():
    ap = argparse.ArgumentParser(description="EasyEDA Pro .enet 网表分析器")
    ap.add_argument("enet")
    ap.add_argument("--issues", action="store_true", help="只打印问题清单")
    ap.add_argument("--net", help="追踪指定网络到所有引脚")
    ap.add_argument("--filter", help="只显示位号前缀在此列表中的器件, 如 U,R,C")
    ap.add_argument("--csv", help="额外导出元件表为 CSV")
    ap.add_argument("--no-nets", action="store_true", help="不打印网络表")
    args = ap.parse_args()

    enet = load(args.enet)
    comps, net2pins = parse(enet)
    iss = issues(comps, net2pins)

    if args.net:
        target = args.net
        match = [n for n in net2pins if n == target]
        if not match:
            match = [n for n in net2pins if target.lower() in n.lower()]
        if not match:
            print("[无此网络] %s" % target)
            print("可用网络: %s" % ", ".join(sorted(net2pins)))
            return 1
        for n in match:
            ps = sorted(net2pins[n], key=lambda x: des_key(x.split(".")[0]))
            print("网络 %s  (%d 个引脚)" % (n, len(ps)))
            for p in ps:
                c = next((c for c in comps if c["des"] == p.split(".")[0]), None)
                extra = ""
                if c:
                    extra = "   %-24s %s" % (c["name"][:24], c["fp"][:30])
                print("   %-10s%s" % (p, extra))
        return 0

    print("=" * 74)
    print(" %s" % os.path.basename(args.enet))
    print(" 器件 %d 个 | 网络 %d 个 | 问题 %d 条 (E=%d W=%d)"
          % (len(comps), len(net2pins), len(iss),
             sum(1 for s, _ in iss if s == "E"), sum(1 for s, _ in iss if s == "W")))
    print("=" * 74)

    print("\n■ 问题清单")
    if not iss:
        print("   (无)")
    for sev, msg in iss:
        print("   [%s] %s" % (sev, msg))

    if not args.issues:
        flt = set(x.strip().upper() for x in args.filter.split(",")) if args.filter else None
        print("\n■ 元件表")
        print("   %-7s %-26s %-30s %s" % ("位号", "器件名", "封装", "引脚→网络"))
        for c in comps:
            pre = re.match(r"^[A-Za-z_]+", c["des"])
            if flt and (not pre or pre.group(0).upper() not in flt):
                continue
            pl = " ".join("%s:%s" % (k, v or "-") for k, v in
                          sorted(c["pins"].items(), key=lambda x: des_key(x[0])))
            mark = " (NC)" if c["nc"] else ""
            print("   %-7s %-26s %-30s %s" % (c["des"], (c["name"] + mark)[:25],
                                              c["fp"][:29], pl))

        if not args.no_nets:
            print("\n■ 网络表 (按引脚数降序)")
            for n, ps in sorted(net2pins.items(), key=lambda x: -len(x[1])):
                print("   %-22s (%2d) %s" % (n, len(ps),
                                             " ".join(sorted(ps, key=lambda x: des_key(x.split(".")[0])))))

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8-sig") as f:
            w = csv.writer(f)
            w.writerow(["位号", "器件名", "封装", "未贴装", "引脚", "网络"])
            for c in comps:
                for num, net in sorted(c["pins"].items(), key=lambda x: des_key(x[0])):
                    w.writerow([c["des"], c["name"], c["fp"], "Y" if c["nc"] else "",
                                num, net])
        print("\n[已导出 CSV] %s" % args.csv)

    return 1 if any(s == "E" for s, _ in iss) else 0


if __name__ == "__main__":
    sys.exit(main())
