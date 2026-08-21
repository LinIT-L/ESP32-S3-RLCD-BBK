# -*- coding: utf-8 -*-
"""低像素字体规格预览: 设备屏幕 400x300 ×2 = 800x600, 每个设备点=2x2(4)真实像素.
对 12/14/16/20/24/28/32 七种规格, 用同一句话各排一行, 逐个像素栅格化(1点=1设备像素)."""
import os
from PIL import Image, ImageDraw, ImageFont

BASE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(BASE, "preview_800x600.png")

# 各规格用哪个字体文件 (12/14/16 文泉驿真点阵; 20~32 霞鹜文楷渲染)
FONTS = {
    12: os.path.join(BASE, "wqy", "wqy_12px.ttf"),
    14: os.path.join(BASE, "wqy", "wqy_14px.ttf"),
    16: os.path.join(BASE, "wqy", "wqy_16px.ttf"),
    20: os.path.join(BASE, "lxgw", "LXGWWenKaiGBLite-Regular.ttf"),
    24: os.path.join(BASE, "lxgw", "LXGWWenKaiGBLite-Regular.ttf"),
    28: os.path.join(BASE, "lxgw", "LXGWWenKaiGBLite-Regular.ttf"),
    32: os.path.join(BASE, "lxgw", "LXGWWenKaiGBLite-Regular.ttf"),
}
SIZES = [12, 14, 16, 20, 24, 28, 32]

# 同一行字 (笔画丰富度覆盖简/繁)
TEXT = "我能吞下玻璃而不伤身体"

DEV_W, DEV_H = 400, 300          # 设备逻辑分辨率
SCALE = 2                        # ×2 -> 800x600
BLACK = 0
WHITE = 255

def cell_bitmap(ch, cell, ttf):
    """把单个汉字栅格化为 cell*cell 的 1 位位图(list of rows, 白底黑字)."""
    f = ImageFont.truetype(ttf, cell)
    img = Image.new("L", (cell, cell), WHITE)
    d = ImageDraw.Draw(img)
    # 水平居中, 垂直微调让字芯大致居中
    bbox = d.textbbox((0, 0), ch, font=f)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    dx = (cell - tw) / 2 - bbox[0]
    dy = (cell - th) / 2 - 1 - bbox[1]
    d.text((dx, dy), ch, font=f, fill=BLACK)
    return [[1 if img.getpixel((x, y)) == BLACK else 0 for x in range(cell)] for y in range(cell)]

def paste_bits(img, rows, px, py, cell):
    """按 1 像素=1 设备点 把位图(黑点)粘到 img."""
    for y in range(cell):
        for x in range(cell):
            if rows[y][x]:
                img.putpixel((px + x, py + y), BLACK)

# 统一画布 (白底), 直接画
canvas = Image.new("L", (DEV_W, DEV_H), WHITE)
d = ImageDraw.Draw(canvas)

title_f = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 16)
d.text((6, 2), "低像素字体 12-32px (同一句: 我能吞下玻璃而不伤身体) 400x300 x2", font=title_f, fill=BLACK)

cur_y = 24
label_f = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 13)
for cell in SIZES:
    ttf = FONTS[cell]
    # label（普通字体标注）
    d.text((6, cur_y + (cell - 13) / 2 + 1), f"{cell}px", font=label_f, fill=BLACK)
    # 同一行字形
    x = 8 + 36
    for ch in TEXT:
        rows = cell_bitmap(ch, cell, ttf)
        paste_bits(canvas, rows, x, cur_y, cell)
        x += cell
    cur_y += cell + 3

# 放大 2x 最近邻 -> 800x600 (每设备点 = 2x2 = 4 真实像素)
big = canvas.resize((DEV_W * SCALE, DEV_H * SCALE), Image.NEAREST)
big.save(OUT)
print("saved", OUT, big.size)