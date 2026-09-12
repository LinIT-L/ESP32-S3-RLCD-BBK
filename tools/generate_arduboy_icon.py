#!/usr/bin/env python3
"""生成 Arduboy 掌机图标 (96x96, 1bpp, 2px线条)
形状参考: 圆角矩形掌机, 中间屏幕, 左侧方向键, 右侧A/B按钮
"""
import math

W = H = 96

def pixels_to_bytes(pixels):
    bytes_per_row = (W + 7) // 8
    data = []
    for y in range(H):
        for b in range(bytes_per_row):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                if x < W and pixels[y][x] == 1:
                    byte |= (1 << (7 - bit))
            data.append(byte)
    return data

def print_c_array(data, name):
    print(f"const uint8_t {name}[XMB_ICON_BYTES] = {{")
    for i in range(0, len(data), 16):
        line = data[i:i + 16]
        hex_str = ",".join(f"0x{b:02X}" for b in line)
        print(f"    {hex_str},")
    print("};")
    print()

def draw_hline(pixels, x0, x1, y, thickness=2):
    for t in range(thickness):
        yy = y + t - thickness // 2
        if yy < 0 or yy >= H:
            continue
        for x in range(min(x0, x1), max(x0, x1) + 1):
            if 0 <= x < W:
                pixels[yy][x] = 1

def draw_vline(pixels, y0, y1, x, thickness=2):
    for t in range(thickness):
        xx = x + t - thickness // 2
        if xx < 0 or xx >= W:
            continue
        for y in range(min(y0, y1), max(y0, y1) + 1):
            if 0 <= y < H:
                pixels[y][xx] = 1

def draw_rounded_rect(pixels, x0, y0, x1, y1, r, thickness=2):
    """圆角矩形, 2px线条"""
    if x0 > x1: x0, x1 = x1, x0
    if y0 > y1: y0, y1 = y1, y0

    # 四边直线
    draw_hline(pixels, x0 + r, x1 - r, y0, thickness)
    draw_hline(pixels, x0 + r, x1 - r, y1, thickness)
    draw_vline(pixels, y0 + r, y1 - r, x0, thickness)
    draw_vline(pixels, y0 + r, y1 - r, x1, thickness)

    # 四个圆角 (Bresenham style)
    for angle in range(0, 91):
        rad = math.radians(angle)
        for t in range(thickness):
            rt = r - t
            if rt <= 0:
                continue
            cx = int(rt * math.cos(rad))
            cy = int(rt * math.sin(rad))
            # 四个象限
            for qx, qy in [(1, 1), (-1, 1), (1, -1), (-1, -1)]:
                px = (x1 if qx > 0 else x0) + qx * cx
                py = (y1 if qy > 0 else y0) + qy * cy
                if 0 <= px < W and 0 <= py < H:
                    pixels[py][px] = 1

def draw_circle(pixels, cx, cy, r, thickness=2):
    """圆形, 2px线条"""
    for angle in range(0, 360):
        rad = math.radians(angle)
        for t in range(thickness):
            rt = r - t
            if rt <= 0:
                continue
            x = int(cx + rt * math.cos(rad))
            y = int(cy + rt * math.sin(rad))
            if 0 <= x < W and 0 <= y < H:
                pixels[y][x] = 1

def draw_fill_rect(pixels, x0, y0, x1, y1):
    """实心填充"""
    if x0 > x1: x0, x1 = x1, x0
    if y0 > y1: y0, y1 = y1, y0
    for y in range(y0, y1 + 1):
        if y < 0 or y >= H:
            continue
        for x in range(x0, x1 + 1):
            if 0 <= x < W:
                pixels[y][x] = 1

def generate_arduboy_icon():
    pixels = [[0] * W for _ in range(H)]

    # 掌机主体: 横向圆角矩形
    # 位置: 居中偏上, 宽80, 高44
    body_x0 = 8
    body_y0 = 26
    body_x1 = 87
    body_y1 = 69
    body_r = 10
    draw_rounded_rect(pixels, body_x0, body_y0, body_x1, body_y1, body_r, 2)

    # 屏幕: 居中圆角矩形, 黑色填充边框
    # Arduboy 屏幕 128x64 (横向), 掌机上是横向的
    screen_x0 = 22
    screen_y0 = 33
    screen_x1 = 73
    screen_y1 = 62
    screen_r = 3
    draw_rounded_rect(pixels, screen_x0, screen_y0, screen_x1, screen_y1, screen_r, 2)
    # 屏幕内部再加一圈细线模拟屏幕边框
    draw_rounded_rect(pixels, screen_x0 + 3, screen_y0 + 3, screen_x1 - 3, screen_y1 - 3, 2, 2)

    # 左侧: 方向键 (D-pad) - 十字形
    # 位置: 屏幕左侧
    dpad_cx = 15
    dpad_cy = 48
    dpad_arm_len = 6
    dpad_arm_w = 4
    # 横臂
    draw_fill_rect(pixels, dpad_cx - dpad_arm_len, dpad_cy - dpad_arm_w // 2,
                   dpad_cx + dpad_arm_len, dpad_cy + dpad_arm_w // 2)
    # 竖臂
    draw_fill_rect(pixels, dpad_cx - dpad_arm_w // 2, dpad_cy - dpad_arm_len,
                   dpad_cx + dpad_arm_w // 2, dpad_cy + dpad_arm_len)

    # 右侧: A/B 按钮 (两个圆形)
    # A键 - 右上
    btn_a_cx = 80
    btn_a_cy = 42
    draw_circle(pixels, btn_a_cx, btn_a_cy, 4, 2)
    # B键 - 左下
    btn_b_cx = 74
    btn_b_cy = 54
    draw_circle(pixels, btn_b_cx, btn_b_cy, 4, 2)

    # 底部: 扬声器孔 (右侧三个横条)
    speaker_x = 78
    speaker_y_start = 62
    speaker_len = 6
    for i in range(3):
        yy = speaker_y_start + i * 3
        draw_hline(pixels, speaker_x, speaker_x + speaker_len, yy, 1)

    # 左侧底部: 两个小按钮 (开始/选择, 椭圆形)
    # 小按钮简化为短横线
    draw_hline(pixels, 12, 18, 60, 1)
    draw_hline(pixels, 12, 18, 64, 1)

    return pixels


if __name__ == '__main__':
    pixels = generate_arduboy_icon()
    data = pixels_to_bytes(pixels)
    print_c_array(data, 'icon_arduboy')
