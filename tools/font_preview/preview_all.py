# -*- coding: utf-8 -*-
"""生成字体选择预览: 当前字库 vs HZK 经典点阵字库 (宋/黑/楷/仿宋) @ 16/24/32"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fnt_render import load_fnt, render_text, rows_to_png, glyph_bitmap
from PIL import Image, ImageDraw, ImageFont

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hzk')
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')

def hzk_glyph(hi, lo, cell, path, skip=0):
    d = open(path,'rb').read()[skip:]
    idx = ((hi-0xA1)*94 + (lo-0xA1)) * (cell*cell//8)
    raw = d[idx:idx+cell*cell//8]
    if len(raw) < cell*cell//8: return None
    return [[(raw[(y*cell+x)//8] >> (7-(y*cell+x)%8)) & 1 for x in range(cell)] for y in range(cell)]

def render_hzk_text(path, text, cell, skip=0, gap=0):
    line_h = cell + 4
    W, H, margin = 400, 300, 8
    cols = W - 2*margin
    lines, cur, cur_w = [], [], 0
    for ch in text:
        if ch == '\n':
            lines.append(cur); cur, cur_w = [], 0; continue
        cw = cell + gap
        if cur_w + cw > cols and cur:
            lines.append(cur); cur, cur_w = [], 0
        cur.append(ch); cur_w += cw
    if cur: lines.append(cur)
    rows = [[0]*W for _ in range(min(line_h*len(lines), H))]
    for li, line in enumerate(lines):
        if margin + li*line_h >= H: break
        used = sum(cell + gap for c in line)
        extra = cols - used
        gaps = len(line)-1
        per = rem = 0
        if gaps > 0 and extra > 0 and len(line) > 1 and used*3 >= cols*2:
            per, rem = divmod(extra, gaps)
        x = margin
        for ci, ch in enumerate(line):
            hi, lo = ch.encode('gb2312')
            bm = hzk_glyph(hi, lo, cell, path, skip)
            if bm is None:
                x += cell + gap; continue
            y0 = margin + li*line_h
            for yy in range(cell):
                for xx in range(cell):
                    if bm[yy][xx] and y0+yy < len(rows) and x+xx < W:
                        rows[y0+yy][x+xx] = 1
            x += cell + gap
            if gaps > 0 and ci < gaps:
                x += per + (1 if ci < rem else 0)
    return rows

def label_img(rows, label, out, scale=2):
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
    print('saved', out)

SAMPLE = ("他抬起头，看了一眼窗外的雨，又低下头继续写着。\n"
          "“这个字念什么？”她指着书页上的一个字问。\n"
          "我说：“念‘絮’，柳絮的絮。”她若有所思地点点头。\n"
          "夜里，风很大，院子里的梧桐树被吹得哗哗响。\n"
          "“那……我们明天还去河边吗？”她小声地问。\n"
          "我没有回答，只是默默地把那本书收进了怀里。")

# 当前字库 (设备正在用的)
for sz in (20, 24, 28, 32):
    f = load_fnt(f'components/book_reader/assets/book{sz}.fnt')
    rows = render_text(f, SAMPLE, sz, 400)
    label_img(rows, f'当前字库 {sz}px', os.path.join(OUT, f'now_{sz}.png'))

# HZK 候选: 样式 -> (文件, 字号)
cands = [
    ('HZK16S 宋体16', 'HZK16S', 16),
    ('HZK24S 宋体24', 'HZK24S', 24),
    ('HZK32  宋体32', 'HZK32', 32),
    ('HZK24H 黑体24', 'HZK24H', 24),
    ('HZK24K 楷体24', 'HZK24K', 24),
    ('HZK24F 仿宋24', 'HZK24F', 24),
    ('HZK16F 仿宋16', 'HZK16F', 16),
]
for lab, fn, sz in cands:
    rows = render_hzk_text(os.path.join(BASE, fn), SAMPLE, sz)
    label_img(rows, f'{lab}px', os.path.join(OUT, f'hzk_{fn}_{sz}.png'))

# 总览接触表: 2 列 x 4 行, 每格 800x640
grid_files = [
    [('now_24.png', '当前 24px'), ('hzk_HZK24S_24.png', '宋体 24px (HZK24S)')],
    [('hzk_HZK24H_24.png', '黑体 24px (HZK24H)'), ('hzk_HZK24K_24.png', '楷体 24px (HZK24K)')],
    [('hzk_HZK24F_24.png', '仿宋 24px (HZK24F)'), ('hzk_HZK16S_16.png', '宋体 16px (HZK16S)')],
    [('hzk_HZK32_32.png', '宋体 32px (HZK32)'), ('hzk_HZK16F_16.png', '仿宋 16px (HZK16F)')],
]
TW, TH, PAD = 800, 640, 12
sheet = Image.new('L', (2*TW + 3*PAD, 4*TH + 5*PAD), 200)
for r, row in enumerate(grid_files):
    for c, (fn, lab) in enumerate(row):
        im = Image.open(os.path.join(OUT, fn))
        sheet.paste(im, (PAD + c*(TW+PAD), PAD + r*(TH+PAD)))
sheet.save(os.path.join(OUT, '总览对比.png'))
print('sheet saved')
