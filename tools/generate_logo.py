#!/usr/bin/env python3.11
import os
from PIL import Image, ImageDraw, ImageFont

LOGO_W = 400
LOGO_H = 300

img = Image.new('1', (LOGO_W, LOGO_H), 0)  # 0 = white
draw = ImageDraw.Draw(img)

try:
    font_large = ImageFont.truetype("/System/Library/Fonts/PingFang.ttc", 48)
    font_small = ImageFont.truetype("/System/Library/Fonts/PingFang.ttc", 24)
except:
    try:
        font_large = ImageFont.truetype("/System/Library/Fonts/Hiragino Sans GB.ttc", 48)
        font_small = ImageFont.truetype("/System/Library/Fonts/Hiragino Sans GB.ttc", 24)
    except:
        font_large = ImageFont.load_default()
        font_small = ImageFont.load_default()

text1 = "步步高"
text2 = "游戏模拟器"

bbox1 = draw.textbbox((0, 0), text1, font=font_large)
w1 = bbox1[2] - bbox1[0]
h1 = bbox1[3] - bbox1[1]
draw.text(((LOGO_W - w1) / 2, 80), text1, font=font_large, fill=1)

bbox2 = draw.textbbox((0, 0), text2, font=font_small)
w2 = bbox2[2] - bbox2[0]
h2 = bbox2[3] - bbox2[1]
draw.text(((LOGO_W - w2) / 2, 160), text2, font=font_small, fill=1)

import math
pixels = list(img.getdata())
bytes_per_row = (LOGO_W + 7) // 8
data = bytearray(bytes_per_row * LOGO_H)

for y in range(LOGO_H):
    for x in range(LOGO_W):
        if pixels[y * LOGO_W + x]:
            byte_idx = y * bytes_per_row + (x // 8)
            bit = 7 - (x % 8)
            data[byte_idx] |= (1 << bit)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
out_path = os.path.join(SCRIPT_DIR, '..', 'components', 'st7305', 'bbk_logo_data.inc')
with open(out_path, 'w') as f:
    for i, byte in enumerate(data):
        if i % 16 == 0:
            f.write('\n')
        f.write(f'0x{byte:02X}, ')

print(f"Generated {len(data)} bytes, text1 size: {w1}x{h1}, text2 size: {w2}x{h2}")
