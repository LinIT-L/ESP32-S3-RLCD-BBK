#!/usr/bin/env python3
"""使用 macOS SF Symbols 生成图标

调用 swift render_sf_symbols.swift 渲染 PNG, 然后转为 1-bit 位图

输出:
  - components/menu/icons_data.inc         (XMB 9 个 96x96)
  - components/menu/status_icons_data.inc  (状态栏 8 个 24x24)
"""
import os
import subprocess
import sys
from PIL import Image

# SF Symbols 名称
# 参考: https://developer.apple.com/sf-symbols/
SYMBOLS = {
    # === 96x96 主菜单图标 ===
    # 以下 6 个图标由用户提供桌面图片切出, 已放入 icons_preview/ 作为 96x96 PNG,
    # 跳过 SF Symbols 渲染, 直接由 png_to_1bit() 读取.
    'icon_game':  ('__custom_png__',           'regular',  96),  # 电子词典 (Aa)
    'icon_gb':    ('__custom_png__',           'regular',  96),  # Game Boy
    'icon_pad':   ('__custom_png__',           'regular',  96),  # 手柄
    'icon_sd':    ('__custom_png__',           'regular',  96),  # TF 卡
    'icon_info':  ('__custom_png__',           'regular',  96),  # 设置 (齿轮)
    'icon_play':  ('__custom_png__',           'regular',  96),  # MP3 (音符)

    'icon_key':   ('keyboard',                 'regular',  96),  # 按键设置
    'icon_bt':    ('personalhotspot',          'regular',  96),  # 蓝牙
    'icon_vol':   ('speaker.wave.2',           'regular',  96),  # 音量

    # === 状态栏电池自定义 (24x24, 4 档电量) - 不通过 SF Symbols ===
    'icon_bat_0': ('__custom_battery_0__',     'regular', 24),
    'icon_bat_1': ('__custom_battery_1__',     'regular', 24),
    'icon_bat_2': ('__custom_battery_2__',     'regular', 24),
    'icon_bat_3': ('__custom_battery_3__',     'regular', 24),

    # 状态栏蓝牙/手柄: 用主菜单 icon_bt / icon_pad 缩小到 24x24 (无需单独图标)
}

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
preview_dir = os.path.join(SCRIPT_DIR, '..', 'icons_preview')
os.makedirs(preview_dir, exist_ok=True)

# 写入 sf_symbols.txt 给 swift 脚本读取 (跳过 custom 自定义图标)
sf_txt = os.path.join(preview_dir, 'sf_symbols.txt')
with open(sf_txt, 'w') as f:
    for name, (sym, weight, size) in SYMBOLS.items():
        if sym.startswith('__custom_'):
            continue  # 自定义图标, 跳过 SF Symbols 渲染
        f.write(f"{name} {sym} {weight} {size}\n")

# 调用 swift 脚本
print("渲染 SF Symbols via Swift...")
result = subprocess.run(
    ['swift', os.path.join(SCRIPT_DIR, 'render_sf_symbols.swift'), preview_dir],
    capture_output=True, text=True
)
print(result.stdout)
if result.returncode != 0:
    print("STDERR:", result.stderr, file=sys.stderr)
    sys.exit(1)

