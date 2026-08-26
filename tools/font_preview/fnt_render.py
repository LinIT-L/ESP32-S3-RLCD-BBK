# -*- coding: utf-8 -*-
"""解码 book*.fnt 内嵌字库, 还原设备实际 1bpp 渲染效果 (供字体选择预览)"""
import struct, sys

def load_fnt(path):
    d = open(path, 'rb').read()
    magic = d[:8]
    cell_w, cell_h, ascii_w, ascii_h, ascii_count, gb_count, gb_bpc, ascii_bpc, flags = \
        struct.unpack_from('<HHHHHHIII', d, 8)
    off = 32
    ascii_tab = d[off:off + ascii_count * ascii_bpc]; off += len(ascii_tab)
    gb_off_tab = d[off:off + 94*94*2]; off += len(gb_off_tab)
    rle = flags & 1
    if rle:
        glyph_off = struct.unpack_from('<%dI' % (gb_count+1), d, off); off += (gb_count+1)*4
        glyphs = d[off:off + (glyph_off[-1]-glyph_off[0]) if False else (len(d) - (gb_count)*4 - off)]
        glyphs = d[off:len(d)-gb_count*4]
        uni_tab = d[len(d)-gb_count*4:]
    else:
        glyphs = d[off:off + gb_count*gb_bpc]; off += len(glyphs)
        uni_tab = d[off:]
    return dict(cell_w=cell_w, cell_h=cell_h, ascii_w=ascii_w, ascii_h=ascii_h,
                ascii_count=ascii_count, gb_off_tab=gb_off_tab, glyphs=glyphs,
                glyph_off=glyph_off if rle else None, rle=rle, gb_bpc=gb_bpc,
                uni_tab=uni_tab, gb_count=gb_count)

def rle_decode(src):
    out = bytearray()
    i = 0
    while i < len(src):
        v = src[i]; i += 1
        if v < 128:
            out += b'\x00' * (v + 1)
        else:
            c = v - 127
            out += src[i:i+c]; i += c
    return bytes(out)

def gb_glyph(f, hi, lo, cell):
    if not (0xA1 <= hi <= 0xF7 and 0xA1 <= lo <= 0xFE):
        return None
    idx = struct.unpack_from('<H', f['gb_off_tab'], ((hi-0xA1)*94 + (lo-0xA1))*2)[0]
    if idx == 0xFFFF:
        return None
    if f['rle']:
        a, b = f['glyph_off'][idx], f['glyph_off'][idx+1]
        raw = rle_decode(f['glyphs'][a:b])
        assert len(raw) >= cell*cell//8, (cell, len(raw))
        return raw[:cell*cell//8]
    return f['glyphs'][idx*f['gb_bpc']:(idx+1)*f['gb_bpc']]

def glyph_bitmap(f, hi, lo, cell):
    raw = gb_glyph(f, hi, lo, cell)
    if raw is None: return None
    # 字形按行存储, 每行 ceil(cell/8) 字节 (与 st7305_blit_1bit 一致)
    wb = (cell + 7) // 8
    rows = []
    for y in range(cell):
        bits = []
        base = y * wb
        for x in range(cell):
            byte = raw[base + x // 8]
            bits.append((byte >> (7 - x % 8)) & 1)
        rows.append(bits)
    return rows

def render_text(f, text, cell, wpx, margin=8, line_h=None, gap=0, justify=True, H=None):
    """按设备布局渲染 1bpp 页: 返回 rows (list of list of 0/1)"""
    line_h = line_h or (cell + 4)
    W = wpx
    H = H or (300 if W == 400 else 400)
    cols = W - 2*margin
    # 每行最大字数
    maxch = cols // cell
    lines = []
    cur = []
    cur_w = 0
    for ch in text:
        if ch == '\n':
            lines.append(cur); cur = []; cur_w = 0; continue
        if ch == ' ':
            cw = cell//2
        else:
            cw = cell + gap
        if cur_w + cw > cols and cur:
            lines.append(cur); cur = []; cur_w = 0
        cur.append(ch); cur_w += cw
    if cur: lines.append(cur)
    rows = [[0]*W for _ in range(min(line_h*len(lines), H))]
    for li, line in enumerate(lines):
        if margin + li*line_h >= H: break
        used = sum(cell + gap if c != ' ' else cell//2 for c in line)
        extra = cols - used
        gaps = len(line)-1
        per = rem = 0
        if justify and gaps > 0 and extra > 0 and len(line) > 1 and used*3 >= cols*2:
            per, rem = divmod(extra, gaps)
        x = margin
        for ci, ch in enumerate(line):
            y0 = margin + li*line_h
            if ch == ' ':
                x += cell//2
                continue
            hi, lo = ch.encode('gb2312')
            bm = glyph_bitmap(f, hi, lo, cell)
            if bm is None:
                x += cell + gap; continue
            for yy in range(cell):
                for xx in range(cell):
                    if bm[yy][xx] and y0+yy < len(rows) and x+xx < W:
                        rows[y0+yy][x+xx] = 1
            x += cell + gap
            if justify and gaps > 0 and ci < gaps:
                x += per + (1 if ci < rem else 0)
    return rows

def rows_to_png(rows, path, scale=2, invert=True):
    from PIL import Image
    H, W = len(rows), len(rows[0])
    img = Image.new('L', (W*scale, H*scale), 255)
    px = img.load()
    for y in range(H):
        for x in range(W):
            v = rows[y][x]
            c = 255 if v == 0 else 0   # 字=黑
            for dy in range(scale):
                for dx in range(scale):
                    px[x*scale+dx, y*scale+dy] = c
    img.save(path)

if __name__ == '__main__':
    import os
    SAMPLE = ("他抬起头，看了一眼窗外的雨，又低下头继续写着。\n"
              "“这个字念什么？”她指着书页上的一个字问。\n"
              "我说：“念‘絮’，柳絮的絮。”她若有所思地点点头。\n"
              "夜里，风很大，院子里的梧桐树被吹得哗哗响。\n"
              "“那……我们明天还去河边吗？”她小声地问。\n"
              "我没有回答，只是默默地把那本书收进了怀里。\n"
              "有些话，说出口就收不回了。")
    base = 'components/book_reader/assets'
    for sz in (20, 24, 28, 32):
        f = load_fnt(f'{base}/book{sz}.fnt')
        rows = render_text(f, SAMPLE, sz, 400)
        rows_to_png(rows, f'tools/font_preview/out/current_{sz}.png', scale=2)
    print('current font previews done')
