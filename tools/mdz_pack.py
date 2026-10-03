#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mdz_pack.py — 把桌面 minidayz-master（Godot 复刻工程）的美术资源打成掌机 1bpp 图集。

源：`~/Desktop/minidayz-master/asset/images/`
    character/  zed/  animal/  item/  equipment/  …（753 张 PNG）

关键事实（探查结果）
--------------------
- **角色/僵尸/动物/装备** 是 **32×32 帧的 sprite sheet**（例：`bandit_skin.png` 128×256 = 4×8 帧；
  `equipment/backpack/*.png` 128×352 = 4×11）。带透明通道。
- **道具** 是 9~40px 的**小图标**（弹药只有 9×8），本身就是单帧。
- 所以：sheet 按 32×32 切格并**丢掉全透明格**；图标整张当一帧。

1bpp 转换（复用 `qqt_bw` 的拉伸/抖动；但**必须带 alpha**）
----------------------------------------------------------
⛔ 直接套 `qqt_bw` 会出错：那些函数吃的是"灰度行"，不知道透明。
   这里的规则是：**透明像素绝不落墨**（否则精灵会带一块黑底），
   不透明像素里**较暗的**落墨（像素画的黑描边 + 暗部），亮部留白 → 线描质感。

产出（写到 `apps/lua/minidayz/`）
---------------------------------
  spr.bmp    人物/僵尸/动物 帧横排（32×32）
  item.bmp   道具图标横排（32×32 归一化，小图居中放大到 32 里更好看）
  tile.bmp   地形瓦片（程序化重绘：草地/道路/树/水/墙/地板/沙地）
  map.lua    元信息（各图集帧数与索引约定）
