#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_wp_1bpp.py — 把一张 PNG 转成 1bpp 位图 C 头文件（壁纸/图标资源通用）。

规范（LinTOS 图片资源命名）:
    符号 = <域>_<类别>_<对象>_<尺寸>_bits
    例: wp_sleep_default_300x200_bits? —— 不, 壁纸统一用
        wp_<用途>_<名>  +  _W/_H 宏, 见下。

用法:
    python3 tools/gen_wp_1bpp.py 图.png --name wp_sleep_default \
        --w 300 --h 200 --out components/ui/wallpapers/wp_sleep_default.h

约定:
  · 位=1 表示"白"(点亮的像素), 位=0 表示黑 —— 与 st7305_blit_1bit_white 一致。
  · 尺寸用 **面积平均 + 阈值** 缩放（不是最近邻）: 源图是带抗锯齿的灰阶,
    平均后再阈值能得到干净的边缘, 不会出现马赛克锯齿。
  · 每行 (W+7)/8 字节, MSB 在左。
"""
import argparse
import os

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--name", required=True, help="数组符号名 (不带 _bits)")
    ap.add_argument("--w", type=int, required=True)
    ap.add_argument("--h", type=int, required=True)
    ap.add_argument("--thr", type=int, default=110, help="阈值 (0-255), 默认 110")
    ap.add_argument("--out", required=True)
    ap.add_argument("--note", default="", help="头文件里的一句话说明")
    ap.add_argument("--rle", action="store_true",
                    help="逐行亮段压缩 (每行: u8 段数 + 每段 u16 起点/u8 长度)。"
                         "1bpp 壁纸通常压到 30%% 左右, 解码零依赖。")
    a = ap.parse_args()

    im = Image.open(a.src)
    # alpha 先压在黑底上: 透明处按黑算
    if im.mode in ("RGBA", "LA"):
        bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
        im = Image.alpha_composite(bg, im)
    g = im.convert("L")
    if g.size != (a.w, a.h):
        g = g.resize((a.w, a.h), Image.BOX)
    px = g.load()

    bpr = (a.w + 7) // 8
    data = bytearray(bpr * a.h)
    lit = 0
    for y in range(a.h):
        for x in range(a.w):
            if px[x, y] >= a.thr:
                data[y * bpr + (x >> 3)] |= 0x80 >> (x & 7)
                lit += 1

    name = a.name
    lines = []
    lines.append("/* %s" % (a.note or os.path.basename(a.src)))
    lines.append(" * 由 tools/gen_wp_1bpp.py 生成, 勿手工改.")
    lines.append(" * %dx%d 1bpp, MSB-first, 位=1 为白(点亮) — 配合 st7305_blit_1bit_white()" % (a.w, a.h))
    lines.append(" */")
    lines.append("#ifndef %s_%dx%d_H" % (name.upper(), a.w, a.h))
    lines.append("#define %s_%dx%d_H" % (name.upper(), a.w, a.h))
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("#define %s_W %d" % (name.upper(), a.w))
    lines.append("#define %s_H %d" % (name.upper(), a.h))

    if a.rle:
        # 逐行亮段编码: 每行 = u8 段数, 每段 = (起点高字节, 起点低字节, 长度)
        enc = bytearray()
        for y in range(a.h):
            row = data[y * bpr:(y + 1) * bpr]
            runs = []
            x = 0
            while x < a.w:
                if (row[x >> 3] >> (7 - (x & 7))) & 1:
                    s = x
                    while x < a.w and ((row[x >> 3] >> (7 - (x & 7))) & 1):
                        x += 1
                    runs.append((s, x - s))
                else:
                    x += 1
            assert len(runs) <= 255 and all(l <= 255 for _, l in runs), "行 %d 段太碎, 无法用 u8 编码" % y
            enc.append(len(runs))
            for s, l in runs:
                enc += bytes(((s >> 8) & 0xFF, s & 0xFF, l))
        lines.append("#define %s_RLE_BYTES %d" % (name.upper(), len(enc)))
        lines.append("")
        lines.append("static const uint8_t %s_rle[%d] = {" % (name, len(enc)))
        for i in range(0, len(enc), 16):
            lines.append("    " + ",".join("0x%02X" % b for b in enc[i:i + 16]) + ",")
        lines.append("};")
        lines.append("")
        lines.append("#endif")
        open(a.out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
        print("✅ %s  %dx%d  RLE %d 字节 (原 %d 字节, %.0f%%)  → %s"
              % (name, a.w, a.h, len(enc), len(data), 100.0 * len(enc) / len(data), a.out))
        return

    lines.append("")
    lines.append("static const uint8_t %s[%d] = {" % (name, len(data)))
    for i in range(0, len(data), 16):
        lines.append("    " + ",".join("0x%02X" % b for b in data[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    open(a.out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("✅ %s  %dx%d  %d 字节 (%.1f%% 点亮)  → %s"
          % (name, a.w, a.h, len(data), 100.0 * lit / (a.w * a.h), a.out))


if __name__ == "__main__":
    main()
