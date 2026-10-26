#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 SDLPal 源码提取所有大块字模数据 -> TF 卡文件（不占 flash）

为什么要提取：
  ESP32-S3 的 factory 分区只有 5MB，SDLPal 三套内置字模 + 字宽表要占
  1.05MB flash。游戏数据本来就在 TF 卡上，这些只读资源跟着外置最划算。

生成 4 个文件（放 TF 卡 /sdcard/pal/）：
  fontglyph.bin     2,162,688 B  = unicode_font[65536][32] 2MB + font_width[65536] 64KB
  fontglyph_cn.bin    300,424 B  简体字模  (8836 条)
  fontglyph_tw.bin    470,084 B  繁体字模 (13826 条)
  fontglyph_jp.bin    233,886 B  日文字模  (6879 条)

两级格式：
  unicode_font 段：定长 65536 x 32B，按码位顺序，无头无尾
  font_width 段  ：定长 65536 x 1B
  字模包        ：[u32 count][count x (u16 code_le + 32B bitmap)]

用法：
  python3 tools/gen_pal_fontbin.py <sdlpal_src_dir> <out_dir>
"""
import os
import re
import struct
import sys

UNICODE_COUNT = 65536
GLYPH_BYTES = 32


def parse_unicode_font(text):
    """unicode_font[65536][32] 只有前 57086 条有初值，其余隐式 0。"""
    m = re.search(r"unicode_font\s*\[\s*65536\s*\]\s*\[\s*32\s*\]\s*=\s*\{", text)
    if not m:
        raise ValueError("找不到 unicode_font[65536][32] 定义")
    rows, i = [], m.end()
    while len(rows) < UNICODE_COUNT:
        lb = text.find("{", i)
        if lb < 0:
            break
        rb = text.find("}", lb + 1)
        if rb < 0:
            break
        j = rb + 1
        while j < len(text) and text[j] in " \t\r\n":
            j += 1
        if j < len(text) and text[j] == ";":      # 数组结束
            break
        toks = [t for t in text[lb + 1:rb].replace("\n", "").replace("\r", "").split(",") if t.strip()]
        # ASCII 区是 8x16 = 16 字节，汉字 16x16 = 32 字节，短的补 0
        if len(toks) == 16:
            toks += ["0"] * 16
        if len(toks) != GLYPH_BYTES:
            raise ValueError("第 %d 个字形宽度异常: %d" % (len(rows), len(toks)))
        rows.append(bytes(int(t, 0) & 0xFF for t in toks))
        i = rb + 1
    if len(rows) < 1000:
        raise ValueError("解析出的字形太少: %d" % len(rows))
    return rows


def parse_font_width(text):
    """font_width[65536] 只写了前 57086 个，其余默认 32（与 SDLPal 语义一致）。"""
    m = re.search(r"font_width\s*\[\s*\d+\s*\]\s*=\s*\{", text)
    if not m:
        raise ValueError("找不到 font_width[] 定义")
    end = text.find("}", m.end())
    vals = [int(t, 0) & 0xFF for t in text[m.end():end].split(",") if t.strip()]
    if len(vals) < 1000:
        raise ValueError("font_width 解析过少: %d" % len(vals))
    return vals + [32] * (UNICODE_COUNT - len(vals))


def parse_glyph_pack(text, name):
    """fontglyph_cn/tw/jp: { 0x3000, { 0x00, ... } },  34 字节一条。"""
    m = re.search(r"fontglyph_\w+\s*\[\]\s*=\s*\{", text)
    if not m:
        raise ValueError("%s: 找不到数组定义" % name)
    out = []
    for cm in re.finditer(r"\{\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*\{(.*?)\}\s*\}", text[m.end():], re.S):
        code = int(cm.group(1), 0)
        vals = [int(t, 0) & 0xFF for t in cm.group(2).split(",") if t.strip()]
        vals += [0] * (GLYPH_BYTES - len(vals))
        out.append(struct.pack("<H", code) + bytes(vals[:GLYPH_BYTES]))
    if len(out) < 100:
        raise ValueError("%s: 只解析出 %d 条" % (name, len(out)))
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    srcdir, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    glyph_h = open(os.path.join(srcdir, "fontglyph.h"), encoding="utf-8", errors="replace").read()

    # ---- 1) unicode_font + font_width 合并成一个包（启动时一次性读）----
    uni = parse_unicode_font(glyph_h)
    wid = parse_font_width(glyph_h)
    uni_blob = b"".join(uni) + b"\x00" * ((UNICODE_COUNT - len(uni)) * GLYPH_BYTES)
    wid_blob = bytes(wid)
    p = os.path.join(outdir, "fontglyph.bin")
    with open(p, "wb") as f:
        f.write(uni_blob)
        f.write(wid_blob)
    print("%-22s %9d B  (字模 %d 条 + 字宽 %d 条)" % ("fontglyph.bin", len(uni_blob) + len(wid_blob), len(uni), len(wid)))

    # ---- 2) 三套内置字模包 ----
    for fn, tag in (("fontglyph_cn.h", "cn"), ("fontglyph_tw.h", "tw"), ("fontglyph_jp.h", "jp")):
        path = os.path.join(srcdir, fn)
        if not os.path.exists(path):
            print("跳过 %s (不存在)" % fn)
            continue
        packs = parse_glyph_pack(open(path, encoding="utf-8", errors="replace").read(), fn)
        blob = struct.pack("<I", len(packs)) + b"".join(packs)
        dst = os.path.join(outdir, "fontglyph_%s.bin" % tag)
        with open(dst, "wb") as f:
            f.write(blob)
        print("%-22s %9d B  (%d 条)" % ("fontglyph_%s.bin" % tag, len(blob), len(packs)))

    print("\n全部写到: %s  ⇒ 拷到 TF 卡 /sdcard/pal/" % outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
