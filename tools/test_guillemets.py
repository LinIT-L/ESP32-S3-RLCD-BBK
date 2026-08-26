#!/usr/bin/env python3
"""模拟新的像素绘制《》符号，看看实际效果"""

glyph_h = 24
top = 2
bot = top + glyph_h - 1
mid = top + glyph_h // 2

# 测试文字
test_text = "测试文字"
char_w = 24  # 中文字符宽度
tw = len(test_text) * char_w
tx = 100  # 文字起始 x

# 创建画布
width = 300
height = 40
canvas = [[' ' for _ in range(width)] for _ in range(height)]

# 先绘制文字区域（用占位符）
for i in range(tw):
    for j in range(glyph_h):
        if i % char_w == 0 or i % char_w == char_w - 1 or j == 0 or j == glyph_h - 1:
            canvas[top + j][tx + i] = '.'

# 左《
gx = tx - 20
for layer in range(2):
    ox = gx + layer * 5
    for i in range(glyph_h // 2):
        px = ox + 8 - i
        if px < 0 or px >= width:
            continue
        for d in range(2):
            y1 = top + i + d
            y2 = bot - i + d
            if 0 <= y1 < height:
                canvas[y1][px] = '#'
            if 0 <= y2 < height:
                canvas[y2][px] = '#'

# 右》
gx_right = tx + tw + 6
for layer in range(2):
    ox = gx_right + layer * 5
    for i in range(glyph_h // 2):
        px = ox + i
        if px < 0 or px >= width:
            continue
        for d in range(2):
            y1 = top + i + d
            y2 = bot - i + d
            if 0 <= y1 < height:
                canvas[y1][px] = '#'
            if 0 <= y2 < height:
                canvas[y2][px] = '#'

# 打印结果
print("选中项效果 (《 测试文字 》):")
print()
for row in canvas:
    print(''.join(row))
print()

# 放大显示左《
print("左《 放大:")
for y in range(top, bot + 1):
    line = ''
    for x in range(tx - 25, tx):
        if 0 <= x < width and 0 <= y < height:
            c = canvas[y][x]
            line += c * 2  # 水平放大2倍
        else:
            line += '  '
    print(line)
print()

print("右》 放大:")
for y in range(top, bot + 1):
    line = ''
    for x in range(tx + tw, tx + tw + 25):
        if 0 <= x < width and 0 <= y < height:
            c = canvas[y][x]
            line += c * 2
        else:
            line += '  '
    print(line)
