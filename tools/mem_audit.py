#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mem_audit.py —— LinTOS 内部 RAM「静态占用」归属审计 (真值工具)

一句话: 回答「**到底是什么程序/组件在吃内部 RAM**」。
数据源 = 编译产物本身 (build/LinTOS.elf 符号表 + build/LinTOS.map 段布局),
不是注释、不是估算 —— 每一条都能追到源文件:行号。

用法:
    python tools/mem_audit.py                 # 完整报告
    python tools/mem_audit.py --top 20        # 只列 TOP20
    python tools/mem_audit.py --json base.json    # 存基线
    python tools/mem_audit.py --diff base.json    # 与基线的增量对比 (防回归)

判读要点:
  * 内部 DRAM 静态  +  内部堆(运行时)  =  整机内部 RAM 占用
  * 内部堆的运行时归属用设备命令: 串口 `mem` / `audit` / `mt` (见 docs/内存使用强制规范.md)
  * PSRAM 静态占用同样列出 (理论上应为「其余全部」)
"""
import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "LinTOS.elf")
MAP = os.path.join(ROOT, "build", "LinTOS.map")
NM_CANDIDATES = [
    os.path.expanduser("~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/"
                       "xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm"),
    os.path.expanduser("~/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/"
                       "xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm"),
]

# ---- 地址区间 (ESP32-S3) ----
R_DRAM = (0x3F800000, 0x3FD00000)   # 内部 DRAM (含 .dram0.data/.bss/.noinit + heap)
R_IRAM = (0x40370000, 0x40400000)   # 内部 IRAM (指令总线别名)
R_PSRAM = (0x3C400000, 0x3E000000)  # 外部 PSRAM (DROM 在 0x3C000000..0x3C3FFFFF)
R_DROM = (0x3C000000, 0x3C400000)   # flash rodata (不算 RAM)
R_ROM = (0x40000000, 0x40100000)    # ROM 代码
R_RTCRAM = (0x50000000, 0x50100000)


def region_of(addr):
    if R_DRAM[0] <= addr < R_DRAM[1]:
        return "内部DRAM"
    if R_IRAM[0] <= addr < R_IRAM[1]:
        return "内部IRAM"
    if R_PSRAM[0] <= addr < R_PSRAM[1]:
        return "PSRAM"
    if R_DROM[0] <= addr < R_DROM[1]:
        return "flash.rodata"
    if R_RTCRAM[0] <= addr < R_RTCRAM[1]:
        return "RTC"
    if R_ROM[0] <= addr < R_ROM[1]:
        return "ROM"
    return "其它"


def short_component(path):
    """源文件路径 → 组件名 (本工程 components/<cat>/<name> 或 esp-idf/<name>)"""
    p = path.replace("\\", "/")
    m = re.search(r"/esp-idf/([^/]+)/", p) or re.search(r"^esp-idf/([^/]+)/", p)
    if m:
        return "idf:" + m.group(1)
    m = re.search(r"/managed_components/([^/]+)/", p) or re.search(r"managed_components/([^/]+)/", p)
    if m:
        return "managed:" + m.group(1)
    m = re.search(r"/components/([^/]+)/([^/]+)/", p)
    if m:
        return "{}/{}".format(m.group(1), m.group(2))
    m = re.search(r"/components/([^/]+)/([^/.]+\.c)$", p)
    if m:
        return m.group(1)
    m = re.search(r"/main/([^/]+)$", p)
    if m:
        return "main"
    if "/build/" in p:
        return "?"
    return p.rsplit("/", 1)[-1]


def find_nm():
    env = os.environ.get("XTENSA_NM")
    if env and os.path.exists(env):
        return env
    for c in NM_CANDIDATES:
        if os.path.exists(c):
            return c
    out = subprocess.run(["which", "xtensa-esp32s3-elf-nm"],
                         capture_output=True, text=True).stdout.strip()
    if out:
        return out
    sys.exit("找不到 xtensa-esp32s3-elf-nm；设 XTENSA_NM=/path/to/nm")


def read_symbols():
    """nm -S -l --defined-only → [(size, type, name, region, srcfile)]"""
    nm = find_nm()
    out = subprocess.run([nm, "-S", "-l", "--defined-only", ELF],
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        # 形如: 3c2b637c 00000004 d NAME<TAB>/path/file.c:123
        m = re.match(r"^([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})\s+(\S)\s+(\S+)(?:\s+(.*))?$", line)
        if not m:
            continue
        addr, size, typ, name, src = (int(m.group(1), 16), int(m.group(2), 16),
                                      m.group(3), m.group(4), (m.group(5) or "").strip())
        if typ not in "bBdDtTrR":
            continue
        if size == 0:
            continue
        srcfile = src.split(":")[0] if src else "?"
        kind = "代码" if typ in "tTrR" else "数据"
        syms.append((size, typ, name, region_of(addr), kind, srcfile))
    return syms


def parse_map_regions():
    """从链接脚本/map 提取内部 DRAM 段布局 (真值)。"""
    if not os.path.exists(MAP):
        return None
    txt = open(MAP, errors="ignore").read()
    info = {}
    m = re.search(r"^\s*(dram0_0_seg)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", txt, re.M)
    if m:
        info["dram0_0_seg"] = (int(m.group(2), 16), int(m.group(3), 16))
    for sec in (".dram0.dummy", ".dram0.data", ".dram0.bss", ".noinit", ".ext_ram.bss"):
        m = re.search(r"^\s*" + re.escape(sec) + r"\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", txt, re.M)
        if m:
            info[sec] = (int(m.group(1), 16), int(m.group(2), 16))
    for sym in ("_heap_low_start", "_heap_start", "_bss_end", "_data_start", "_iram_text_start"):
        m = re.search(r"^\s*0x([0-9a-f]+)\s+" + re.escape(sym) + r"\s*=", txt, re.M)
        if m:
            info[sym] = (int(m.group(1), 16), 0)
    return info


def human(n):
    return "{:,}".format(n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--json", dest="json_out")
    ap.add_argument("--diff", dest="diff_in")
    ap.add_argument("--min", type=int, default=1024, help="单符号列表阈值 (字节)")
    args = ap.parse_args()

    if not os.path.exists(ELF):
        sys.exit("找不到 %s —— 先构建 (PYTHONPATH= ./build.sh)" % ELF)

    syms = read_symbols()

    by_region = defaultdict(int)
    by_region_comp = defaultdict(lambda: defaultdict(int))
    for size, typ, name, region, kind, src in syms:
        key = region + ("(代码)" if (region == "内部IRAM" and kind == "代码") else "")
        by_region[key] += size
        by_region_comp[key][short_component(src)] += size

    internal_static = (by_region.get("内部DRAM", 0)
                       + by_region.get("内部IRAM", 0)
                       + by_region.get("内部IRAM(代码)", 0))
    psram_static = by_region.get("PSRAM", 0)

    rep = {}
    print("=" * 74)
    print(" LinTOS 内部 RAM 静态占用审计  (源: build/LinTOS.elf 符号表)")
    print("=" * 74)
    print("  —— 按地址区间汇总 ——")
    for k in sorted(by_region, key=lambda k: -by_region[k]):
        print("   %-16s %10s B  (%6.1f KB)" % (k, human(by_region[k]), by_region[k] / 1024.0))
        rep[k] = by_region[k]
    print("   %-16s %10s B  (%6.1f KB)" % ("= 内部合计", human(internal_static),
                                          internal_static / 1024.0))
    print("   %-16s %10s B  (%6.1f KB)" % ("= PSRAM静态", human(psram_static),
                                          psram_static / 1024.0))

    mp = parse_map_regions()
    if mp:
        print("\n  —— 链接脚本内存布局 (真值) ——")
        if "dram0_0_seg" in mp:
            o, l = mp["dram0_0_seg"]
            print("   dram0_0_seg     0x%08X +0x%X (%s B)" % (o, l, human(l)))
        for sec in (".dram0.dummy", ".dram0.data", ".dram0.bss", ".noinit", ".ext_ram.bss"):
            if sec in mp:
                a, s = mp[sec]
                print("   %-16s 0x%08X  %8s B" % (sec, a, human(s)))
                rep[sec] = s

    for region in ("内部DRAM", "内部IRAM", "内部IRAM(代码)", "PSRAM"):
        if region not in by_region_comp:
            continue
        sk = region
        print("\n" + "=" * 74)
        print(" 【%s】按组件 (TOP %d)" % (sk, args.top))
        print("=" * 74)
        items = sorted(by_region_comp[sk].items(), key=lambda kv: -kv[1])[:args.top]
        for comp, sz in items:
            print("   %9s B  (%6.1f KB)  %s" % (human(sz), sz / 1024.0, comp))
        rep.setdefault("by_component", {})[sk] = dict(items)

    # 单符号大块
    print("\n" + "=" * 74)
    print(" 【单符号 ≥%d B】内部 RAM 优先 (真凶通常在这里)" % args.min)
    print("=" * 74)
    big = [s for s in syms if s[0] >= args.min and s[3] in ("内部DRAM", "内部IRAM")]
    for size, typ, name, region, kind, src in sorted(big, reverse=True)[:60]:
        rel = src.replace(ROOT + "/", "")
        print("   %9s B  %-9s %-34s %s" % (human(size), region, name[:34], rel[:60]))

    print("\n" + "=" * 74)
    print(" 【单符号 ≥%d B】PSRAM" % args.min)
    print("=" * 74)
    bigp = [s for s in syms if s[0] >= args.min and s[3] == "PSRAM"]
    for size, typ, name, region, kind, src in sorted(bigp, reverse=True)[:20]:
        rel = src.replace(ROOT + "/", "")
        print("   %9s B  %-9s %-34s %s" % (human(size), region, name[:34], rel[:60]))

    print("\n提示: 运行时堆归属用串口命令 `mem` / `audit` / `mt`；")
    print("      验收铁律见 docs/内存使用强制规范.md (largest ≥ 8KB)。")

    if args.json_out:
        json.dump(rep, open(args.json_out, "w"), indent=1, ensure_ascii=False)
        print("\n基线已存: %s" % args.json_out)

    if args.diff_in:
        base = json.load(open(args.diff_in))
        print("\n" + "=" * 74)
        print(" 与基线对比 (正数=变多/变差)")
        print("=" * 74)
        bcomp = base.get("by_component", {})
        for sk in ("内部DRAM", "内部IRAM", "PSRAM"):
            cur = rep.get("by_component", {}).get(sk, {})
            old = bcomp.get(sk, {})
            for comp in set(list(cur) + list(old)):
                d = cur.get(comp, 0) - old.get(comp, 0)
                if abs(d) >= 256:
                    print("   %+9s B  %s" % (human(d), comp))


if __name__ == "__main__":
    main()
