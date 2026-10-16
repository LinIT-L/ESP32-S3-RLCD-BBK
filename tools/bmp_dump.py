#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bmp_dump.py — 把 1bpp / 24bpp Windows BMP 打成 ASCII 看内容。

用途: 复刻 gmud.online《英雄坛说》的菜单素材时, 先"眼睛看一遍"每张图长什么样,
      再决定它对应哪个 UI 元素 (指针/勾选/单选/经验球/翻页箭头…)。
不需要 Pillow: 直接解 BMP 头 + 行对齐, 单色按位展开, 24 位按亮度阈值二值化。
"""
import struct
import sys


def load_bmp(path):
    d = open(path, "rb").read()
    if d[:2] != b"BM":
        raise ValueError("不是 BMP: %s" % path)
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    flip = h > 0                      # 正高度 = 自下而上
    h = abs(h)
    px = [[0] * w for _ in range(h)]
    if bpp == 1:
        stride = ((w + 31) // 32) * 4
        for y in range(h):
            row = off + y * stride
            for x in range(w):
                byte = d[row + (x >> 3)]
                bit = (byte >> (7 - (x & 7))) & 1
                py = (h - 1 - y) if flip else y
                px[py][x] = 1 if bit else 0      # 1bpp: 1=黑
    elif bpp in (4, 8):
        stride = (((w * bpp + 31) // 32) * 4)
        for y in range(h):
            row = off + y * stride
            for x in range(w):
                if bpp == 8:
                    v = d[row + x]
                    bit = 1 if v < 128 else 0
                else:
                    b = d[row + (x >> 1)]
                    v = (b >> 4) if (x & 1) == 0 else (b & 0xF)
                    bit = 1 if v < 8 else 0
                py = (h - 1 - y) if flip else y
                px[py][x] = bit
    elif bpp == 24:
        stride = ((w * 3 + 3) // 4) * 4
        for y in range(h):
            row = off + y * stride
            for x in range(w):
                b, g, r = d[row + x * 3], d[row + x * 3 + 1], d[row + x * 3 + 2]
                lum = 0.299 * r + 0.587 * g + 0.114 * b
                py = (h - 1 - y) if flip else y
                px[py][x] = 1 if lum < 128 else 0
    else:
        raise ValueError("不支持的位深 %d: %s" % (bpp, path))
    return w, h, bpp, px


def show(path, on="#", off="."):
    w, h, bpp, px = load_bmp(path)
    print("%s  %dx%d x%d" % (path, w, h, bpp))
    for r in px:
        print("  " + "".join(on if v else off for v in r))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        show(p)
        print()
