#!/usr/bin/env python3
"""生成电子书多档 GB2312 点阵字库 (从 macOS 苹果字体渲染)

输入: macOS 系统字体 (Hiragino Sans GB 优先 = 苹果黑体; 可传参指定其它字体)
输出: components/book_reader/assets/book{size}.fnt (黑体, 默认)
      components/book_reader/assets/song{size}.fnt (宋体, 传 Songti 路径 + 后缀 song)
      SD 字体: python3 generate_book_font.py <任意字体> <名称> 20 → <名称>20.fnt
      (只生成指定单字号, 放进 /sdcard/fonts/ 即可用)
      (二进制: 32B 头 + ASCII + GB2312 偏移表 + GB2312 字形 + Unicode 索引表)
      V1.0.64: 字形 RLE 游程压缩 (flags bit0=1), 体积约为原 50~60%.

渲染: 3 倍超采样 + LANCZOS 缩小 + 阈值二值化, 小字号下笔画更稳更圆润.
字库烧进 flash 通过 XIP 只读引用, 不占内部 RAM / PSRAM.
"""

import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_PATH = os.path.join(SCRIPT_DIR, "..", "components", "book_reader", "assets", "book16.fnt")

SS = 1                           # 渲染倍数: 1 = 原生尺寸 (FreeType 提示), 保留笔画
                                  # 之前 3x 超采样+LANCZOS 缩小会把细笔画整条丢失
FAMILY_FONT = None               # 外部指定字体路径 (argv[1])
FAMILY_TAG = ""                  # 输出家族名 (argv[2]: "song" = 宋体; 空 = 黑体 book)

def generate_size(cell):
    """按指定汉字网格尺寸生成字库文件"""
    global CELL_W, CELL_H, ASCII_W, ASCII_H, OUT_PATH
    CELL_W, CELL_H = cell, cell
    ASCII_W, ASCII_H = cell // 2, cell
    OUT_PATH = os.path.join(SCRIPT_DIR, "..", "components", "book_reader",
                            "assets", f"{FAMILY_TAG or 'book'}{cell}.fnt")
    main()

# macOS 系统字体候选: 黑体优先 STHeiti Medium (笔画饱满, 与菜单同款),
# 其次 Hiragino Sans GB (较细, 24px 下观感接近宋体, 已降级)
FONT_CANDIDATES = [
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/Library/Fonts/Songti.ttc",
]


def pick_font():
    if FAMILY_FONT and os.path.exists(FAMILY_FONT):
        try:
            f = ImageFont.truetype(FAMILY_FONT, CELL_H * SS)
            print(f"使用字体: {FAMILY_FONT}")
            return f, FAMILY_FONT
        except Exception:
            pass
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            try:
                f = ImageFont.truetype(p, CELL_H * SS)  # 3x 超采样
                print(f"使用字体: {p}")
                return f, p
            except Exception:
                continue
    print("警告: 未找到中文字体, 使用默认字体 (可能全是方块)")
    return ImageFont.load_default(), "default"


def render_cjk(font, ch):
    """在目标尺寸画布原生渲染字符 (FreeType 提示, 不缩放), 返回位图 (MSB 左)"""
    img = Image.new("L", (CELL_W, CELL_H), 255)
    d = ImageDraw.Draw(img)
    try:
        d.text((0, 0), ch, font=font, fill=0)
    except Exception:
        return None
    return img_to_1bit_rows(img, CELL_W, CELL_H)


def render_ascii(font, ch):
    """原生尺寸渲染西文字符 (顶对齐)"""
    img = Image.new("L", (ASCII_W, ASCII_H), 255)
    d = ImageDraw.Draw(img)
    try:
        d.text((0, 0), ch, font=font, fill=0)
    except Exception:
        return None
    return img_to_1bit_rows(img, ASCII_W, ASCII_H)


