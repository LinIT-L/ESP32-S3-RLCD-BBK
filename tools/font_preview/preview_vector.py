# -*- coding: utf-8 -*-
"""矢量字体 → 设备 1bpp 点阵渲染预览 (每字号原生渲染, 非缩放)"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw, ImageFont, ImageOps

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')

SAMPLE = ("他抬起头，看了一眼窗外的雨，又低下头继续写着。\n"
          "“这个字念什么？”她指着书页上的一个字问。\n"
          "我说：“念‘絮’，柳絮的絮。”她若有所思地点点头。\n"
          "夜里，风很大，院子里的梧桐树被吹得哗哗响。\n"
          "“那……我们明天还去河边吗？”她小声地问。\n"
          "我没有回答，只是默默地把那本书收进了怀里。")

def make_glyph_cache(font_path, cell):
    """预渲染常用字形: 每字一张 cell×cell 1bpp"""
    f = ImageFont.truetype(font_path, cell)
    cache = {}
    for ch in set(SAMPLE.replace('\n', '')):
        try:
            img = Image.new('L', (cell, cell), 255)
            d = ImageDraw.Draw(img)
            d.text((0, -1), ch, font=f, fill=0)
            # 阈值 1bpp
            img = img.point(lambda v: 255 if v > 127 else 0)
            cache[ch] = img
        except Exception:
            pass
    return cache

def render(font_path, cell, text, out):
    cache = make_glyph_cache(font_path, cell)
    line_h = cell + 4
    W, H, margin = 400, 300, 8
    cols = W - 2 * margin
    lines, cur, cur_w = [], [], 0
    for ch in text:
        if ch == '\n':
            lines.append(cur); cur, cur_w = [], 0; continue
        cw = cell
        if cur_w + cw > cols and cur:
            lines.append(cur); cur, cur_w = [], 0
        cur.append(ch); cur_w += cw
    if cur: lines.append(cur)
    rows = [[0] * W for _ in range(min(line_h * len(lines), H))]
    for li, line in enumerate(lines):
        if margin + li * line_h >= H: break
        used = sum(cell for c in line)
        extra = cols - used
        gaps = len(line) - 1
        per = rem = 0
        if gaps > 0 and extra > 0 and len(line) > 1 and used * 3 >= cols * 2:
            per, rem = divmod(extra, gaps)
        x = margin
        for ci, ch in enumerate(line):
            g = cache.get(ch)
            y0 = margin + li * line_h
            if g:
                px = g.load()
                for yy in range(cell):
                    for xx in range(cell):
                        if px[xx, yy] == 0 and y0 + yy < len(rows) and x + xx < W:
                            rows[y0 + yy][x + xx] = 1
            x += cell
            if gaps > 0 and ci < gaps:
                x += per + (1 if ci < rem else 0)
    # 保存
    img = Image.new('L', (W * 2, H * 2 + 40), 255)
    d2 = ImageDraw.Draw(img)
    try:
        f2 = ImageFont.truetype('/System/Library/Fonts/Supplemental/Songti.ttc', 26)
    except Exception:
        f2 = ImageFont.load_default()
    d2.text((6, 4), os.path.basename(out)[:-4], fill=0, font=f2)
    px2 = img.load()
    for y in range(len(rows)):
        for x in range(W):
            c = 255 if rows[y][x] == 0 else 0
            for dy in range(2):
                for dx in range(2):
                    px2[x * 2 + dx, y * 2 + 40 + dy] = c
    img.save(out)
    print('saved', os.path.basename(out))

if __name__ == '__main__':
    jobs = [
        ('/tmp/fz/simsun.ttf',   '矢量宋体'),
        ('/tmp/fz/simkai.ttf',   '矢量楷体'),
    ]
    for path, name in jobs:
        for cell in (16, 20, 24, 28):
            render(path, cell, SAMPLE, os.path.join(OUT, f'vec_{name}_{cell}.png'))
