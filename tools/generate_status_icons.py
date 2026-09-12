#!/usr/bin/env python3
"""生成状态栏小图标 (24x24, 1-bit) - 蓝牙/手柄/电量
输出: components/menu/status_icons_data.inc
"""
import os
from PIL import Image, ImageDraw

ICON_SIZE = 24
HALF = ICON_SIZE // 2

def save_icon(name, img):
    data = []
    for row in range(ICON_SIZE):
        for byte_idx in range((ICON_SIZE + 7) // 8):
            b = 0
            for bit in range(8):
                col = byte_idx * 8 + bit
                if col < ICON_SIZE and img.getpixel((col, row)) == 0:
                    b |= (1 << (7 - bit))
            data.append(b)
    return data  # 仅返回 data 数组

# === 1. 蓝牙 (未连接) - 灰空心 ===
img = Image.new('1', (ICON_SIZE, ICON_SIZE), 255)
d = ImageDraw.Draw(img)
# 中心竖线
d.line((HALF, 4, HALF, ICON_SIZE-5), fill=0, width=1)
# 上三角
d.polygon([(HALF, 4), (HALF+10, 9), (HALF, HALF)], outline=0, fill=255)
# 下三角
d.polygon([(HALF, ICON_SIZE-4), (HALF+10, ICON_SIZE-9), (HALF, HALF)], outline=0, fill=255)
# 斜线 (实心箭头)
d.polygon([(HALF, 4), (HALF+10, 9), (HALF, HALF)], fill=0)
d.polygon([(HALF, ICON_SIZE-4), (HALF+10, ICON_SIZE-9), (HALF, HALF)], fill=0)
# 中间三角镂空
d.polygon([(HALF+10, 9), (HALF, HALF), (HALF+10, ICON_SIZE-9)], fill=255)
icon_bt_off = save_icon("icon_bt_off", img)
# remove tuple wrapping

# === 2. 蓝牙 (已连接) - 实心 ===
img = Image.new('1', (ICON_SIZE, ICON_SIZE), 255)
d = ImageDraw.Draw(img)
d.line((HALF, 4, HALF, ICON_SIZE-5), fill=0, width=1)
d.polygon([(HALF, 4), (HALF+10, 9), (HALF, HALF)], fill=0)
d.polygon([(HALF, ICON_SIZE-4), (HALF+10, ICON_SIZE-9), (HALF, HALF)], fill=0)
d.polygon([(HALF+10, 9), (HALF, HALF), (HALF+10, ICON_SIZE-9)], fill=255)
icon_bt_on = save_icon("icon_bt_on", img)

# === 3. 手柄 (未连接) - 简化小图 ===
img = Image.new('1', (ICON_SIZE, ICON_SIZE), 255)
d = ImageDraw.Draw(img)
# 主体
d.rounded_rectangle((2, 8, ICON_SIZE-2, 18), radius=4, fill=0)
# D-pad
d.rectangle((6, HALF-2, 12, HALF+2), fill=255)
# 右侧按键
d.ellipse((ICON_SIZE-12, HALF-2, ICON_SIZE-6, HALF+2), fill=255)
icon_pad_off = save_icon("icon_pad_off", img)

# === 4. 手柄 (已连接) - 同上实心 ===
img = Image.new('1', (ICON_SIZE, ICON_SIZE), 255)
d = ImageDraw.Draw(img)
d.rounded_rectangle((2, 8, ICON_SIZE-2, 18), radius=4, fill=0)
# D-pad
d.rectangle((5, HALF-3, 13, HALF+3), fill=0)
d.rectangle((HALF-2, 7, HALF+2, 17), fill=0)
d.rectangle((HALF-1, HALF-1, HALF+1, HALF+1), fill=255)
# 右侧按钮
d.ellipse((ICON_SIZE-13, HALF-3, ICON_SIZE-5, HALF+3), fill=0)
d.ellipse((ICON_SIZE-11, HALF-1, ICON_SIZE-7, HALF+1), fill=255)
icon_pad_on = save_icon("icon_pad_on", img)

# === 5. 电池外壳 (空) ===
img = Image.new('1', (ICON_SIZE, ICON_SIZE), 255)
d = ImageDraw.Draw(img)
d.rectangle((2, 8, 20, 18), outline=0, width=1)
d.rectangle((20, 11, 22, 15), fill=0)
icon_bat_empty = save_icon("icon_bat_empty", img)

# === 6. 电池满格 ===
img = Image.new('1', (ICON_SIZE, ICON_SIZE), 255)
d = ImageDraw.Draw(img)
d.rectangle((2, 8, 20, 18), outline=0, width=1)
d.rectangle((20, 11, 22, 15), fill=0)
d.rectangle((3, 9, 19, 17), fill=0)
# 3格条
d.rectangle((4, 10, 9, 16), fill=255)
d.rectangle((10, 10, 14, 16), fill=255)
d.rectangle((15, 10, 18, 16), fill=255)
icon_bat_full = save_icon("icon_bat_full", img)

# === 7. 电池 2格 ===
img = Image.new('1', (ICON_SIZE, ICON_SIZE), 255)
d = ImageDraw.Draw(img)
d.rectangle((2, 8, 20, 18), outline=0, width=1)
d.rectangle((20, 11, 22, 15), fill=0)
d.rectangle((3, 9, 19, 17), fill=0)
d.rectangle((4, 10, 11, 16), fill=255)
d.rectangle((12, 10, 19, 16), fill=255)
icon_bat_2 = save_icon("icon_bat_2", img)

# === 8. 电池 1格 ===
img = Image.new('1', (ICON_SIZE, ICON_SIZE), 255)
d = ImageDraw.Draw(img)
d.rectangle((2, 8, 20, 18), outline=0, width=1)
d.rectangle((20, 11, 22, 15), fill=0)
d.rectangle((3, 9, 19, 17), fill=0)
d.rectangle((4, 10, 19, 16), fill=255)
icon_bat_1 = save_icon("icon_bat_1", img)

# === 输出 ===
icons = [
    ("icon_bt_off", icon_bt_off),
    ("icon_bt_on", icon_bt_on),
    ("icon_pad_off", icon_pad_off),
    ("icon_pad_on", icon_pad_on),
    ("icon_bat_empty", icon_bat_empty),
    ("icon_bat_1", icon_bat_1),
    ("icon_bat_2", icon_bat_2),
    ("icon_bat_full", icon_bat_full),
]

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
out_path = os.path.join(SCRIPT_DIR, '..', 'components', 'menu', 'status_icons_data.inc')
with open(out_path, 'w') as f:
    f.write(f"/* 状态栏图标 {ICON_SIZE}x{ICON_SIZE}, {len(icons)} 个 */\n")
    f.write(f"#define STATUS_ICON_W {ICON_SIZE}\n")
    f.write(f"#define STATUS_ICON_H {ICON_SIZE}\n")
    f.write(f"#define STATUS_ICON_BYTES (({ICON_SIZE} * {ICON_SIZE}) / 8)\n")
    f.write(f"#define STATUS_ICON_COUNT {len(icons)}\n\n")
    for name, data in icons:
        f.write(f"const uint8_t {name}[STATUS_ICON_BYTES] = {{\n")
        for i, b in enumerate(data):
            f.write(f"0x{b:02X},")
            if (i + 1) % 12 == 0:
                f.write("\n")
        f.write("\n};\n\n")
    f.write("const uint8_t *status_icons[STATUS_ICON_COUNT] = {\n")
    for name, _ in icons:
        f.write(f"    {name},\n")
    f.write("};\n")

print(f"Generated: {out_path}")
print(f"Status icons: {len(icons)} ({ICON_SIZE}x{ICON_SIZE})")