def img_to_1bit_rows(img, w, h):
    """灰度图 -> 1bpp 行存储位图 (MSB 在左, 位=1 为字, 阈值 150 保笔画)"""
    out = bytearray()
    for y in range(h):
        for x_byte in range((w + 7) // 8):
            b = 0
            for bit in range(8):
                x = x_byte * 8 + bit
                if x < w and img.getpixel((x, y)) < 96:
                    b |= 1 << (7 - bit)
            out.append(b)
    return bytes(out)


def rle_encode(data):
    """1bpp 位图 RLE 编码: 0..127 = 1..128 个 0 字节; 128..255 = (v-127) 个紧跟的字面字节"""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        if data[i] == 0:
            j = i
            while j < n and data[j] == 0 and (j - i) < 128:
                j += 1
            out.append((j - i) - 1)
            i = j
        else:
            j = i
            while j < n and (j - i) < 128:
                j += 1
            c = j - i
            out.append(c + 127)
            out += data[i:j]
            i = j
    return bytes(out)


def collect_gb2312():
    """遍历 GB2312 全码区 (0xA1A1..0xF7FE), 返回 [(hi, lo, char, unicode)]"""
    chars = []
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            chars.append((hi, lo, ch, ord(ch)))
    return chars


def read_old_unicode_extras():
    """从旧字库读取 Unicode 表, 返回全部码点 (用于保留既有覆盖)"""
    try:
        with open(OUT_PATH, "rb") as f:
            data = f.read()
        if len(data) < 32 or data[:8] != b"BK16FNT1":
            return []
        (cw, ch_, aw, ah, ac, gbc, gbb, abb, flags) = struct.unpack_from(
            "<HHHHHHIII", data, 8)
        hdr = 32
        ascii_end = hdr + ac * abb
        gb_off_end = ascii_end + 94 * 94 * 2
        uni_off = gb_off_end + gbc * gbb
        extras = []
        i = uni_off
        while i + 4 <= len(data):
            cp, idx = struct.unpack_from("<HH", data, i)
            extras.append(cp)
            i += 4
        return extras
    except Exception:
        return []


def is_invisible_cp(cp):
    """不可见/格式控制码点: 不进字库 (避免渲染出空心方块字形)"""
    if cp in (0x0020, 0x00A0, 0x1680, 0x202F, 0x205F, 0x3000, 0xFEFF,
              0xFFFD, 0xFFFE, 0xFFFF, 0x00AD, 0x180E, 0x200B,
              0x2028, 0x2029):
        return True
    if 0x2000 <= cp <= 0x200F: return True
    if 0x202A <= cp <= 0x202E: return True
    if 0x2060 <= cp <= 0x206F: return True
    if 0xFE00 <= cp <= 0xFE0F: return True
    return False


def main():
    font = None
    font, font_path = pick_font()
    old_cps = [cp for cp in read_old_unicode_extras() if not is_invisible_cp(cp)]
    print(f"旧字库 Unicode 码点: {len(old_cps)}")

    # 网络小说常见但不在 GB2312 的字符 (空格/破折号/符号等)
    extra_cps = [
        0x3000, 0x00A0, 0x2003, 0x2009,                       # 各类可见空格
        0x2014, 0x2015,                                           # — ―
        0x00B7, 0x2022, 0x2027,                                   # · • ‧
        0x301C, 0xFF5E,                                           # 〜 ～
        0x2460, 0x2461, 0x2462, 0x2463, 0x2464, 0x2465, 0x2466, 0x2467, 0x2468, 0x2469,  # ①-⑩
        0x2474, 0x2475, 0x2476, 0x2477, 0x2478, 0x2479, 0x247A, 0x247B, 0x247C, 0x247D,  # ⑴-⑽
        0x25A0, 0x25A1, 0x25C6, 0x25CB, 0x2605, 0x2606, 0x2609, 0x2611, 0x2713, 0x2717,  # ■□◆○★☆☉☑✓✗
        0x266A, 0x266B, 0x266C,                                   # ♪♫♬
        0x2190, 0x2191, 0x2192, 0x2193,                           # ←↑→↓
        0x2103, 0x2109,                                           # ℃℉
        0x2116, 0x00A9, 0x00AE, 0x2122,                           # №©®™
        0x00D7, 0x00F7, 0x00B1, 0x00B0,                           # ×÷±°
        0x30FB,                                                   # ・
    ]
    # 常见网络小说/出版书里的生僻字 (GB2312 之外, 用户实际阅读书籍中缺字)
    rare_cps = [
        0x5ADA, 0x5C43, 0x9ED2, 0x8D51, 0x69C3, 0x7AB8, 0x7AA3, 0x8DF6,
        0x5B1B, 0x69D1, 0x5570, 0x877A, 0x8C68, 0x6E1F, 0x83AC, 0x70DC,
        0x52E0, 0x8E5A, 0x56E7, 0x77AD, 0x54B5, 0x87C5, 0x81DC, 0x7606,
        0x5ECB, 0x9F41, 0x5147, 0x66BC, 0x55D0, 0x5C28,
    ]

    # ---- ASCII 字形 ----
    ascii_glyphs = []
    for code in range(0x20, 0x7F):
        g = render_ascii(font, chr(code))
        ascii_glyphs.append(g if g else bytes(((ASCII_W + 7) // 8) * ASCII_H))

    # ---- GB2312 字形 ----
    gb_chars = collect_gb2312()
    covered = {cp for _, _, _, cp in gb_chars}
    gb_glyphs = []       # index -> 32B 位图
    gb_offset = [0xFFFF] * (94 * 94)
    unicode_map = []     # (unicode, glyph_index)
    missing = []
    blank_glyph = bytes(CELL_W * ((CELL_W + 7) // 8))
    for hi, lo, ch, cp in gb_chars:
        g = render_cjk(font, ch)
        if g is None:
            missing.append(ch)
            continue
        idx = len(gb_glyphs)
        gb_glyphs.append(g if g != blank_glyph else blank_glyph)
        gb_offset[(hi - 0xA1) * 94 + (lo - 0xA1)] = idx
        unicode_map.append((cp, idx))

    print(f"GB2312 有效字符: {len(gb_chars)}, 渲染成功: {len(gb_glyphs)}, 渲染失败: {len(missing)}")
    if missing:
        print("  空白字符示例:", "".join(missing[:20]))

    # ---- 额外 Unicode 覆盖 (旧字库已有码点 + 常用符号) ----
    extra_added = 0
    for cp in list(old_cps) + extra_cps + rare_cps:
        if cp in covered or cp >= 0x10000:
            continue
        try:
            ch = chr(cp)
        except Exception:
            continue
        g = render_cjk(font, ch)
        if g is None:
            continue
        idx = len(gb_glyphs)
        gb_glyphs.append(g if g != blank_glyph else blank_glyph)
        unicode_map.append((cp, idx))
        covered.add(cp)
        extra_added += 1
    print(f"额外 Unicode 字符: {extra_added}")
    unicode_map.sort(key=lambda t: t[0])

    # ---- 打包 (V1.0.64: 字形 RLE 压缩, flags bit0=1) ----
    compressed_blob = b""
    glyph_off = [0]
    for g in gb_glyphs:
        e = rle_encode(g)
        compressed_blob += e
        glyph_off.append(len(compressed_blob))
    header = struct.pack(
        "<8sHHHHHHIII",
        b"BK16FNT1",
        CELL_W, CELL_H,
        ASCII_W, ASCII_H,
        len(ascii_glyphs),
        len(gb_glyphs),
        ((CELL_W + 7) // 8) * CELL_H,
        ((ASCII_W + 7) // 8) * ASCII_H,
        1,   # flags bit0 = 字形 RLE 压缩
    )
    ascii_blob = b"".join(ascii_glyphs)
    off_blob = struct.pack("<" + "H" * len(gb_offset), *gb_offset)
    off32_blob = struct.pack("<" + "I" * len(glyph_off), *glyph_off)
    gb_blob = compressed_blob
    uni_blob = b"".join(struct.pack("<HH", cp, idx) for cp, idx in unicode_map)

    data = header + ascii_blob + off_blob + off32_blob + gb_blob + uni_blob
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "wb") as f:
        f.write(data)

    print(f"输出: {OUT_PATH}")
    print(f"总大小: {len(data)} B ({len(data)/1024:.1f} KB) (RLE: 字形 {len(gb_blob)} B, 原始 {len(gb_glyphs)*((CELL_W+7)//8)*CELL_H} B)")
    print(f"  ASCII: {len(ascii_blob)} B, 偏移表: {len(off_blob)} B, "
          f"GB 字形: {len(gb_blob)} B, Unicode 索引: {len(uni_blob)} B")

    # ---- 预览图 (人工 QA) ----
    preview = Image.new("L", (CELL_W * 12, CELL_H * 8), 255)
    pd = ImageDraw.Draw(preview)
    sample = "步步高电子书敲击翻页测试ABC123，。！？——《红楼梦》第 一 回"
    px = 0
    for i, ch in enumerate(sample[:60]):
        g = None
        if 0x20 <= ord(ch) < 0x7F:
            g = render_ascii(font, ch)
            w = ASCII_W
        else:
            for hi, lo, c, cp in gb_chars:
                if c == ch:
                    g = render_cjk(font, ch)
                    break
            w = CELL_W
        if g:
            # 直接按位还原
            row_bytes = (w + 7) // 8
            img = Image.new("L", (w, CELL_H), 255)
            for y in range(CELL_H):
                row = g[y * row_bytes: (y + 1) * row_bytes]
                for xb, byte in enumerate(row):
                    for bit in range(8):
                        x = xb * 8 + bit
                        if x < w and (byte >> (7 - bit)) & 1:
                            img.putpixel((x, y), 0)
            preview.paste(img, (px, 0))
            px += w
    prev_path = os.path.join(SCRIPT_DIR, "preview_book_font.png")
    preview.save(prev_path)
    print(f"预览: {prev_path}  (字体: {os.path.basename(font_path)})")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        FAMILY_FONT = sys.argv[1]
    if len(sys.argv) > 2:
        FAMILY_TAG = sys.argv[2]
    sizes = (20, 24, 28, 32)   # V1.0.64: 用户要求四档 20/24/28/32
    if len(sys.argv) > 3:
        sizes = (int(sys.argv[3]),)   # 只生成单字号 (SD 字体瘦身)
    for cell in sizes:
        generate_size(cell)
    print(f"全部字库生成完毕 ({FAMILY_TAG or 'hei'})")
