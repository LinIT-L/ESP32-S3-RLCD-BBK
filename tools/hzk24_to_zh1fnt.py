#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hzk24_to_zh1fnt.py — 把 HZK24S/H/F/K（GB2312 区位码 24×24）转成系统 24px 字库 ZH1FNT01

为什么这样做:
  · 系统 24px 字库 (font_zh.bin) 放在 appdata 分区固定槽位 (512KB)，格式
    [8B "ZH1FNT01"][u32 count][count*3 UTF-8 字符表][count*72 字形]；
  · 槽位装不下"全库"（512KB/75B ≈ 6990 字），所以**沿用手头字库的字符表**
    （顺序、字数完全不变 ⇒ 体积不变、槽位不用改、分区不用动）；
  · HZK24S 里缺的字（个别符号/异体）保留原字形，避免出现空白字。

用法:
  python3 tools/hzk24_to_zh1fnt.py <HZK24源> <现有font_zh.bin> <输出font_zh.bin>
"""
import os
import sys

MAGIC = b"ZH1FNT01"


def hzk_glyph(path, ch, cache={}):
    key = (path, ch)
    if key in cache:
        return cache[key]
    g = None
    try:
        b = ch.encode("gb2312")
    except UnicodeEncodeError:
        b = b""
    if len(b) == 2:
        idx = ((b[0] - 0xA1) * 94 + (b[1] - 0xA1)) * 72
        if 0 <= idx and idx + 72 <= os.path.getsize(path):
            with open(path, "rb") as f:
                f.seek(idx)
                d = f.read(72)
            if any(d):
                g = d
    cache[key] = g
    return g


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    src, base, out = sys.argv[1], sys.argv[2], sys.argv[3]

    d = open(base, "rb").read()
    assert d[:8] == MAGIC, "基准字库不是 ZH1FNT01"
    n = int.from_bytes(d[8:12], "little")
    chars = d[12:12 + n * 3]
    base_glyphs = d[12 + n * 3: 12 + n * 3 + n * 72]
    assert len(base_glyphs) == n * 72, "基准字库长度不符"

    glyphs = bytearray()
    fallback = 0
    changed = 0
    for i in range(n):
        ch = chars[i * 3:i * 3 + 3].decode("utf-8", "replace")
        g = hzk_glyph(src, ch)
        old = base_glyphs[i * 72:(i + 1) * 72]
        if g is None:
            g = old
            fallback += 1
        elif g != old:
            changed += 1
        glyphs += g

    out_d = bytearray()
    out_d += MAGIC
    out_d += n.to_bytes(4, "little")
    out_d += chars
    out_d += glyphs
    with open(out, "wb") as f:
        f.write(out_d)

    blank = sum(1 for i in range(n) if not any(glyphs[i * 72:(i + 1) * 72]))
    print("字库: %s → %s" % (src, out))
    print("  字数 %d (与基准一致), 体积 %d B (基准 %d B)" % (n, len(out_d), len(d)))
    print("  取自源字体 %d 字, 回退原字形 %d 字, 空白字形 %d 字"
          % (n - fallback, fallback, blank))
    return 0


if __name__ == "__main__":
    sys.exit(main())
