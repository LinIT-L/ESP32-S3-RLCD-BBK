#!/usr/bin/env python3
"""
icon_to_c.py — 把一张 PNG 转成 svc_quickbar.c 用的 1bpp C 数组.

用法:
    python3 tools/icon_to_c.py 我的图标.png                    # 输出到 stdout
    python3 tools/icon_to_c.py 我的图标.png -n qb_wifi_24      # 指定数组名
    python3 tools/icon_to_c.py 我的图标.png -s 24 24 -n qb_wifi_24

规则 (与固件里 qb_blit_1bpp_scaled 完全一致):
  · 每行 ceil(W/8) 字节, MSB 在前 (最左像素是 0x80)
  · 1 = 黑, 0 = 白
  · PNG 可以是任意尺寸, 会先按"深色=黑"二值化(阈值 128), 再等比缩放居中到 W×H
  · 透明像素按白处理
"""
import argparse
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow: python3 -m pip install pillow")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("-s", "--size", nargs=2, type=int, default=[24, 24],
                    metavar=("W", "H"), help="输出像素宽高, 默认 24 24")
    ap.add_argument("-n", "--name", default="qb_wifi_24", help="C 数组名")
    ap.add_argument("-t", "--threshold", type=int, default=128)
    a = ap.parse_args()

    W, H = a.size
    im = Image.open(a.png).convert("RGBA")
    bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
    im = Image.alpha_composite(bg, im).convert("L")

    # 二值化后裁掉四周空白, 再等比缩放到 W×H 内并居中
    bin_img = im.point(lambda v: 255 if v >= a.threshold else 0)
    bbox = bin_img.point(lambda v: 255 if v < 128 else 0).getbbox()
    if bbox:
        bin_img = bin_img.crop(bbox)

    iw, ih = bin_img.size
    scale = min(W / iw, H / ih)
    nw, nh = max(1, round(iw * scale)), max(1, round(ih * scale))
    bin_img = bin_img.resize((nw, nh), Image.LANCZOS)
    ox, oy = (W - nw) // 2, (H - nh) // 2

    bpr = (W + 7) // 8
    data = [0] * (bpr * H)
    px = bin_img.load()
    for y in range(nh):
        for x in range(nw):
            if px[x, y] < a.threshold:
                X, Y = ox + x, oy + y
                data[Y * bpr + (X >> 3)] |= 0x80 >> (X & 7)

    rows = []
    for y in range(H):
        rows.append(",".join("0x%02X" % data[y * bpr + b] for b in range(bpr)))
    print("static const uint8_t %s[%d * %d] = {" % (a.name, bpr, H))
    print(",\n".join("    " + r for r in rows) + ",")
    print("};")


if __name__ == "__main__":
    main()
