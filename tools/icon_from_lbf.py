#!/usr/bin/env python3
"""从 LBF 取真实字形，生成 128x128 黑白 PNG 图标。

规则（按需求）：
  1. 优先用字体本身渲染「字体」两字并排；
  2. 缺字则退化为单字「字」；
  3. 再缺则该字体无法出图标，明确报告（不用别的字体冒充）；
  4. 纯黑白、居中、统一画布 128x128。
"""
import struct
import sys
from PIL import Image

CANVAS = 128
MARGIN = 6           # 四周留白（收紧到 6，让两字尽量占满 128 画布）
CP_FONT = 0x5B57     # 字
CP_TI = 0x4F53       # 体


class Lbf:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.buf = f.read()
        b = self.buf
        magic, = struct.unpack_from('<I', b, 0)
        if magic != 0x3146424C:
            raise ValueError('magic 不符: %08X' % magic)
        self.ver_major, self.ver_minor = struct.unpack_from('<HH', b, 4)
        self.native_w, self.native_h = b[16], b[17]
        self.baseline = struct.unpack_from('<b', b, 18)[0]
        self.count, = struct.unpack_from('<I', b, 20)
        self.cmap_off, self.idx_off, self.data_off = struct.unpack_from('<III', b, 24)
        self.name = b[48:80].split(b'\x00')[0].decode('utf-8', 'replace')

    def find(self, cp):
        lo, hi = 0, self.count - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            v, = struct.unpack_from('<I', self.buf, self.cmap_off + mid * 4)
            if v == cp:
                return mid
            if v < cp:
                lo = mid + 1
            else:
                hi = mid - 1
        return -1

    def glyph(self, cp):
        """返回 (w, h, xoff, yoff, rows) —— rows 是每行的 bit 列表"""
        i = self.find(cp)
        if i < 0:
            return None
        off, w, h, xoff, yoff = struct.unpack_from('<IBBbb', self.buf, self.idx_off + i * 8)
        if w == 0 or h == 0:
            return None
        bpr = (w + 7) // 8
        base = self.data_off + off
        rows = []
        for y in range(h):
            row = []
            for x in range(w):
                byte = self.buf[base + y * bpr + (x >> 3)]
                row.append((byte >> (7 - (x & 7))) & 1)
            rows.append(row)
        return (w, h, xoff, yoff, rows)


def compose(glyphs):
    """把多个字形按各自 w 水平并排, 按 baseline 对齐"""
    total_w = sum(g[0] for g in glyphs)
    total_h = max(g[1] for g in glyphs)
    canvas = [[0] * total_w for _ in range(total_h)]
    x0 = 0
    for (w, h, xoff, yoff, rows) in glyphs:
        for y in range(h):
            for x in range(w):
                if rows[y][x]:
                    canvas[y][x0 + x] = 1
        x0 += w
    return total_w, total_h, canvas


def make_icon(src, out_path):
    f = Lbf(src)
    g1 = f.glyph(CP_FONT)
    g2 = f.glyph(CP_TI)
    used = None
    if g1 and g2:
        used = '字体'
        glyphs = [g1, g2]
    elif g1:
        used = '字'
        glyphs = [g1]
    elif g2:
        used = '体'
        glyphs = [g2]
    else:
        return None, '缺「字」与「体」两字，无法用本字体出图标'

    tw, th, cells = compose(glyphs)

    inner = CANVAS - MARGIN * 2
    scale = min(inner // tw, inner // th)
    if scale < 1:
        scale = 1
    dw, dh = tw * scale, th * scale
    ox = (CANVAS - dw) // 2
    oy = (CANVAS - dh) // 2

    im = Image.new('RGBA', (CANVAS, CANVAS), (0, 0, 0, 0))
    px = im.load()
    for y in range(th):
        for x in range(tw):
            if not cells[y][x]:
                continue
            for dy in range(scale):
                for dx in range(scale):
                    px[ox + x * scale + dx, oy + y * scale + dy] = (0, 0, 0, 255)
    im.save(out_path)
    return {'glyph': used, 'native': '%dx%d' % (tw, th), 'scale': scale,
            'size': (dw, dh), 'name': f.name}, None


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print('用法: icon_from_lbf.py <in.lbf> <out.png>')
        sys.exit(1)
    info, err = make_icon(sys.argv[1], sys.argv[2])
    if err:
        print('FAIL', err)
        sys.exit(2)
    print('OK', info)
