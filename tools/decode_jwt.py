#!/usr/bin/env python3
import struct

P = "/Users/linit/Desktop/字体备份/jwt_dot.bin"
data = open(P, "rb").read()
N = len(data)
print("size", N)

# 1) header 2-byte words 0x78..0x300
print("\n== header words (LE) 0x70..0x300 ==")
for off in range(0x70, 0x300, 16):
    ws = [struct.unpack('<H', data[o:o+2])[0] for o in range(off, off+16, 2)]
    hs = ' '.join(f'{w:04x}' for w in ws)
    print(f"{off:04x}: {hs}")

# 2) scan for monotonic-increasing U32 tables across the whole file
def find_asc_tables(data, step=4, min_entries=4, max_abs_gap=0x100000):
    """find runs where consecutive u32 (LE) are strictly increasing with small gaps"""
    runs = []
    i = 0
    n = len(data)
    while i <= n - step:
        v0 = struct.unpack('<I', data[i:i+4])[0]
        if v0 == 0 or v0 >= 0x01000000:  # skip zero / suspicious
            i += step; continue
        run = [i]
        j = i + step
        prev = v0
        while j <= n - step:
            v = struct.unpack('<I', data[j:j+4])[0]
            if 0 < v - prev < max_abs_gap and v < 0x02000000:
                run.append(j); prev = v; j += step
            else:
                break
        if len(run) >= min_entries:
            # a run that is dense (most consecutive entries increasing)
            runs.append((run[0], run[-1]+4, len(run)))
        i = run[-1] + step if len(run) > 1 else i + step
    return runs

runs = find_asc_tables(data)
print("\n== ascending u32 tables (>=4 entries) ==")
print("count:", len(runs))
for s,e,l in runs[:80]:
    # first 3 & last entry value
    first = struct.unpack('<I', data[s:s+4])[0]
    lastval = struct.unpack('<I', data[e-4:e])[0]
    # avg gap
    gaps = []
    for o in range(s, e-4, 4):
        a = struct.unpack('<I', data[o:o+4])[0]
        b = struct.unpack('<I', data[o+4:o+8])[0]
        gaps.append(b-a)
    ag = sum(gaps)//max(1,len(gaps))
    print(f"  off=0x{s:08x} len={l:<6} first=0x{first:x} last=0x{lastval:x} avgGap={ag}")