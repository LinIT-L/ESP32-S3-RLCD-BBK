#!/usr/bin/env python3
"""verify_pal.py — 把 pal_feasibility 的 RESULT 行与 **ZIP 原件**逐字节核对。

用法：
    /tmp/palf > /tmp/palf.out
    python3 tests/lpk/verify_pal.py /tmp/pal_fix/pal_stored.zip /tmp/palf.out

为什么单独一步：C 侧算的是「从 LPK 里读出来的字节」的 CRC，这里用 zipfile 解出
同名条目的字节再算一次 —— 两边的输入完全独立，只有真的一致才算通过。
"""
import sys
import zipfile
import binascii
import re

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(2)

zip_path, out_path = sys.argv[1], sys.argv[2]

# 收集 C 侧结果
res = {}
for line in open(out_path, encoding="utf-8", errors="replace"):
    m = re.match(r"\s*RESULT (\S+) size=(\d+) crc=([0-9a-f]{8})", line)
    if m:
        res[m.group(1)] = (int(m.group(2)), int(m.group(3), 16))

if not res:
    print("没有找到 RESULT 行 —— 先跑测试再把输出存下来")
    sys.exit(2)

zf = zipfile.ZipFile(zip_path)
names = set(zf.namelist())
print("ZIP 条目数: %d | 待核对: %d" % (len(names), len(res)))

bad = 0
for name, (sz, crc) in sorted(res.items()):
    if name not in names:
        print("  FAIL  ZIP 里没有 %s" % name)
        bad += 1
        continue
    data = zf.read(name)
    zcrc = binascii.crc32(data) & 0xFFFFFFFF
    ok = (len(data) == sz and zcrc == crc)
    print("  %s  %-14s size %d/%d  crc %08x/%08x" %
          ("PASS" if ok else "FAIL", name, sz, len(data), crc, zcrc))
    if not ok:
        bad += 1

print("\n==== %d FAIL ====" % bad)
sys.exit(1 if bad else 0)