# === 96x96 主菜单: PNG -> 1-bit 位图 ===
SZ = 96
def png_to_1bit(name):
    """读取 PNG 转 1-bit 位图 (1=黑, 0=白)"""
    path = os.path.join(preview_dir, f'{name}.png')
    img = Image.open(path).convert('L')  # 灰度
    img = img.resize((SZ, SZ), Image.LANCZOS)
    # 阈值: < 128 = 黑 (1), >= 128 = 白 (0)
    data = []
    for y in range(SZ):
        for x_byte in range((SZ + 7) // 8):
            b = 0
            for bit in range(8):
                x = x_byte * 8 + bit
                if x < SZ:
                    pixel = img.getpixel((x, y))
                    if pixel < 128:
                        b |= (1 << (7 - bit))
            data.append(b)
    return data

def png_to_1bit_flattened(name, target_h=72):
    """读取 PNG 压扁后转 1-bit 位图 (96x96 中, 内容占中间 96x72)"""
    path = os.path.join(preview_dir, f'{name}.png')
    img = Image.open(path).convert('L')
    # 先缩放到 96x96
    img = img.resize((SZ, SZ), Image.LANCZOS)
    # 然后压扁: 把高度 96 缩到 target_h, 再居中放回 96x96 (上下留白)
    img_flat = img.resize((SZ, target_h), Image.LANCZOS)
    canvas = Image.new('L', (SZ, SZ), 255)  # 白底
    y_offset = (SZ - target_h) // 2
    canvas.paste(img_flat, (0, y_offset))
    # 转 1-bit
    data = []
    for y in range(SZ):
        for x_byte in range((SZ + 7) // 8):
            b = 0
            for bit in range(8):
                x = x_byte * 8 + bit
                if x < SZ:
                    pixel = canvas.getpixel((x, y))
                    if pixel < 128:
                        b |= (1 << (7 - bit))
            data.append(b)
    return data

# === Game Boy 掌机自定义 ===
def draw_gameboy_png():
    """绘制 Game Boy 风格掌机:
       - 上半: 屏幕 (圆角矩形带边框 + 中间一条横线代表像素内容)
       - 下半左侧: 十字方向键
       - 下半右侧: A B 圆形按键
    """
    from PIL import ImageDraw
    img = Image.new('L', (SZ, SZ), 255)  # 白底
    d = ImageDraw.Draw(img)
    SW = 2
    # 外壳 (圆角矩形) - 从 y=12 到 y=84
    d.rounded_rectangle((10, 12, SZ-10, 84), radius=6, outline=0, width=SW, fill=255)
    # 屏幕 - y=20 到 y=52, 留出上方菜单条
    d.rounded_rectangle((18, 24, SZ-18, 52), radius=2, outline=0, width=SW, fill=255)
    # 屏幕内像素内容 (3 条横线)
    d.line((24, 32, SZ-30, 32), fill=0, width=1)
    d.line((24, 38, SZ-26, 38), fill=0, width=1)
    d.line((24, 44, SZ-34, 44), fill=0, width=1)
    # 屏幕下方标签点 (POWER LED 风格)
    d.ellipse((SZ-30, 55, SZ-26, 59), outline=0, width=1)
    # 十字方向键 (左下)
    cx, cy = 28, 70
    d.rectangle((cx-2, cy-9, cx+2, cy+9), outline=0, width=1, fill=255)  # 竖
    d.rectangle((cx-9, cy-2, cx+9, cy+2), outline=0, width=1, fill=255)  # 横
    # A B 按键 (右下, 倾斜 45 度两个圆)
    d.ellipse((SZ-40, 64, SZ-30, 74), outline=0, width=1)  # B
    d.ellipse((SZ-28, 56, SZ-18, 66), outline=0, width=1)  # A
    return img

# === 文曲星简化版: 复古翻盖学习机 (96x96 比例, 简洁描边风) ===
def draw_wenquxing_png():
    """绘制文曲星 NV5000 风格 - 简化版:
       - 上半: 屏幕 (圆角矩形 + 简单内容)
       - 中间: 铰链
       - 下半: 键盘 (3x6 简化)
    """
    from PIL import ImageDraw
    img = Image.new('L', (SZ, SZ), 255)  # 白底
    d = ImageDraw.Draw(img)
    H = SZ // 2
    SW = 2  # 描边宽度
    # 上半: 屏幕外壳 (圆角矩形) - 从 y=10 到 y=48
    d.rounded_rectangle((10, 10, SZ-10, 48), radius=4, outline=0, width=SW, fill=255)
    # 屏幕内: 一条横线 (代表菜单栏) + 1 行内容
    d.line((16, 18, SZ-16, 18), fill=0, width=1)
    d.line((16, 28, SZ-22, 28), fill=0, width=1)
    d.line((16, 36, SZ-26, 36), fill=0, width=1)
    d.line((16, 42, SZ-30, 42), fill=0, width=1)
    # 铰链
    d.rectangle((30, 48, SZ-30, 52), fill=0)
    # 下半: 键盘底座 (圆角矩形) - 从 y=52 到 y=86
    d.rounded_rectangle((8, 52, SZ-8, 86), radius=3, outline=0, width=SW, fill=255)
    # 键盘: 3 行 x 8 列简化方块
    for row in range(3):
        for col in range(8):
            x = 14 + col * 9
            y = 58 + row * 8
            d.rounded_rectangle((x, y, x+7, y+6), radius=1, outline=0, width=1, fill=255)
    return img

icons = {}
# 用户自定义 6 个主菜单图标 (由桌面图片切出, 已放入 icons_preview/)
for name in ['icon_game', 'icon_gb', 'icon_pad', 'icon_sd', 'icon_info', 'icon_play']:
    icons[name] = png_to_1bit(name)

# 蓝牙仍压扁 (SF Symbols 比较"胖", 需要压扁保持与菜单风格一致)
icons['icon_bt'] = png_to_1bit_flattened('icon_bt', target_h=72)

# 其他正常 SF Symbols 图标
for name in ['icon_key', 'icon_vol']:
    icons[name] = png_to_1bit(name)

# 输出 96x96 icons_data.inc
out = os.path.join(SCRIPT_DIR, '..', 'components', 'menu', 'icons_data.inc')
order = ['icon_game', 'icon_key', 'icon_bt', 'icon_pad', 'icon_vol',
         'icon_sd', 'icon_info', 'icon_play', 'icon_gb']
with open(out, 'w') as f:
    f.write(f"/* SF Symbols 图标 {SZ}x{SZ}, {len(order)} 个 (Apple 官方) */\n")
    f.write(f"#define XMB_ICON_W {SZ}\n#define XMB_ICON_H {SZ}\n")
    f.write(f"#define XMB_ICON_BYTES (({SZ}*{SZ})/8)\n#define XMB_ICON_COUNT {len(order)}\n\n")
    for n in order:
        f.write(f"const uint8_t {n}[XMB_ICON_BYTES] = {{\n")
        for i, b in enumerate(icons[n]):
            f.write(f"0x{b:02X},")
            if (i+1) % 16 == 0: f.write("\n")
        f.write("\n};\n\n")
    f.write("const uint8_t *xmb_icons[XMB_ICON_COUNT] = {\n")
    for n in order: f.write(f"    {n},\n")
    f.write("};\n")
print(f"Generated {out}: {len(order)} icons")

# === 24x24 状态栏: PNG -> 1-bit 位图 ===
SZ2 = 24
def png24_to_1bit(name):
    path = os.path.join(preview_dir, f'{name}.png')
    img = Image.open(path).convert('L')
    img = img.resize((SZ2, SZ2), Image.LANCZOS)
    data = []
    for y in range(SZ2):
        for x_byte in range((SZ2 + 7) // 8):
            b = 0
            for bit in range(8):
                x = x_byte * 8 + bit
                if x < SZ2:
                    pixel = img.getpixel((x, y))
                    if pixel < 128:
                        b |= (1 << (7 - bit))
            data.append(b)
    return data

# === 自定义加宽电池图标 (24x24, 加正极帽, 加宽外壳) ===
def draw_battery_png(level):
    """level: 0=空, 1=1/4, 2=1/2, 3=满 (4=全填充示意)
    外壳: 20x12, 正极帽 2x6, 加宽版"""
    from PIL import ImageDraw
    img = Image.new('L', (SZ2, SZ2), 255)
    d = ImageDraw.Draw(img)
    # 外壳: 圆角矩形 (1, 6) - (21, 18) = 20x12
    d.rounded_rectangle((1, 6, 21, 18), radius=2, outline=0, width=1, fill=255)
    # 正极帽: (21, 9) - (23, 15) = 2x6
    d.rectangle((21, 9, 23, 15), fill=0)
    # 内部填充格 (按 level) - 3 段
    if level >= 1:
        d.rectangle((3, 8, 8, 16), fill=0)   # 1/4
    if level >= 2:
        d.rectangle((9, 8, 14, 16), fill=0)  # 1/2
    if level >= 3:
        d.rectangle((15, 8, 19, 16), fill=0) # 3/4
    return img

# 渲染电池 (跳过 swift 渲染, 用 PIL 自画)
for level in range(4):
    name = f'icon_bat_{level}'
    bat_img = draw_battery_png(level)
    bat_img.save(os.path.join(preview_dir, f'{name}.png'))

si = {}
for n in ['icon_bt_off', 'icon_bt_on', 'icon_pad_off', 'icon_pad_on']:
    si[n] = png24_to_1bit(n)
for level in range(4):
    si[f'icon_bat_{level}'] = png24_to_1bit(f'icon_bat_{level}')

# 输出 24x24 status_icons_data.inc (只含电池 4 档, 蓝牙/手柄用主菜单图标)
out2 = os.path.join(SCRIPT_DIR, '..', 'components', 'menu', 'status_icons_data.inc')
order2 = ['icon_bat_0', 'icon_bat_1', 'icon_bat_2', 'icon_bat_3']
with open(out2, 'w') as f:
    f.write(f"/* 自定义电池图标 {SZ2}x{SZ2}, {len(order2)} 个 (加正极帽加宽版) */\n")
    f.write(f"#define STATUS_ICON_W {SZ2}\n#define STATUS_ICON_H {SZ2}\n")
    f.write(f"#define STATUS_ICON_BYTES (({SZ2}*{SZ2})/8)\n#define STATUS_ICON_COUNT {len(order2)}\n\n")
    for n in order2:
        f.write(f"const uint8_t {n}[STATUS_ICON_BYTES] = {{\n")
        for i, b in enumerate(si[n]):
            f.write(f"0x{b:02X},")
            if (i+1) % 12 == 0: f.write("\n")
        f.write("\n};\n\n")
    f.write("const uint8_t *status_icons[STATUS_ICON_COUNT] = {\n")
    for n in order2: f.write(f"    {n},\n")
    f.write("};\n")
print(f"Generated {out2}: {len(order2)} battery icons")
