#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sprite_sheet.py — 把一批 1bpp BMP 拼成一张 PNG 总览图 (纯标准库, 不需要 Pillow)。

为什么要有它
------------
复刻 gmud.online 时我犯过一次错: 只看 JSON 字段就断言"238 个精灵",
用户直接质疑"哪有什么精灵, 都是 NPC"。正确做法是**把图拼出来自己看**,
看清 id 区间究竟对应地形纹理 / 墙 / 门 / 物件 / 人物, 再写代码。

用法
----
    python3 tools/sprite_sheet.py --dir /tmp/gmud_art/img/res --out /tmp/sheet_res.png
    python3 tools/sprite_sheet.py --dir /tmp/gmud_art/img/res --from 430 --to 511 --scale 3
"""
import argparse
import glob
import os
import re
import struct
import zlib
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bmp_dump import load_bmp        # 复用同一套 BMP 解码 (1/4/8/24 bpp)


def write_png(path, w, h, gray):
    """gray: bytes, 每像素 1 字节 (0=黑 255=白)。写 8bit 灰度 PNG。"""
    raw = b"".join(b"\x00" + bytes(gray[y * w:(y + 1) * w]) for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


# 5x7 点阵数字+字母, 用来在总览图上标 id (否则几百个格子根本对不上号)
GLYPH = {
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11111", "00010", "00100", "00010", "00001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
}


def draw_label(buf, W, x, y, text, scale=1):
    cx = x
    for ch in text:
        g = GLYPH.get(ch)
        if not g:
            cx += 6 * scale
            continue
        for r, row in enumerate(g):
            for c, v in enumerate(row):
                if v != "1":
                    continue
                for dy in range(scale):
                    for dx in range(scale):
                        px, py = cx + c * scale + dx, y + r * scale + dy
                        if 0 <= px < W:
                            buf[py * W + px] = 0
        cx += 6 * scale


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--pattern", default="resmap*.bmp")
    ap.add_argument("--out", default="/tmp/sheet.png")
    ap.add_argument("--from", dest="lo", type=int, default=-1)
    ap.add_argument("--to", dest="hi", type=int, default=1 << 30)
    ap.add_argument("--cols", type=int, default=16)
    ap.add_argument("--scale", type=int, default=2, help="每像素放大倍数")
    ap.add_argument("--label", action="store_true", help="在每格左上角标 id")
    a = ap.parse_args()

    files = []
    for p in sorted(glob.glob(os.path.join(a.dir, a.pattern))):
        m = re.search(r"(\d+)", os.path.basename(p))
        idx = int(m.group(1)) if m else len(files)
        if a.lo <= idx <= a.hi:
            files.append((idx, p))
    if not files:
        raise SystemExit("没有匹配的文件")

    cell = 32
    cols = a.cols
    rows = (len(files) + cols - 1) // cols
    W = cols * cell * a.scale
    H = rows * cell * a.scale
    buf = bytearray([255]) * (W * H)

    for n, (idx, p) in enumerate(files):
        w, h, bpp, px = load_bmp(p)
        cx = (n % cols) * cell * a.scale
        cy = (n // cols) * cell * a.scale
        for y in range(min(cell, h)):
            for x in range(min(cell, w)):
                if px[y][x]:
                    continue                      # 1=白(纸), 不画
                for dy in range(a.scale):
                    for dx in range(a.scale):
                        buf[(cy + y * a.scale + dy) * W + cx + x * a.scale + dx] = 0
        # 格子边框
        for x in range(cell * a.scale):
            k = (cy + cell * a.scale - 1) * W + cx + x
            if k < len(buf):
                buf[k] = 200
        if a.label:
            draw_label(buf, W, cx + 1, cy + 1, str(idx), 1)

    write_png(a.out, W, H, buf)
    ids = [i for i, _ in files]
    print("拼了 %d 张 (%d..%d), %dx%d → %s" % (len(files), min(ids), max(ids), W, H, a.out))


if __name__ == "__main__":
    main()
