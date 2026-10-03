#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""qqt_sheet.py — 把解出来的 QQ堂 .img 拼成总览图 (纯标准库)。

用法
----
    python3 tools/qqt_sheet.py --root /tmp/qt --out /tmp/qqt_all.png --take 240
"""
import argparse
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qqt_img import parse, decode_frame      # noqa: E402
from sprite_sheet import write_png           # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="/tmp/qt")
    ap.add_argument("--out", default="/tmp/qqt_all.png")
    ap.add_argument("--take", type=int, default=240, help="最多拼多少张 (均匀抽样)")
    ap.add_argument("--cell", type=int, default=72)
    ap.add_argument("--cols", type=int, default=16)
    a = ap.parse_args()

    cands = []
    for p in sorted(glob.glob(os.path.join(a.root, "**", "*.img"), recursive=True)):
        im = parse(p)
        if im and im["exact"] and im["dw"] >= 8 and im["dh"] >= 8:
            cands.append((p, im))
    total = len(cands)
    step = max(1, total // a.take)
    sel = cands[::step][:a.take]

    CELL, COLS = a.cell, a.cols
    rows = (len(sel) + COLS - 1) // COLS
    W, H = COLS * CELL, rows * CELL
    buf = bytearray([255]) * (W * H)
    drawn = 0
    for k, (p, im) in enumerate(sel):
        r = decode_frame(im, 0)
        if not r:
            continue
        ox, oy = (k % COLS) * CELL, (k // COLS) * CELL
        sw, sh = im["dw"], im["dh"]
        kk = min((CELL - 4) / sw, (CELL - 4) / sh)
        dw, dh = max(1, int(sw * kk)), max(1, int(sh * kk))
        for y in range(dh):
            sy = min(sh - 1, int(y / kk))
            for x in range(dw):
                buf[(oy + 2 + y) * W + ox + 2 + x] = r[sy][min(sw - 1, int(x / kk))]
        drawn += 1
    write_png(a.out, W, H, buf)
    print("→ %s  (%dx%d, 画了 %d 张 / 可出图共 %d)" % (a.out, W, H, drawn, total))


if __name__ == "__main__":
    main()
