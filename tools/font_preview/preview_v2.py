# -*- coding: utf-8 -*-
"""二次确认预览: 仿宋16 / 楷体24 / 宋体24 + 补充 20/28 号 + 对比图"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw, ImageFont

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hzk')
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')

SAMPLE = ("他抬起头，看了一眼窗外的雨，又低下头继续写着。\n"
          "“这个字念什么？”她指着书页上的一个字问。\n"
          "我说：“念‘絮’，柳絮的絮。”她若有所思地点点头。\n"
          "夜里，风很大，院子里的梧桐树被吹得哗哗响。\n"
          "“那……我们明天还去河边吗？”她小声地问。\n"
          "我没有回答，只是默默地把那本书收进了怀里。")

def hzk_glyph(hi, lo, cell, path):
    d = open(path, 'rb').read()
    idx = ((hi - 0xA1) * 94 + (lo - 0xA1)) * (cell * cell // 8)
    raw = d[idx:idx + cell * cell // 8]
    if len(raw) < cell * cell // 8:
        return None
    return [[(raw[(y * cell + x) // 8] >> (7 - (y * cell + x) % 8)) & 1 for x in range(cell)] for y in range(cell)]

def scale_nn(bm, tgt):
    h, w = len(bm), len(bm[0])
    return [[bm[min(h - 1, y * h // tgt)][min(w - 1, x * w // tgt)] for x in range(tgt)] for y in range(tgt)]

def render(path, src_cell, text, tgt, scale_to=None):
    line_h = tgt + 4
    W, H, margin = 400, 300, 8
    cols = W - 2 * margin
    lines, cur, cur_w = [], [], 0
    for ch in text:
        if ch == '\n':
            lines.append(cur); cur, cur_w = [], 0; continue
        cw = tgt
        if cur_w + cw > cols and cur:
            lines.append(cur); cur, cur_w = [], 0
        cur.append(ch); cur_w += cw
    if cur: lines.append(cur)
    rows = [[0] * W for _ in range(min(line_h * len(lines), H))]
    for li, line in enumerate(lines):
        if margin + li * line_h >= H: break
        used = sum(tgt for c in line)
        extra = cols - used
        gaps = len(line) - 1
        per = rem = 0
        if gaps > 0 and extra > 0 and len(line) > 1 and used * 3 >= cols * 2:
            per, rem = divmod(extra, gaps)
        x = margin
        for ci, ch in enumerate(line):
            hi, lo = ch.encode('gb2312')
            bm = hzk_glyph(hi, lo, src_cell, path)
            if bm is None:
                x += tgt; continue
            if scale_to and scale_to != src_cell:
                bm = scale_nn(bm, scale_to)
            y0 = margin + li * line_h
            for yy in range(tgt):
                for xx in range(tgt):
                    if bm[yy][xx] and y0 + yy < len(rows) and x + xx < W:
                        rows[y0 + yy][x + xx] = 1
            x += tgt
            if gaps > 0 and ci < gaps:
                x += per + (1 if ci < rem else 0)
    return rows

def save(rows, label, out, scale=2):
    H, W = len(rows), len(rows[0])
    img = Image.new('L', (W * scale, H * scale + 40), 255)
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
                    px[x * scale + dx, y * scale + 40 + dy] = c
    img.save(out)
    print('saved', os.path.basename(out))

# 样式: (名字, 源文件, 源字号)  × 目标字号列表
jobs = [
    ('仿宋', 'HZK24F', 24, [20, 24, 28]),   # 16 用 HZK16F
    ('楷体', 'HZK24K', 24, [20, 24, 28]),
    ('宋体', 'HZK24S', 24, [20, 24, 28]),
]
for style, fname, src, sizes in jobs:
    for tgt in sizes:
        if style == '仿宋' and tgt == 16:
            fname2, src2 = 'HZK16F', 16
            scale_to = 16
        elif style == '宋体' and tgt == 16:
            fname2, src2 = 'HZK16S', 16
            scale_to = 16
        else:
            fname2, src2, scale_to = fname, src, tgt
        rows = render(os.path.join(BASE, fname2), src2, SAMPLE, tgt, scale_to)
        nat = '原生' if (scale_to == src2) else f'{src2}→{tgt}缩放'
        save(rows, f'{style} {tgt}px ({nat})', os.path.join(OUT, f'v2_{style}_{tgt}.png'))

# 宋体 32 原生 (参考)
rows = render(os.path.join(BASE, 'HZK32'), 32, SAMPLE, 32, 32)
save(rows, '宋体 32px (原生)', os.path.join(OUT, 'v2_宋体_32.png'))

# 对比图: 仿宋24 / 楷体24 / 宋体24 三列
tiles = ['v2_仿宋_24.png', 'v2_楷体_24.png', 'v2_宋体_24.png']
TW, TH, PAD = 820, 640, 12
sheet = Image.new('L', (3 * TW + 4 * PAD, TH + 2 * PAD), 200)
for c, fn in enumerate(tiles):
    im = Image.open(os.path.join(OUT, fn))
    sheet.paste(im, (PAD + c * (TW + PAD), PAD))
sheet.save(os.path.join(OUT, 'v2_三款对比24.png'))
print('对比图 OK')
