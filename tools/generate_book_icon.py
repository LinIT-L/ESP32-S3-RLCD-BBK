#!/usr/bin/env python3
"""生成电子书主菜单图标 (96x96 打开的书本), 追加到 icons_data.inc 末尾"""

import os
import re
from PIL import Image, ImageDraw

SZ = 96
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(SCRIPT_DIR, "..", "components", "menu", "icons_data.inc")

img = Image.new("L", (SZ, SZ), 255)
d = ImageDraw.Draw(img)
SW = 2

# 书脊 + 左右页
d.rectangle((46, 16, 50, 86), fill=0)
d.rounded_rectangle((12, 14, 48, 86), radius=5, outline=0, width=SW, fill=255)
d.rounded_rectangle((50, 14, 84, 86), radius=5, outline=0, width=SW, fill=255)
# 左页文字行
for yy in (26, 34, 46, 54, 66, 74):
    d.line((19, yy, 41, yy), fill=0, width=2)
# 右页文字行
for yy in (26, 34, 46, 54, 66, 74):
    d.line((57, yy, 77, yy), fill=0, width=2)
# 底部书页厚度
d.rectangle((40, 86, 56, 90), fill=0)

# 转 1bpp (MSB 左, 位=1 黑)
data = []
for y in range(SZ):
    for xb in range(SZ // 8):
        b = 0
        for bit in range(8):
            x = xb * 8 + bit
            if img.getpixel((x, y)) < 128:
                b |= 1 << (7 - bit)
        data.append(b)

def icon_book_block():
    lines = ["\nconst uint8_t icon_book[XMB_ICON_BYTES] = {\n"]
    for i, b in enumerate(data):
        lines.append(f"0x{b:02X},")
        if (i + 1) % 16 == 0:
            lines.append("\n")
    lines.append("\n};\n")
    return "".join(lines)

# 更新计数和索引数组
with open(OUT, "r") as f:
    text = f.read()
text = re.sub(r'const uint8_t icon_book\[XMB_ICON_BYTES\] = \{(?:[^}]*)\};\n', '', text)
text = text.replace("#define XMB_ICON_COUNT 9", "#define XMB_ICON_COUNT 10")
old_arr = (
    "const uint8_t *xmb_icons[XMB_ICON_COUNT] = {\n"
    "    icon_game,\n    icon_key,\n    icon_bt,\n    icon_pad,\n    icon_vol,\n"
    "    icon_sd,\n    icon_info,\n    icon_play,\n    icon_gb,\n"
    "};"
)
new_arr = (
    "const uint8_t *xmb_icons[XMB_ICON_COUNT] = {\n"
    "    icon_game,\n    icon_key,\n    icon_bt,\n    icon_pad,\n    icon_vol,\n"
    "    icon_sd,\n    icon_info,\n    icon_play,\n    icon_gb,\n    icon_book,\n"
    "};"
)
if old_arr not in text:
    print("警告: xmb_icons 数组格式不匹配, 请手动检查")
else:
    text = text.replace(old_arr, new_arr)
    anchor = "const uint8_t *xmb_icons[XMB_ICON_COUNT] = {"
    idx = text.find(anchor)
    if idx >= 0:
        text = text[:idx] + icon_book_block() + "\n" + text[idx:]
with open(OUT, "w") as f:
    f.write(text)
print("XMB_ICON_COUNT -> 10, xmb_icons 已加入 icon_book (索引 9)")