"""
import argparse
import glob
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
import qqt_bw as BW

SRC = os.path.expanduser("~/Desktop/minidayz-master/asset/images")
FRAME = 32


def load_rgba(path):
    im = Image.open(path)
    if im.mode != "RGBA":
        im = im.convert("RGBA")
    return im


def guess_frame(w, h, fs=FRAME):
    """推断 sprite sheet 的帧格边长。

    ⛔ 不能写死 32: 实测这套素材里帧格有好几种 ——
       128×352/128×480/128×224 = **32**(4列);
       172×258 = **43**;  184×460 = **46**;  84×105 = **21**;  64×112 = **16**。
    规律: 帧格一定是 **gcd(w,h) 的因子**。所以在 [16,64] 里挑最大的那个因子, 且优先 32。
    """
    g = math.gcd(w, h)
    cands = [d for d in range(16, 65) if g % d == 0]
    if not cands:
        cands = [d for d in range(6, 129) if g % d == 0]
    if not cands:
        return fs
    return 32 if 32 in cands else max(cands)


def slice_sheet(im, fs=None):
    """按 fs×fs 切格并丢掉**全透明格**。返回 [(col,row,Image)]。fs=None → 自动推断。"""
    w, h = im.size
    if fs is None:
        fs = guess_frame(w, h)
    cols, rows = max(1, w // fs), max(1, h // fs)
    out = []
    for r in range(rows):
        for c in range(cols):
            t = im.crop((c * fs, r * fs, (c + 1) * fs, (r + 1) * fs))
            if t.getchannel("A").getextrema()[1] > 16:      # 有内容
                out.append((c, r, t))
    return out


def rgba_to_bits(im, fs=FRAME, thresh=150, opaque_alpha=110):
    """RGBA 精灵 → fs×fs 的 0/1 墨点。
    规则：**透明绝不落墨**；不透明里亮度 < thresh 的落墨（黑描边/暗部），亮部留白。
    这样像素画的轮廓和结构都能保住，而不是糊成一坨黑。"""
    im = im.convert("RGBA").resize((fs, fs), Image.LANCZOS)
    px = im.load()
    bits = [[0] * fs for _ in range(fs)]
    for y in range(fs):
        for x in range(fs):
            r, g, b, a = px[x, y]
            if a < opaque_alpha:
                continue                                   # 透明 → 不落墨
            lum = 0.299 * r + 0.587 * g + 0.114 * b
            if lum < thresh:
                bits[y][x] = 1
    return bits


def glyph_icon(im, fs=FRAME, pad=3):
    """小图标（9~40px）→ 居中放大到 fs 帧里（**最近邻**，像素画不要插值），再按 alpha 转墨。"""
    im = im.convert("RGBA")
    w, h = im.size
    s = max(1, min((fs - pad * 2) // max(1, w), (fs - pad * 2) // max(1, h)))
    im = im.resize((w * s, h * s), Image.NEAREST)
    canvas = Image.new("RGBA", (fs, fs), (0, 0, 0, 0))
    canvas.paste(im, ((fs - im.size[0]) // 2, (fs - im.size[1]) // 2))
    px = canvas.load()
    bits = [[0] * fs for _ in range(fs)]
    for y in range(fs):
        for x in range(fs):
            r, g, b, a = px[x, y]
            if a < 110:
                continue
            lum = 0.299 * r + 0.587 * g + 0.114 * b
            if lum < 150:
                bits[y][x] = 1
    return bits


# ---------------- 瓦片（程序化重绘，理由同 QQ堂：原图是高清贴图，二值化必成噪点） ----------
def blank(w, h):
    return [[0] * w for _ in range(h)]


def hline(g, x0, x1, y, v=1):
    if 0 <= y < len(g):
        for x in range(max(0, x0), min(len(g[0]) - 1, x1) + 1):
            g[y][x] = v


def vline(g, x, y0, y1, v=1):
    if 0 <= x < len(g[0]):
        for y in range(max(0, y0), min(len(g) - 1, y1) + 1):
            g[y][x] = v


def box(g, x0, y0, x1, y1, v=1):
    hline(g, x0, x1, y0, v); hline(g, x0, x1, y1, v)
    vline(g, x0, y0, y1, v); vline(g, x1, y0, y1, v)


def make_tile(kind, T=20):
    g = blank(T, T)
    if kind == "grass":                                    # 草地: 稀疏草簇
        for (x, y) in ((3, 5), (12, 3), (7, 13), (16, 10), (2, 16)):
            g[y][x] = 1; g[y - 1][x - 1] = 1; g[y - 1][x + 1] = 1
    elif kind == "dirt":                                   # 土路: 小点
        for (x, y) in ((4, 4), (13, 7), (6, 14), (15, 15), (9, 9)):
            g[y][x] = 1
    elif kind == "tree":                                   # 树: 树冠 + 树干
        for r in (9, 6, 3):
            for k in range(-r, r + 1):
                yy = 9 - int((r * r - k * k) ** 0.5)
                if 0 <= yy < T:
                    g[yy][9 + k] = 1
        for i in range(4, 10):
            hline(g, 8 - (9 - i) // 3, 10 + (9 - i) // 3, i)
        vline(g, 9, 13, 18); vline(g, 10, 13, 18)
    elif kind == "water":                                  # 水: 波纹
        for y in (4, 8, 12, 16):
            hline(g, 2, 17, y)
            g[y - 1][3] = 1; g[y - 1][16] = 1
    elif kind == "wall":                                   # 建筑墙: 粗框 + 砖缝
        box(g, 0, 0, T - 1, T - 1)
        box(g, 1, 1, T - 2, T - 2)
        hline(g, 2, T - 3, 9); hline(g, 2, T - 3, 10)
        vline(g, 13, 2, 9); vline(g, 6, 11, T - 3)
    elif kind == "floor":                                  # 室内地板: 极淡格线
        for (x, y) in ((2, 2), (T - 4, 2), (2, T - 4), (T - 4, T - 4)):
            g[y][x] = 1; g[y][x + 1] = 1
    elif kind == "sand":
        for (x, y) in ((5, 3), (14, 6), (3, 11), (11, 15), (17, 12)):
            g[y][x] = 1; g[y + 1][x] = 1
    elif kind == "road":                                   # 柏油路: 中线
        hline(g, 8, 11, 0, 1)
        for y in range(0, T, 6):
            hline(g, 9, 10, y)
    return g


TILE_KINDS = ("grass", "dirt", "tree", "water", "wall", "floor", "sand", "road")


def write_bmp1(path, rows, w, h):
    import struct
    stride = ((w + 31) // 32) * 4
    pix = bytearray()
    for y in range(h - 1, -1, -1):
        line = bytearray([0xFF] * stride)
        for x in range(w):
            if rows[y][x]:
                line[x >> 3] &= (~(0x80 >> (x & 7))) & 0xFF
        pix += line
    off = 14 + 40 + 8
    fh = b"BM" + struct.pack("<IHHI", off + len(pix), 0, 0, off)
    dh = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 1, 0, len(pix), 2835, 2835, 2, 2)
    pal = bytes((0, 0, 0, 0)) + bytes((255, 255, 255, 0))
    open(path, "wb").write(fh + dh + pal + bytes(pix))


def hstack(tiles, th):
    out = []
    for y in range(th):
        row = []
        for t in tiles:
            row += t[y]
        out.append(row)
    return out


# 挑素材：优先 *_skin.png（真正的行走图集），其次 sheet
# 素材组: 第一项是显示名, 第二项是候选路径(取第一个存在的)
# ⛔ 别用 `*_base-sheet0.png` / `*_breathe-sheet0.png` —— 那些是 **2×2 / 6×6 的碰撞占位图**,
#   真正的行走图集在 `character/player_skin/player_skin_*.png`。
SPR_GROUPS = [
    ("player",   ["character/player_skin/player_skin_def_1.png",
                  "character/player_skin/player_skin_def_2.png"]),
    ("bandit",   ["character/bandit_skin.png"]),
    ("tutor",    ["character/bot_tutor.png"]),
    ("z_normal", ["zed/zed_normal_skin1.png"]),
    ("z_fast",   ["zed/zed_fast_skin1.png"]),
    ("z_army",   ["zed/zed_army_skin1.png"]),
    ("z_shooter",["zed/zed_shooter_skin1.png"]),
    ("z_scream", ["zed/zed_screamer.png"]),
    ("z_buried", ["zed/zed_buried_skin.png"]),
    ("z_tank",   ["zed/zed_tank_skin.png"]),
    ("dog",      ["animal/dog_skin.png"]),
    ("deer",     ["animal/deer_skin.png"]),
    ("wolf",     ["animal/wolf_skin.png"]),
    ("rabbit",   ["animal/rabbit_skin.png"]),
    ("crow",     ["animal/crow_skin.png"]),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=SRC)
    ap.add_argument("--out", default="apps/lua/minidayz")
    ap.add_argument("--preview", default="/tmp/mdz_pack.png")
    ap.add_argument("--max-frames-per-sheet", type=int, default=12)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    # ---------- 1. 人物/僵尸/动物 帧 ----------
    frames, meta = [], []
    for name, paths in SPR_GROUPS:
        got, seen = 0, set()
        for rel in paths:
            p = os.path.join(args.src, rel)
            if not os.path.exists(p):
                continue
            for (c, r, t) in slice_sheet(load_rgba(p)):
                if got >= args.max_frames_per_sheet:
                    break
                b = rgba_to_bits(t)
                key = "".join("".join(str(v) for v in row) for row in b)
                if key in seen:            # 图集里重复帧很多, 去掉省体积
                    continue
                seen.add(key)
                frames.append(b)
                got += 1
            break                       # 每个角色只用第一张 sheet
        meta.append((name, got))
        print("  %-12s %d 帧" % (name, got))
    if not frames:
        raise SystemExit("没取到任何帧 —— 检查 --src")
    write_bmp1(os.path.join(args.out, "spr.bmp"), hstack(frames, FRAME), FRAME * len(frames), FRAME)
    print("  spr.bmp  %d 帧 @%dpx" % (len(frames), FRAME))

    # ---------- 2. 道具 ----------
    ipaths = sorted(glob.glob(os.path.join(args.src, "item", "*.png")))[:64]
    icons = [glyph_icon(load_rgba(p)) for p in ipaths]
    write_bmp1(os.path.join(args.out, "item.bmp"), hstack(icons, FRAME), FRAME * len(icons), FRAME)
    print("  item.bmp %d 图标 @%dpx" % (len(icons), FRAME))

    # ---------- 3. 地形瓦片 ----------
    tiles = [make_tile(k) for k in TILE_KINDS]
    write_bmp1(os.path.join(args.out, "tile.bmp"), hstack(tiles, 20), 20 * len(tiles), 20)
    print("  tile.bmp %d 格 @20px = %s" % (len(tiles), " ".join(TILE_KINDS)))

    # ---------- 4. map.lua ----------
    L = ["-- minidayz/map.lua — Mini DayZ 掌机版资源元信息 (由 tools/mdz_pack.py 生成)",
         "-- 素材来源: ~/Desktop/minidayz-master/asset/images (Godot 复刻工程, 粉丝作品)",
         "--   spr.bmp  人物/僵尸/动物 帧横排, 每帧 %dpx; 顺序:" % FRAME]
    idx = 0
    for name, n in meta:
        if n:
            L.append("--     %-12s 帧 %d..%d" % (name, idx, idx + n - 1))
            idx += n
    L += ["--   item.bmp 道具图标 %d 个 @%dpx" % (len(icons), FRAME),
          "--   tile.bmp 地形 %d 格 @20px = %s" % (len(tiles), " ".join(TILE_KINDS)),
          "return {",
          "  dir = \"/store/minidayz/\",",
          "  spr_px = %d, spr_n = %d," % (FRAME, len(frames)),
          "  item_px = %d, item_n = %d," % (FRAME, len(icons)),
          "  tile = 20,",
          "  groups = {"]
    idx = 0
    for name, n in meta:
        if n:
            L.append('    { name="%s", first=%d, count=%d },' % (name, idx, n))
            idx += n
    L += ["  },", "}", ""]
    open(os.path.join(args.out, "map.lua"), "w", encoding="utf-8").write("\n".join(L))
    print("  map.lua  角色 %d 组 / 道具 %d / 瓦片 %d" % (len(meta), len(icons), len(tiles)))

    # ---------- 5. 预览 ----------
    try:
        cols = 32
        rows = (len(frames) + cols - 1) // cols + (len(icons) + cols - 1) // cols
        im = Image.new("L", (cols * 34 + 8, rows * 34 + 8), 255)
        k = 0
        for i, f in enumerate(frames + icons):
            r, c = divmod(i, cols)
            for y in range(FRAME):
                for x in range(FRAME):
                    if f[y][x]:
                        im.putpixel((4 + c * 34 + x, 4 + r * 34 + y), 0)
            k += 1
        im.resize((im.size[0] * 3, im.size[1] * 3), Image.NEAREST).save(args.preview)
        print("  预览 → %s (%d 帧)" % (args.preview, k))
    except Exception as e:
        print("  (跳过预览: %s)" % e)
    print("\n完成 → %s" % args.out)


if __name__ == "__main__":
    main()
