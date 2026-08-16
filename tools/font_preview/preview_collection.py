# -*- coding: utf-8 -*-
"""7款原生点阵字体 × 16/24/32 预览 (简体, 已验证)"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw, ImageFont

BASE = '/tmp/fz/gz2'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')

SAMPLE = ("他抬起头，看了一眼窗外的雨，又低下头继续写着。\n"
          "“这个字念什么？”她指着书页上的一个字问。\n"
          "我说：“念‘絮’，柳絮的絮。”她若有所思地点点头。\n"
          "夜里，风很大，院子里的梧桐树被吹得哗哗响。")

def load_collection(path, pix):
    d = open(path, 'rb').read()
    MAP_N = 6951
    hdr = MAP_N * 4
    hzsize = pix * pix // 8
    m = {}
    for i in range(MAP_N):
        uni = (d[i*4] << 8) | d[i*4+1]
        pos = (d[i*4+2] << 8) | d[i*4+3]
        m[uni] = pos
    cache = {}
    def glyph(cp):
        if cp in cache: return cache[cp]
        pos = m.get(cp)
        if pos is None: return None
        raw = d[hdr + pos*hzsize : hdr + (pos+1)*hzsize]
        wb = (pix+7)//8
        rows = [[(raw[y*wb + x//8] >> (7-x%8)) & 1 for x in range(pix)] for y in range(pix)] if len(raw) >= hzsize else None
        cache[cp] = rows
        return rows
    return glyph

def render(glyph, pix, text):
    line_h = pix + 4
    W, H, margin = 400, 300, 8
    cols = W - 2*margin
    lines, cur, cur_w = [], [], 0
    for ch in text:
        if ch == '\n':
            lines.append(cur); cur, cur_w = [], 0; continue
        cw = pix
        if cur_w + cw > cols and cur:
            lines.append(cur); cur, cur_w = [], 0
        cur.append(ch); cur_w += cw
    if cur: lines.append(cur)
    rows = [[0]*W for _ in range(min(line_h*len(lines), H))]
    for li, line in enumerate(lines):
        if margin + li*line_h >= H: break
        used = sum(pix for c in line)
        extra = cols - used
        gaps = len(line)-1
        per = rem = 0
        if gaps > 0 and extra > 0 and len(line) > 1 and used*3 >= cols*2:
            per, rem = divmod(extra, gaps)
        x = margin
        for ci, ch in enumerate(line):
            g = glyph(ord(ch))
            y0 = margin + li*line_h
            if g:
                for yy in range(pix):
                    for xx in range(pix):
                        if g[yy][xx] and y0+yy < len(rows) and x+xx < W:
                            rows[y0+yy][x+xx] = 1
            x += pix
            if gaps > 0 and ci < gaps:
                x += per + (1 if ci < rem else 0)
    return rows

def save(rows, label, out, scale=2):
    H, W = len(rows), len(rows[0])
    img = Image.new('L', (W*scale, H*scale + 40), 255)
    d = ImageDraw.Draw(img)
    try:
        f = ImageFont.truetype('/System/Library/Fonts/Supplemental/Songti.ttc', 26)
    except Exception:
        f = ImageFont.load_default()
    d.text((6, 4), label, fill=0, font=f)
    px = img.load()
    for y in range(H):
        for x in range(W):
            c = 255 if rows[y][x] == 0 else 0
            for dy in range(scale):
                for dx in range(scale):
                    px[x*scale+dx, y*scale+40+dy] = c
    img.save(out)
    print('saved', os.path.basename(out))

if __name__ == '__main__':
    styles = ['宋体','仿宋','黑体','楷体','隶书','幼圆','小标宋']
    sizes = {16: '16x16', 24: '24x24', 32: '32x32'}
    for st in styles:
        for pix, sub in sizes.items():
            glyph = load_collection(f'{BASE}/{sub}/HZK{pix}_{st}', pix)
            rows = render(glyph, pix, SAMPLE)
            save(rows, f'{st} {pix}px (原生点阵)', os.path.join(OUT, f'col_{st}_{pix}.png'))
