#!/usr/bin/env python3
import math
import subprocess
import os

W = H = 96
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def blank():
    return [[0] * W for _ in range(H)]


def set_pixel(px, x, y, v=1):
    if 0 <= x < W and 0 <= y < H:
        px[y][x] = v


def fill_rect(px, x0, y0, x1, y1, v=1):
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            px[y][x] = v


def fill_circle(px, cx, cy, r, v=1):
    for y in range(int(cy - r), int(cy + r) + 1):
        for x in range(int(cx - r), int(cx + r) + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                set_pixel(px, x, y, v)


def stroke_circle(px, cx, cy, r, thickness=2, v=1):
    for t in range(thickness):
        rr = r - t
        if rr <= 0:
            break
        for angle_deg in range(0, 360):
            angle = math.radians(angle_deg)
            x = int(cx + rr * math.cos(angle))
            y = int(cy + rr * math.sin(angle))
            set_pixel(px, x, y, v)


def fill_rounded_rect(px, x0, y0, x1, y1, r, v=1):
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            in_corner = False
            for (ccx, ccy) in [(x0 + r, y0 + r), (x1 - r, y0 + r),
                               (x0 + r, y1 - r), (x1 - r, y1 - r)]:
                if ((x <= x0 + r or x >= x1 - r) and
                        (y <= y0 + r or y >= y1 - r)):
                    if (x - ccx) ** 2 + (y - ccy) ** 2 <= r * r:
                        px[y][x] = v
                        in_corner = True
                    break
            if not in_corner:
                if x0 + r <= x <= x1 - r or y0 + r <= y <= y1 - r:
                    px[y][x] = v


def fill_rounded_rect2(px, x0, y0, x1, y1, r, v=1):
    """标准圆角矩形填充 (四角用圆心圆判据, 圆弧平滑)."""
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            if x0 + r <= x <= x1 - r or y0 + r <= y <= y1 - r:
                set_pixel(px, x, y, v)
                continue
            for (ccx, ccy) in [(x0 + r, y0 + r), (x1 - r, y0 + r),
                               (x0 + r, y1 - r), (x1 - r, y1 - r)]:
                if (x - ccx) ** 2 + (y - ccy) ** 2 <= r * r:
                    set_pixel(px, x, y, v)
                    break


def stroke_rounded_rect_smooth(px, x0, y0, x1, y1, r, t, v=1):
    """平滑圆角矩形描边: 实心填充后挖空内部, 圆弧自然顺滑, 线宽 t."""
    fill_rounded_rect2(px, x0, y0, x1, y1, r, v)
    fill_rounded_rect2(px, x0 + t, y0 + t, x1 - t, y1 - t, max(0, r - t), 0)


def stroke_circle_smooth(px, cx, cy, r, t, v=1):
    """平滑圆环: 实心圆挖空内圆."""
    fill_circle(px, cx, cy, r, v)
    fill_circle(px, cx, cy, r - t, 0)


def stroke_rounded_rect(px, x0, y0, x1, y1, r, thickness=2, v=1):
    for t in range(thickness):
        xr0 = x0 + t
        yr0 = y0 + t
        xr1 = x1 - t
        yr1 = y1 - t
        rr = r - t
        if rr < 0:
            rr = 0
        for y in range(max(0, yr0), min(H, yr1 + 1)):
            for x in range(max(0, xr0), min(W, xr1 + 1)):
                on_edge = False
                if xr0 + rr <= x <= xr1 - rr:
                    if y == yr0 or y == yr1:
                        on_edge = True
                elif yr0 + rr <= y <= yr1 - rr:
                    if x == xr0 or x == xr1:
                        on_edge = True
                else:
                    for (ccx, ccy) in [(xr0 + rr, yr0 + rr), (xr1 - rr, yr0 + rr),
                                       (xr0 + rr, yr1 - rr), (xr1 - rr, yr1 - rr)]:
                        if ((x <= xr0 + rr or x >= xr1 - rr) and
                                (y <= yr0 + rr or y >= yr1 - rr)):
                            d2 = (x - ccx) ** 2 + (y - ccy) ** 2
                            if rr * rr - 1 <= d2 <= rr * rr + 1:
                                on_edge = True
                                break
                if on_edge:
                    px[y][x] = v


def stroke_line(px, x0, y0, x1, y1, thickness=2, v=1):
    dx = abs(x1 - x0)
    dy = abs(y1 - y0)
    steps = max(dx, dy)
    if steps == 0:
        fill_circle(px, x0, y0, thickness // 2, v)
        return
    for i in range(steps + 1):
        t = i / steps
        x = int(x0 + (x1 - x0) * t)
        y = int(y0 + (y1 - y0) * t)
        fill_circle(px, x, y, thickness // 2, v)


def stroke_outline_2px(px):
    dirs4 = [(1, 0), (-1, 0), (0, 1), (0, -1)]
    result = [row[:] for row in px]
    for _ in range(2):
        nxt = [row[:] for row in result]
        for y in range(H):
            for x in range(W):
                if result[y][x] == 0:
                    for dx, dy in dirs4:
                        nx, ny = x + dx, y + dy
                        if 0 <= nx < W and 0 <= ny < H and result[ny][nx] == 1:
                            nxt[y][x] = 1
                            break
        result = nxt
    return result


# ============================================================
# Game console icons - drawn from reference images
# ============================================================

def gen_icon_game():
    """电子词典 - 步步高电子词典，翻盖式，参考第一张图片。"""
    px = blank()
    # 主体：圆角矩形，横向略宽
    stroke_rounded_rect(px, 10, 14, 85, 82, 10, thickness=2)
    # 顶部铰链
    fill_rounded_rect(px, 22, 8, 73, 16, 4)
    fill_rect(px, 26, 10, 69, 14, 0)
    # 屏幕区域
    stroke_rounded_rect(px, 22, 22, 73, 54, 6, thickness=2)
    # 屏幕内纹理 - 几何线条（参考图片的装饰线条）
    # 左上角小logo区域
    fill_rect(px, 26, 28, 38, 30)
    fill_rect(px, 26, 32, 42, 33)
    # 装饰几何线条（右侧方块图案）
    stroke_line(px, 50, 26, 70, 26, thickness=2)
    stroke_line(px, 62, 26, 62, 40, thickness=2)
    stroke_line(px, 50, 40, 70, 40, thickness=2)
    stroke_line(px, 50, 26, 50, 40, thickness=2)
    # 下方文字区域 - 步步高
    for i, x in enumerate(range(38, 68, 6)):
        fill_rect(px, x, 46, x + 4, 48)
    # 键盘区域
    # 方向键（上）
    fill_rect(px, 30, 60, 40, 62)
    fill_rect(px, 32, 64, 38, 70)
    fill_rect(px, 26, 66, 44, 68)
    fill_rect(px, 30, 72, 40, 74)
    # 右侧功能键
    for i in range(3):
        cx = 58 + i * 8
        stroke_circle(px, cx, 64, 3, thickness=2)
    for i in range(2):
        cx = 62 + i * 8
        stroke_circle(px, cx, 72, 3, thickness=2)
    # 底部装饰条
    fill_rect(px, 20, 78, 26, 80)
    fill_rect(px, 30, 80, 34, 82)
    return px


def gen_icon_pad():
    """手柄 - 北通白色游戏手柄，参考第二张图片。"""
    px = blank()
    # 手柄主体轮廓（左右握把 + 中间连接）
    # 左侧握把
    stroke_rounded_rect(px, 8, 30, 32, 86, 12, thickness=2)
    # 右侧握把
    stroke_rounded_rect(px, 63, 30, 87, 86, 12, thickness=2)
    # 中间上沿（肩部曲线）
    stroke_line(px, 20, 30, 76, 30, thickness=2)
    # 顶部肩键
    fill_rounded_rect(px, 22, 18, 40, 28, 5)
    fill_rounded_rect(px, 55, 18, 73, 28, 5)
    fill_rect(px, 26, 22, 36, 24, 0)
    fill_rect(px, 59, 22, 69, 24, 0)
    # 中间面板
    stroke_rounded_rect(px, 30, 32, 65, 58, 8, thickness=2)
    # 中间logo（北通标志 - 简化为几何图案）
    # 中心图案
    fill_rect(px, 42, 38, 54, 40)
    fill_rect(px, 44, 36, 52, 42)
    fill_rect(px, 46, 34, 50, 44)
    # 左右按钮组
    # SELECT / START 风格按钮
    fill_rounded_rect(px, 32, 48, 40, 52, 2)
    fill_rounded_rect(px, 44, 46, 52, 50, 2)
    fill_rounded_rect(px, 56, 48, 64, 52, 2)
    # 左摇杆
    stroke_circle(px, 26, 48, 8, thickness=2)
    stroke_circle(px, 26, 48, 4, thickness=2)
    # 右摇杆
    stroke_circle(px, 69, 56, 8, thickness=2)
    stroke_circle(px, 69, 56, 4, thickness=2)
    # 左侧方向键（十字键）
    cx, cy = 20, 66
    fill_rounded_rect(px, cx - 2, cy - 8, cx + 2, cy + 8, 1)
    fill_rounded_rect(px, cx - 8, cy - 2, cx + 8, cy + 2, 1)
    # 右侧动作键（Y/X/A/B）
    # Y (上)
    stroke_circle(px, 72, 64, 4, thickness=2)
    # X (左)
    stroke_circle(px, 64, 70, 4, thickness=2)
    # A (下)
    stroke_circle(px, 72, 76, 4, thickness=2)
    # B (右)
    stroke_circle(px, 80, 70, 4, thickness=2)
    # 握把纹理（底部斜线）
    for i in range(5):
        y = 76 + i * 2
        stroke_line(px, 10, y, 24, y + 4, thickness=1)
        stroke_line(px, 72, y + 4, 86, y, thickness=1)
    return px


def gen_icon_keyboard():
    """键鼠 - 小键盘: 圆角键盘体 + 4 排键 (上3排 5 列 + 底排空格). 极简实心."""
    px = blank()
    # 键盘体: 圆角矩形描边
    stroke_rounded_rect(px, 14, 30, 82, 80, 6, thickness=3)
    # 顶部一行如上排按键
    rows_y = [38, 50, 62, 74]
    for ri, y0 in enumerate(rows_y):
        if ri == 3:
            # 底排: 宽空格键
            fill_rounded_rect2(px, 20, y0, 76, y0 + 6, 2)
            break
        for c in range(5):
            x0 = 20 + c * 12
            fill_rounded_rect2(px, x0, y0, x0 + 8, y0 + 8, 2)
    return px


def gen_icon_arduboy():
    """Arduboy: 圆角板身 + 大圆键 + 四小键 + 屏幕 + 扬声器. 极简."""
    px = blank()
    # 板身: 圆角矩形 (平滑 3px 描边)
    stroke_rounded_rect_smooth(px, 10, 16, 85, 80, 14, 3)
    # 屏幕: 中央圆角屏
    stroke_rounded_rect_smooth(px, 28, 26, 66, 54, 5, 3)
    fill_rect(px, 33, 45, 61, 47, 0)   # 屏内一条画面线
    # 大圆键: 左侧 (实心圆盘 + 白心, 按键感)
    fill_circle(px, 19, 42, 8)
    fill_circle(px, 19, 42, 3, 0)
    # 四个小键: 右侧菱形排列 (实心圆点)
    for (bx, by) in [(74, 33), (68, 42), (80, 42), (74, 51)]:
        fill_circle(px, bx, by, 3)
    # 扬声器: 右下三条横线
    for i in range(3):
        y = 66 + i * 4
        fill_rounded_rect2(px, 70, y, 80, y + 2, 1)
    return px


def gen_icon_gb():
    """GB - Game Boy，参考第四张黄色图片。"""
    px = blank()
    # 主体：竖长圆角矩形
    stroke_rounded_rect(px, 22, 6, 73, 89, 10, thickness=3)
    # 屏幕外框（深色区域）
    stroke_rounded_rect(px, 30, 14, 65, 50, 6, thickness=3)
    # 屏幕内显示区域（稍小）
    stroke_rounded_rect(px, 36, 20, 59, 44, 3, thickness=3)
    # 屏幕内容（Game Boy 开机画面 - 简化）
    fill_rect(px, 40, 28, 55, 30)
    fill_rect(px, 42, 32, 53, 34)
    fill_rect(px, 44, 36, 51, 38)
    # 屏幕下方文字（Nintendo 风格）
    for i in range(6):
        x = 38 + i * 5
        fill_rect(px, x, 54, x + 3, 56)
    # 十字键（左侧）
    cx, cy = 34, 66
    # 垂直
    fill_rect(px, cx - 2, cy - 9, cx + 2, cy + 9)
    # 水平
    fill_rect(px, cx - 9, cy - 2, cx + 9, cy + 2)
    # 中心凹点
    fill_circle(px, cx, cy, 2, 0)
    # A / B 按钮（右侧，斜向排列）
    # B按钮（左，稍高）
    stroke_circle(px, 56, 62, 5, thickness=3)
    fill_circle(px, 56, 62, 2)
    # A按钮（右，稍低）
    stroke_circle(px, 66, 68, 5, thickness=3)
    fill_circle(px, 66, 68, 2)
    # SELECT / START 按钮（中间下方，小椭圆）
    fill_rounded_rect(px, 38, 78, 48, 82, 2)
    fill_rect(px, 41, 80, 45, 80, 0)
    fill_rounded_rect(px, 50, 78, 60, 82, 2)
    fill_rect(px, 53, 80, 57, 80, 0)
    # 底部扬声器（小圆点阵列）
    for row in range(3):
        for col in range(5):
            x = 62 + col * 3
            y = 84 + row * 2
            set_pixel(px, x, y)
    return px


def gen_icon_gbc():
    """GBC - Game Boy Color: 横向圆角机身 + 大屏 + 十字键 + A/B. 极简顺滑."""
    px = blank()
    # 主体: 横向圆角机身 (平滑 3px 描边)
    stroke_rounded_rect_smooth(px, 9, 15, 86, 81, 16, 3)
    # 屏幕: 左侧大圆角屏 (约占机身宽 45%)
    stroke_rounded_rect_smooth(px, 20, 27, 58, 56, 7, 3)
    fill_rect(px, 25, 47, 53, 49, 0)   # 屏内一条"画面线"
    # 十字键: 左下
    cx, cy = 35, 67
    fill_rounded_rect2(px, cx - 3, cy - 9, cx + 3, cy + 9, 2)
    fill_rounded_rect2(px, cx - 9, cy - 3, cx + 9, cy + 3, 2)
    # A/B: 右下两个圆环按钮
    stroke_circle_smooth(px, 65, 62, 7, 3)
    stroke_circle_smooth(px, 76, 70, 7, 3)
    # 扬声器: 右上三短条
    for i in range(3):
        fill_rounded_rect2(px, 68 + i * 5, 21, 70 + i * 5, 24, 1)
    return px


def gen_icon_nes():
    """NES - 红白机: 宽扁机身 + 插卡槽 + 两个按钮 + 双手柄口. 极简."""
    px = blank()
    # 机身: 宽扁圆角矩形 (平滑 3px 描边)
    stroke_rounded_rect_smooth(px, 6, 38, 89, 78, 10, 3)
    # 顶部插卡槽: 深色圆角凹槽, 微微突出机身
    fill_rounded_rect2(px, 28, 26, 67, 40, 6)
    fill_rounded_rect2(px, 33, 31, 62, 37, 3, 0)
    # 电源/复位: 右侧两个实心滑块按钮 (中间留白线)
    fill_rounded_rect2(px, 72, 46, 84, 56, 4)
    fill_rect(px, 76, 50, 80, 52, 0)
    fill_rounded_rect2(px, 72, 58, 84, 68, 4)
    fill_rect(px, 76, 62, 80, 64, 0)
    # 双手柄接口: 底部左右
    stroke_rounded_rect_smooth(px, 12, 62, 31, 72, 4, 2)
    fill_rect(px, 16, 66, 27, 68, 0)
    stroke_rounded_rect_smooth(px, 64, 62, 83, 72, 4, 2)
    fill_rect(px, 68, 66, 79, 68, 0)
    return px


def gen_icon_game_settings():
    """游戏设置 - 手柄 + 齿轮。"""
    px = blank()
    # 手柄（简化版，上方）
    stroke_rounded_rect(px, 18, 12, 78, 48, 12, thickness=2)
    # 左摇杆
    stroke_circle(px, 32, 30, 5, thickness=2)
    # 右动作键
    stroke_circle(px, 66, 26, 3, thickness=2)
    stroke_circle(px, 60, 32, 3, thickness=2)
    stroke_circle(px, 72, 32, 3, thickness=2)
    stroke_circle(px, 66, 38, 3, thickness=2)
    # 中间按钮
    fill_rounded_rect(px, 44, 34, 52, 38, 2)
    # 下方齿轮
    cx, cy = 48, 72
    outer_r = 16
    inner_r = 11
    hole_r = 4
    teeth = 8
    for angle_deg in range(0, 360, 2):
        angle = math.radians(angle_deg)
        tooth_period = 360 / teeth
        angle_mod = angle_deg % tooth_period
        tooth_half = tooth_period * 0.25
        if angle_mod < tooth_half or angle_mod > tooth_period - tooth_half:
            r = outer_r
        else:
            r = inner_r
        x = cx + r * math.cos(angle)
        y = cy + r * math.sin(angle)
        set_pixel(px, int(x), int(y))
    stroke_circle(px, cx, cy, inner_r, thickness=2)
    fill_circle(px, cx, cy, hole_r, 0)
    stroke_circle(px, cx, cy, hole_r, thickness=2)
    return px


# ============================================================
# Render Apple SF Symbols via Swift
# ============================================================

def render_apple_icons():
    swift_script = os.path.join(SCRIPT_DIR, "render_sf_symbols.swift")
    try:
        result = subprocess.run(
            ["swift", swift_script],
            capture_output=True, text=True, timeout=60
        )
        if result.returncode != 0:
            print(f"Swift render failed: {result.stderr}")
            return {}
        icons = {}
        for line in result.stdout.strip().split("\n"):
            if ":" not in line:
                continue
            name, hexdata = line.split(":", 1)
            pixels = [[0] * W for _ in range(H)]
            for y in range(H):
                for x_byte in range(W // 8):
                    byte = int(hexdata[(y * W // 8 + x_byte) * 2:(y * W // 8 + x_byte) * 2 + 2], 16)
                    for bit in range(8):
                        x = x_byte * 8 + bit
                        if byte & (1 << (7 - bit)):
                            pixels[y][x] = 1
            icons[name] = pixels
        return icons
    except Exception as e:
        print(f"Swift render error: {e}")
        return {}


# ============================================================
# Output
# ============================================================

def emit_array(name, px):
    lines = []
    lines.append(f"const uint8_t {name}[XMB_ICON_BYTES] = {{")
    for y in range(H):
        byte_vals = []
        for x_byte in range(W // 8):
            byte = 0
            for bit in range(8):
                x = x_byte * 8 + bit
                if px[y][x]:
                    byte |= (1 << (7 - bit))
            byte_vals.append(f"0x{byte:02X}")
        lines.append("    " + ",".join(byte_vals) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    print("Rendering Apple SF Symbols...")
    apple_icons = render_apple_icons()
    print(f"Got {len(apple_icons)} Apple icons: {list(apple_icons.keys())}")

    # Game console icons (drawn by code, based on reference images)
    console_icons = {
        "icon_game": gen_icon_game(),
        "icon_pad": gen_icon_pad(),
        "icon_gb": gen_icon_gb(),
        "icon_gbc": gen_icon_gbc(),
        "icon_nes": gen_icon_nes(),
        "icon_arduboy": gen_icon_arduboy(),
        "icon_game_settings": gen_icon_game_settings(),
    }

    # Apply 2px outline stroke to console icons (4-directional, thin outline)
    # 主机图标 (GB/GBC/NES/Arduboy) 为纯 3px 线宽, 不再叠加描边
    for name in console_icons:
        if name in ("icon_gb", "icon_gbc", "icon_nes", "icon_arduboy"):
            continue
        console_icons[name] = stroke_outline_2px(console_icons[name])

    # Merge: order must match theme_icon_names[]
    # 0: icon_game (电子词典)
    # 1: icon_key (按键设置) - Apple
    # 2: icon_bt (蓝牙) - Apple
    # 3: icon_pad (手柄) - console (from image)
    # 4: icon_vol (音量) - Apple
    # 5: icon_sd (SD卡) - Apple
    # 6: icon_info (设置) - Apple
    # 7: icon_play (MP3) - Apple
    # 8: icon_gb (GB) - console
    # 9: icon_game_settings (游戏设置) - console
    # 10: icon_arduboy (Arduboy) - console
    all_icons = [
        ("icon_game", console_icons["icon_game"]),
        ("icon_key", apple_icons.get("icon_key", [[0]*W for _ in range(H)])),
        ("icon_bt", apple_icons.get("icon_bt", [[0]*W for _ in range(H)])),
        ("icon_pad", console_icons["icon_pad"]),
        ("icon_vol", apple_icons.get("icon_vol", [[0]*W for _ in range(H)])),
        ("icon_sd", apple_icons.get("icon_sd", [[0]*W for _ in range(H)])),
        ("icon_info", apple_icons.get("icon_info", [[0]*W for _ in range(H)])),
        ("icon_play", apple_icons.get("icon_play", [[0]*W for _ in range(H)])),
        ("icon_gb", console_icons["icon_gb"]),
        ("icon_game_settings", console_icons["icon_game_settings"]),
        ("icon_arduboy", console_icons["icon_arduboy"]),
    ]

    out = []
    out.append("/* Menu icons 96x96 1bpp (MSB first), 11 icons")
    out.append(" * Apple icons: rendered from REAL SF Symbols with 2px thin outline")
    out.append(" * Game console icons: drawn from reference images with 2px thin outline")
    out.append(" * Generated by generate_all_icons.py + render_sf_symbols.swift")
    out.append(" */")
    out.append("#define XMB_ICON_W 96")
    out.append("#define XMB_ICON_H 96")
    out.append("#define XMB_ICON_BYTES ((96*96)/8)")
    out.append("#define XMB_ICON_COUNT 11")
    out.append("")
    for name, px in all_icons:
        out.append(emit_array(name, px))
        out.append("")

    out.append("/* Icon pointer array (index matches theme_icon_names[]) */")
    out.append("const uint8_t *xmb_icons[XMB_ICON_COUNT] = {")
    for name, _ in all_icons:
        out.append(f"    {name},")
    out.append("};")
    out.append("")

    with open(os.path.join(SCRIPT_DIR, "icons_data.inc"), "w") as f:
        f.write("\n".join(out))
    print(f"Generated icons_data.inc with {len(all_icons)} icons")


if __name__ == "__main__":
    main()
