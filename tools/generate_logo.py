#!/usr/bin/env python3.11
import os
from PIL import Image, ImageDraw, ImageFont

LOGO_W = 400
LOGO_H = 300

img = Image.new('1', (LOGO_W, LOGO_H), 0)  # 0 = white
draw = ImageDraw.Draw(img)

FONT_PATHS = ["/System/Library/Fonts/PingFang.ttc",
              "/System/Library/Fonts/Hiragino Sans GB.ttc"]
def load_font(size):
    for p in FONT_PATHS:
        try:
            return ImageFont.truetype(p, size)
        except Exception:
            pass
    return ImageFont.load_default()

# 主文字 "LinTOS": 动态字号使总宽 ≈ 200, 水平+垂直居中
text1 = "LinTOS"
target_w = 200
size = 40
f = load_font(size)
while True:
    b = draw.textbbox((0, 0), text1, font=f)
    if (b[2] - b[0]) >= target_w or size > 100:
        break
    size += 2
    f = load_font(size)

bbox1 = draw.textbbox((0, 0), text1, font=f)
w1 = bbox1[2] - bbox1[0]
h1 = bbox1[3] - bbox1[1]
draw.text(((LOGO_W - w1) / 2, (LOGO_H - h1) / 2), text1, font=f, fill=1)

# 右下角署名 "by: LinIT" (小字)
text2 = "by: LinIT"
f2 = load_font(24)
bbox2 = draw.textbbox((0, 0), text2, font=f2)
w2 = bbox2[2] - bbox2[0]
h2 = bbox2[3] - bbox2[1]
draw.text((LOGO_W - w2 - 8, LOGO_H - h2 - 8), text2, font=f2, fill=1)

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
    f.write('/* \xe5\xbc\x80\xe6\x9c\xba Logo \xe4\xbd\x8d\xe5\x9b\xbe 400x300 1bpp, \xe7\x94\xb1 tools/generate_logo.py \xe7\x94\x9f\xe6\x88\x90 (\xe5\x8b\xbf\xe6\x89\x8b\xe6\x94\xb9) */\n')
    f.write('static const uint8_t bbk_logo_bits[] = {')
    for i, byte in enumerate(data):
        if i % 16 == 0:
            f.write('\n')
        f.write(f'0x{byte:02X}, ')
    f.write('\n};\n')

print(f"Generated {len(data)} bytes, LinTOS size: {w1}x{h1} (font {size}), byline size: {w2}x{h2}")
