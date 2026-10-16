#!/usr/bin/env python3
"""预览当前 icons_data.inc 中的所有图标."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_icons_pixel import ICONS, SZ
from PIL import Image, ImageDraw

cols = 5
rows = (len(ICONS) + cols - 1) // cols
pad = 8
label_h = 14
cell = SZ + label_h + pad * 2
out = Image.new('1', (cols * cell, rows * cell), 1)
d = ImageDraw.Draw(out)
for i, (name, fn, _) in enumerate(ICONS):
    r, c = divmod(i, cols)
    icon = fn()
    x0 = c * cell + pad
    y0 = r * cell + pad
    out.paste(icon, (x0, y0))
out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icons_preview_v2.png')
out.save(out_path)
print(f"预览: {out_path}")
