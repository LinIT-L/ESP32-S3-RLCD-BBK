#!/usr/bin/env python3
"""按"节 + 归属库/组件"聚合 map 文件占用, 回答"内部 RAM 到底被谁吃掉".

用法: python3 mem_iram_attr.py build/LinTOS.map [.iram0.text .dram0.bss ...]
"""
import re, sys, collections

mapfile = sys.argv[1] if len(sys.argv) > 1 else "build/LinTOS.map"
targets = sys.argv[2:] or [".iram0.text", ".dram0.bss", ".dram0.data"]

TOP     = re.compile(r'^(\.\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s*$')
# 写法1: 地址/大小/归属 独立一行(前面有缩进)
PAT_A   = re.compile(r'^\s+0x([0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S+)$')
# 写法2: 子节名 + 地址 + 大小 + 归属 同一行
PAT_B   = re.compile(r'^\s+\.\S+\s+0x([0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(\S+)$')

agg = {t: collections.Counter() for t in targets}
titles = {}
cur = None
with open(mapfile, errors="replace") as f:
    for line in f:
        line = line.rstrip("\n")
        m = TOP.match(line)
        if m:
            sect = m.group(1)
            cur = sect if sect in targets else None
            if cur:
                titles[cur] = (m.group(2), int(m.group(3), 16))
            continue
        if not cur:
            continue
        c = PAT_A.match(line) or PAT_B.match(line)
        if not c:
            continue
        owner = c.group(3)
        if owner.startswith("0x"):        # 纯大小行(对齐/padding), 跳过
            continue
        lib = owner.split("(")[0]         # 归到库/组件级
        agg[cur][lib] += int(c.group(2), 16)

for t in targets:
    if t not in titles:
        continue
    addr, total = titles[t]
    a = agg[t]
    s = sum(a.values())
    print(f"\n===== {t}  起始 0x{addr}  节大小 {total} B (归集 {s} B / {len(a)} 个库) =====")
    for name, n in a.most_common(16):
        pct = n * 100.0 / total if total else 0
        bar = "#" * int(pct)
        print(f"  {n:8d} B {pct:5.1f}% {bar:<16} {name}")
    print(f"  ---- 已归集 {s} B, 未归集(对齐/padding/弱符号) {total - s} B")
