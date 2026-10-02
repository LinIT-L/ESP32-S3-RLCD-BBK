#!/usr/bin/env python3
import subprocess, re, os, collections

TC = os.path.expanduser("~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin")
ELF = "/Users/linit/AI/LinTOS0.6/build/LinTOS.elf"
READELF = f"{TC}/xtensa-esp32s3-elf-readelf"

def rdelf(*args):
    return subprocess.check_output([READELF, *args, ELF], text=True)

secmap = {}
for line in rdelf("-SW").splitlines():
    m = re.match(r'\s*\[\s*(\d+)\]\s+(\S+)', line)
    if m:
        secmap[int(m.group(1))] = m.group(2)

groups = [
    ("内部DRAM (.dram0.*)", lambda s: s.startswith(".dram0") or s.startswith(".noinit")),
    ("内部IRAM (.iram*)", lambda s: s.startswith(".iram")),
    ("PSRAM (.ext_ram.*)", lambda s: s.startswith(".ext_ram")),
    ("Flash rodata (.rodata*)", lambda s: s.startswith(".rodata")),
]

syms = collections.defaultdict(list)
for line in rdelf("-sW").splitlines():
    m = re.match(r'\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.*)', line)
    if not m:
        continue
    size = int(m.group(2)); typ = m.group(3); ndx = m.group(6); name = m.group(7).strip()
    if size == 0 or not name or not ndx.isdigit():
        continue
    sec = secmap.get(int(ndx), "?")
    for gname, pred in groups:
        if pred(sec):
            syms[gname].append((size, typ, sec, name))
            break

for gname, _ in groups:
    lst = syms.get(gname, [])
    tot = sum(x[0] for x in lst)
    lst.sort(key=lambda x: -x[0])
    print(f"\n===== {gname} — 合计 {tot} 字节 / {len(lst)} 个符号 =====")
    for s, t, sec, n in lst[:22]:
        print(f"  {s:9d}  {t:6s} {sec:12s} {n}")
