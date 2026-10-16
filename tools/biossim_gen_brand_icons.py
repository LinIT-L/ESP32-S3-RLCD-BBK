#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""biossim_gen_brand_icons.py — 生成/校验掌机版 BIOS模拟器 的**品牌图标**（64x64 1bpp）

linit 需求："不同品牌不需要介绍，只要图标 + 图标下面显示品牌名称。图标可以网上下载，
然后制作或者我后续替换。规格用 64x64。"

所以图标是**可替换的资源**，放在：
    apps/biossim/brand_icons/<brand>.pbm      (P4 二进制 1bpp, 64x64 = 512 字节)
你只要把自己的 64x64 图覆盖进去（或给我 PNG，我转成 .pbm），重跑
    python3 tools/biossim_gen_brand_icons.py --keep      # 校验规格
    python3 tools/biossim_gen_lua.py                     # 把图标嵌进 Lua
就换好了。**本脚本默认不覆盖已存在的 .pbm**（保护你替换过的图）。

用法:
    python3 tools/biossim_gen_brand_icons.py           # 只补齐缺失的（占位图）
    python3 tools/biossim_gen_brand_icons.py --force   # 全部重画成占位图（会覆盖你的）
    python3 tools/biossim_gen_brand_icons.py --check   # 只校验：存在 + 尺寸 64x64 + P4

占位图长什么样：统一的"显示器"外形 + 屏幕里是品牌缩写（5x7 点阵 2 倍放大）。
统一外形是为了看起来是一套；你换成真实 logo 后风格自然就统一了。
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTDIR = os.path.join(ROOT, "apps", "biossim", "brand_icons")

ICON_W = ICON_H = 64

BRANDS = [
    ("award",  "AWARD"),
    ("ami",    "AMI"),
    ("asus",   "ASUS"),
    ("dell",   "DELL"),
    ("hp",     "HP"),
    ("ibm",    "IBM"),
    ("huawei", "HUAWEI"),
]

# ---------------------------------------------------------------- 5x7 点阵字模
# 每行 5 位（'#'=1）；只放画图标用得到的字母。
FONT5X7 = {
    'A': [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    'B': ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    'D': ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    'E': ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    'H': ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    'I': ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"],
    'L': ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    'M': ["#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"],
    'P': ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    'R': ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    'S': [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    'U': ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    'W': ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"],
}


class Canvas:
    """64x64 1bpp 画布（0=纸白, 1=墨黑）"""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[0] * w for _ in range(h)]

    def put(self, x, y, v=1):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = v

    def hline(self, x0, x1, y, v=1):
        for x in range(x0, x1 + 1):
            self.put(x, y, v)

    def vline(self, x, y0, y1, v=1):
        for y in range(y0, y1 + 1):
            self.put(x, y, v)

    def box(self, x0, y0, x1, y1, th=1, v=1):
        """空心矩形，th 像素线宽（向内）"""
        for t in range(th):
            self.hline(x0 + t, x1 - t, y0 + t, v)
            self.hline(x0 + t, x1 - t, y1 - t, v)
            self.vline(x0 + t, y0 + t, y1 - t, v)
            self.vline(x1 - t, y0 + t, y1 - t, v)

    def fill(self, x0, y0, x1, y1, v=1):
        for y in range(y0, y1 + 1):
            self.hline(x0, x1, y, v)

    def text(self, x, y, s, scale=2):
        cx = x
        for ch in s:
            rows = FONT5X7.get(ch)
            if not rows:
                cx += (5 + 1) * scale
                continue
            for ry, row in enumerate(rows):
                for rx, c in enumerate(row):
                    if c != '#':
                        continue
                    for dy in range(scale):
                        for dx in range(scale):
                            self.put(cx + rx * scale + dx, y + ry * scale + dy, 1)
            cx += (5 + 1) * scale
        return cx - scale      # 返回右边界（去掉最后那格的间隔）

    def p4(self):
        """P4 二进制位图（每行按字节对齐；64 宽正好 8 字节）"""
        stride = (self.w + 7) // 8
        body = bytearray()
        for y in range(self.h):
            row = bytearray(stride)
            for x in range(self.w):
                if self.px[y][x]:
                    row[x >> 3] |= 0x80 >> (x & 7)
            body += row
        return b"P4\n%d %d\n" % (self.w, self.h) + bytes(body)


def make_icon(abbr):
    c = Canvas(ICON_W, ICON_H)
    # 显示器外框（2px）+ 屏内留白
    c.box(3, 5, 60, 42, th=2)
    c.fill(6, 8, 57, 39, 0)          # 屏幕内部清白
    # 屏幕里的品牌缩写（2 倍放大，居中）
    tw = len(abbr) * 12 - 2
    th = 14
    tx = 6 + max(0, (57 - 6 + 1 - tw) // 2)
    ty = 8 + max(0, (39 - 8 + 1 - th) // 2)
    c.text(tx, ty, abbr, scale=2)
    # 支架 + 底座
    c.fill(27, 43, 36, 47, 1)
    c.fill(17, 48, 46, 50, 1)
    return c.p4()


def check(path):
    d = open(path, 'rb').read()
    head, _, body = d.partition(b'\n')
    if head.strip() != b'P4':
        return '不是 P4 格式'
    dims, _, _ = body.partition(b'\n')
    try:
        w, h = [int(x) for x in dims.split()]
    except ValueError:
        return '尺寸行无法解析'
    if (w, h) != (ICON_W, ICON_H):
        return '尺寸 %dx%d (要求 %dx%d)' % (w, h, ICON_W, ICON_H)
    if len(d) - len(head) - 1 - len(dims) - 1 != (w // 8) * h:
        return '数据长度不对(应为 %d 字节)' % ((w // 8) * h)
    return None


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    force = '--force' in sys.argv
    only_check = '--check' in sys.argv
    bad = 0
    for key, abbr in BRANDS:
        p = os.path.join(OUTDIR, key + '.pbm')
        if only_check:
            if not os.path.exists(p):
                print('  ✗ %-8s 缺失' % key); bad += 1; continue
            err = check(p)
            if err:
                print('  ✗ %-8s %s' % (key, err)); bad += 1
            else:
                print('  ✓ %-8s 64x64 P4' % key)
            continue
        if os.path.exists(p) and not force:
            err = check(p)
            print('  · %-8s 已存在, 保留不动%s' % (key, '' if not err else '  ⚠ ' + err))
            continue
        open(p, 'wb').write(make_icon(abbr))
        print('  ✓ %-8s 已生成占位图（%s）' % (key, abbr))
    if only_check and bad:
        raise SystemExit('有 %d 个图标不合格' % bad)
    if not only_check:
        print('\n目录: %s' % OUTDIR)
        print('替换方法: 把你的 64x64 图存成 <品牌>.pbm(P4 1bpp) 覆盖进去, 然后重跑')
        print('          python3 tools/biossim_gen_lua.py   （图标会嵌进 Lua 的 BRAND_ICON 表）')


if __name__ == '__main__':
    main()
