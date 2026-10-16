#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_map_icon.py — 生成《矢量地图》应用的 64×64 1bpp 图标 (程序化线描)

规范与 `gen_main_icons_64.py` 一致: **4× 超采样 → LANCZOS 缩放 → 128 硬阈值**,
结果写到 `assets/icons/map_app.png` (1 位灰度, 0=墨 255=纸)。

★ 为什么**不加进** `main_icons[]`
   `main_icons[]` 的下标 = ABI (主菜单/应用管家的 `main_icon_index()` 里有重映射表),
   往里插一个图标会动到既有应用。所以地图图标走 `pack_app.py --icon <png>` 这条路:
   图标 base64 直接内嵌进 `.lua` 的 LAPP 前奏, **固件一行不改**。

用法:
    python3 tools/gen_map_icon.py                 # 生成 + 打印墨量 + 存 PNG
    python3 tools/gen_map_icon.py --out /tmp/x.png --ascii   # 控制台预览
"""
import argparse
import os
import sys

S = 4                       # 超采样倍数
W = H = 64
N = W * S

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "assets", "icons", "map_app.png")


def build():
    from PIL import Image, ImageDraw
    img = Image.new("L", (N, N), 255)
    d = ImageDraw.Draw(img)

    def px(v):
        return v * S

    def rect(x0, y0, x1, y1, w=1.0, r=0.0, fill=0):
        box = [px(x0), px(y0), px(x1), px(y1)]
        if r > 0:
            d.rounded_rectangle(box, radius=px(r), outline=fill, width=int(px(w)))
        else:
            d.rectangle(box, outline=fill, width=int(px(w)))

    def line(x0, y0, x1, y1, w=1.0, fill=0):
        d.line([px(x0), px(y0), px(x1), px(y1)], fill=fill, width=int(px(w)))

    def disc(cx, cy, r, fill=0):
        d.ellipse([px(cx - r), px(cy - r), px(cx + r), px(cy + r)], fill=fill)

    def poly(pts, fill=0):
        d.polygon([(px(x), px(y)) for x, y in pts], fill=fill)

    # ---------- ① 地图图纸 (圆角框) ----------
    rect(2, 13, 62, 53, w=2, r=4)

    # ---------- ② 底图路网: 细线 ----------
    line(2.5, 27, 61.5, 27, w=1.2)
    line(2.5, 42, 61.5, 42, w=1.2)
    line(16, 13.5, 16, 52.5, w=1.2)
    line(48, 13.5, 48, 52.5, w=1.2)

    # ---------- ③ 定位针: 先拿**白晕**把底图切开, 再画黑针 (否则针和路网糊在一起) ----------
    disc(32, 25, 11.5, fill=255)
    poly([(23.5, 23), (40.5, 23), (32, 50)], fill=255)

    disc(32, 25, 9.5)
    poly([(24.0, 27.0), (40.0, 27.0), (32, 48.5)])
    disc(32, 25, 3.6, fill=255)

    return img


def to_1bpp(img):
    from PIL import Image
    im = img.resize((W, H), Image.Resampling.LANCZOS)
    # 硬阈值: 1bpp 只有黑白, 抖动会让 64px 小图发毛
    return im.point(lambda v: 0 if v < 128 else 255)


def ascii_art(bw):
    px = bw.load()
    lines = []
    for y in range(H):
        lines.append("".join("#" if px[x, y] < 128 else "." for x in range(W)))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--ascii", action="store_true", help="控制台打印点阵")
    a = ap.parse_args()

    bw = to_1bpp(build())
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    bw.save(a.out)

    ink = sum(1 for y in range(H) for x in range(W) if bw.load()[x, y] < 128)
    print("  ✓ %s  (%dx%d 1bpp, 墨点 %d = %.1f%%)"
          % (a.out, W, H, ink, ink * 100.0 / (W * H)))
    if a.ascii:
        print(ascii_art(bw))


if __name__ == "__main__":
    main()
