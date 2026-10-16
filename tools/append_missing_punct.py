#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""追加缺失标点到现有字库 bin (不重排已有字符, 只追加尾部, 保持原有字形不变).

缺失标点: " " ' ' · ・
在书/UI 长文本里常用, 但 generate_font.py 的全角标点串漏了这几个 → font_zh/font_zh16/font_zh_light 都缺,
导致电子书正文里出现方块.

方案: 逐字渲染对应字形, 追加到 [字符表][字形] 区尾部, 更新 count. 原字符索引不变,
font_*_find_utf8 对已有字符照常命中, 新标点也能查到.

用法: python3 tools/append_missing_punct.py
"""
import struct
from PIL import Image, ImageDraw, ImageFont

MISSING = "“”‘’・"   # 仅补 3 字节 UTF-8 标点 (· U+00B7 是 2 字节, font_zh_find_utf8 3字节槽无法表示, 跳过)

# (bin 路径, 字体路径, ttc索引(0=None), 格子)
JOBS = [
    ("components/ui/fonts/font_zh.bin",      "/System/Library/Fonts/Hiragino Sans GB.ttc", 2, 24, 22),  # 粗 W6
    ("components/ui/fonts/font_zh_light.bin","/System/Library/Fonts/Hiragino Sans GB.ttc", 0, 24, 22),  # 细 W3
    ("components/ui/fonts/font_zh16.bin",    "tools/fonts/wqy/wqy_16px.ttf",               0, 16, 16),  # 文泉驿
]


def load_font(path, index, size):
    if index is not None:
        return ImageFont.truetype(path, size, index=index)
    return ImageFont.truetype(path, size)


def render_char(char, cell, fsize, font):
    img = Image.new("1", (cell, cell), 0)
    draw = ImageDraw.Draw(img)
    draw.text((0, 0), char, font=font, fill=1)
    ink = img.getbbox()
    out = Image.new("1", (cell, cell), 0)
    if ink and ink[2] > ink[0] and ink[3] > ink[1]:
        iw, ih = ink[2] - ink[0], ink[3] - ink[1]
        if iw <= cell and ih <= cell:
            dx = (cell - iw) // 2 - ink[0]
            dy = (cell - ih) // 2 - ink[1]
            out.paste(img, (dx, dy))
    else:
        out.paste(img, (0, 0))
    pixels = list(out.getdata())
    res = bytearray()
    for row in range(cell):
        for cb in range(cell // 8):
            b = 0
            for bit in range(8):
                x = cb * 8 + bit
                if pixels[row * cell + x]:
                    b |= (1 << (7 - bit))
            res.append(b)
    return bytes(res)


def main():
    for path, fontp, idx, cell, fsize in JOBS:
        data = open(path, "rb").read()
        magic = data[:8]
        count = struct.unpack("<I", data[8:12])[0]
        bpc = cell * (cell // 8)
        chars_end = 12 + count * 3
        char_tbl = bytearray(data[12:chars_end])
        glyph_blob = bytearray(data[chars_end:])

        font = load_font(fontp, idx, fsize)
        added = 0
        for ch in MISSING:
            u = ch.encode("utf-8")
            if any(char_tbl[i:i + 3] == u for i in range(0, len(char_tbl), 3)):
                continue
            glyph = render_char(ch, cell, fsize, font)
            char_tbl += u
            glyph_blob += glyph
            added += 1
            print(f"  {path}: + {ch} (U+{ord(ch):04X})")

        if added:
            new_count = count + added
            out = magic + struct.pack("<I", new_count) + bytes(char_tbl) + bytes(glyph_blob)
            open(path, "wb").write(out)
            print(f"  已写回 {path}: count {count} -> {new_count}")
        else:
            print(f"  {path}: 标点已存在, 无追加")


if __name__ == "__main__":
    main()