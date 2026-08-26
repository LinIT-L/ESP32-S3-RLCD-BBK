# -*- coding: utf-8 -*-
"""生成阅读器 fnt 字库: 16/24 = HZK16F/HZK24F 经典仿宋点阵; 20/28 = 朱雀仿宋原生渲染.
格式: BK16FNT1, 非压缩 (flags=0). 字形 1bpp 行存储, 位=1 黑, 行序 MSB-first."""
import struct, os, sys
from PIL import Image, ImageDraw, ImageFont

HZK = 'tools/font_preview/hzk'
ZHUQUE = '/tmp/fz/fzfs.ttf'   # 方正仿宋简体 (朱雀仿宋是繁体风格, 已弃用)
OUT = 'components/book_reader/assets'
MAGIC = b'BK16FNT1'

def gb2312_positions():
    out = []
    for qu in range(94):
        for wei in range(94):
            b = bytes([0xA1 + qu, 0xA1 + wei])
            try:
                cp = ord(b.decode('gb2312'))
                out.append((qu, wei, cp))
            except UnicodeDecodeError:
                pass
    return out

def hzk_rows(hi, lo, cell, path):
    bpc = cell * (cell // 8)
    off = ((hi - 0xA1) * 94 + (lo - 0xA1)) * bpc
    raw = open(path, 'rb').read()[off:off + bpc]
    if len(raw) < bpc:
        return None
    return [list(raw[y * (cell // 8):(y + 1) * (cell // 8)]) for y in range(cell)]

def pack_rows(rows, cell):
    wb = (cell + 7) // 8
    out = bytearray()
    for y in range(cell):
        byte = 0
        for x in range(cell):
            if rows[y][x]:
                byte |= 1 << (7 - (x % 8))
            if x % 8 == 7:
                out.append(byte); byte = 0
        if cell % 8:
            out.append(byte)
    assert len(out) == cell * wb, (len(out), cell * wb)
    return bytes(out)

# 矢量渲染缓存: (cell, ch) -> rows
_vec_cache = {}
def vec_rows(ch, cell):
    key = (cell, ch)
    if key in _vec_cache:
        return _vec_cache[key]
    f = ImageFont.truetype(ZHUQUE, cell + 2)
    img = Image.new('L', (cell, cell), 255)
    d = ImageDraw.Draw(img)
    d.text((-1, -2), ch, font=f, fill=0)
    t = img.point(lambda v: 255 if v > 127 else 0)
    rows = [[1 if t.getpixel((x, y)) == 0 else 0 for x in range(cell)] for y in range(cell)]
    if sum(sum(r) for r in rows) == 0:
        rows = None
    _vec_cache[key] = rows
    return rows

def ascii_rows(ch, cell):
    aw, ah = cell // 2, cell
    f = ImageFont.truetype(ZHUQUE, cell)
    img = Image.new('L', (cell, cell), 255)
    d = ImageDraw.Draw(img)
    d.text((0, -1), ch, font=f, fill=0)
    t = img.point(lambda v: 255 if v > 127 else 0)
    rows = []
    for y in range(ah):
        row = []
        for x in range(aw):
            # 2:1 水平抽稀
            row.append(1 if t.getpixel((x * 2, y)) == 0 else 0)
        rows.append(row)
    return rows

def gen(cell, hzk_src, out_path):
    positions = gb2312_positions()
    gb_off = [0xFFFF] * (94 * 94)
    glyphs = []
    uni = []  # (cp, idx)
    for qu, wei, cp in positions:
        hi, lo = 0xA1 + qu, 0xA1 + wei
        if hzk_src:
            raw = hzk_rows(hi, lo, cell, hzk_src)
            if raw is None:
                continue
            rows = [[(raw[y][x // 8] >> (7 - (x % 8))) & 1 for x in range(cell)] for y in range(cell)]
        else:
            rows = vec_rows(chr(cp), cell)
            if rows is None:
                continue
        if not any(any(r) for r in rows):
            continue
        idx = len(glyphs)
        gb_off[qu * 94 + wei] = idx
        glyphs.append(pack_rows(rows, cell))
        uni.append((cp, idx))
    uni.sort()
    gb_count = len(glyphs)

    # ASCII
    ascii_w, ascii_h = cell // 2, cell
    ascii_bpc = ascii_h * ((ascii_w + 7) // 8)
    ascii_blob = bytearray()
    for c in range(0x20, 0x7F):
        rows = ascii_rows(chr(c), cell)
        ascii_blob += pack_rows(rows, ascii_w)  # pack_rows 按 cell 宽打包, 需按 ascii_w
    # 修正: pack_rows 用 cell 宽 — 改用 ascii_w 打包
    ascii_blob = bytearray()
    for c in range(0x20, 0x7F):
        rows = ascii_rows(chr(c), cell)
        wb = (ascii_w + 7) // 8
        blob = bytearray()
        for y in range(ascii_h):
            byte = 0
            for x in range(ascii_w):
                if rows[y][x]:
                    byte |= 1 << (7 - (x % 8))
                if x % 8 == 7:
                    blob.append(byte); byte = 0
            if ascii_w % 8:
                blob.append(byte)
        assert len(blob) == ascii_h * wb
        ascii_blob += blob

    gb_bpc = cell * ((cell + 7) // 8)
    hdr = struct.pack('<8sHHHHHHIII', MAGIC, cell, cell, ascii_w, ascii_h,
                      95, gb_count, gb_bpc, ascii_bpc, 0)
    gb_off_blob = struct.pack('<%dH' % len(gb_off), *gb_off)
    glyph_blob = b''.join(glyphs)
    uni_blob = b''.join(struct.pack('<HH', cp, idx) for cp, idx in uni)
    data = hdr + bytes(ascii_blob) + gb_off_blob + glyph_blob + uni_blob
    open(out_path, 'wb').write(data)
    print(f'{out_path}: cell={cell} gb={gb_count} ascii={ascii_w}x{ascii_h} bytes={len(data)}')

if __name__ == '__main__':
    # 全部原生国标简体点阵 (宋体): 16/24/32. HZK16F(仿宋16)经实测为繁体/异常, 弃用.
    gen(16, os.path.join(HZK, 'HZK16S'), os.path.join(OUT, 'book16.fnt'))
    gen(24, os.path.join(HZK, 'HZK24S'), os.path.join(OUT, 'book24.fnt'))
    gen(32, os.path.join(HZK, 'HZK32'),  os.path.join(OUT, 'book32.fnt'))
