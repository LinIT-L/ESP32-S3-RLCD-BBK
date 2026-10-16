#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""qqt_pack.py — 把《QQ堂》v5.2 客户端素材打包成掌机用的 1bpp 资源 (v3 精细版)。

产出（写到 apps/lua/qqtang/，设备上 = /store/qqtang/）
------------------------------------------------------
  bg<N>.bmp     12 个世界的关卡背景（原版 map/pve<N>.img 150×130 → LANCZOS 等比放大
                铺满 400×300 + 8×8 有序抖动）—— 网点质感, 不再是黑块
  tile.bmp      4 格瓦片横排 @20px（砖块/地板/硬墙/门）—— 程序化重绘, 三层错缝砖
  face.bmp      16 个吉祥物表情 @48px（原版 face/faces/*.img）→ 冒险大厅「人物展示」
  item.bmp      16 个道具图标 @24px（原版 icon/item/*.img）  → 背包/道具栏
  wicon.bmp     12 个世界图标 @40×36（原版 map/bigIcon/*.img）→ 选关页
  equip.bmp     4 个装备槽图标 @24px（头/身/翅膀/泡泡）
  map.lua       元信息

转换算法见 `tools/qqt_bw.py`（重采样 LANCZOS + 色调拉伸 + 8×8 有序抖动 + 描边）。
⛔ 不要再退回"全局分位数硬阈值 + 最近邻缩放" —— 那正是第一版被判定"太粗糙"的原因。
"""
import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qqt_img import parse, decode_frame          # 复用已验证的双块解码器
import qqt_bw as BW
import qqt_spr as SPR

# 12 个世界: (世界名, 背景 img, 关数, 选关页 bigIcon) —— 关数取自 map/mapDesc.py
WORLDS = [
    ("魔法王国1", "pve1.img",  6, "pve"),
    ("魔法王国2", "pve2.img",  5, "sculpture"),
    ("机械王国",  "pve3.img",  4, "machine"),
    ("黑龙王国",  "pve4.img",  7, "tank"),
    ("迷之森林",  "pve5.img",  5, "field"),
    ("迷之沙漠",  "pve6.img",  5, "desert"),
    ("迷之底",    "pve7.img",  4, "mine"),
    ("风暴海湾",  "pve8.img",  3, "water"),
    ("极昼平原",  "pve9.img",  3, "town"),
    ("怒风峡谷",  "pve10.img", 3, "bomb"),
    ("守望山麓",  "pve11.img", 3, "pig"),
    ("永冬之颠",  "pve12.img", 2, "snow"),
]

TILE = 20
BG_W, BG_H = 400, 300

ITEM_IDX = [1, 180, 2, 4, 0, 112, 113, 12, 13, 168, 170, 190, 200, 162, 3, 151]
EQUIP_IDX = [3, 151, 4, 1]                       # 头/身/翅膀/泡泡
FACE_IDX = [0, 6, 12, 18, 24, 30, 36, 42, 48, 54, 60, 72, 84, 96, 108, 120]

FACE_PX, ITEM_PX, EQUIP_PX = 48, 24, 32
WICON_W, WICON_H = 40, 36


# ============================================================ 位图写入
def write_bmp1(path, rows, w, h):
    """1bpp BMP, 自下而上, 位=1 表示白 → 所以 ink 写成位 0。"""
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


# ============================================================ 绘图小工具
def blank(w, h):
    return [[0] * w for _ in range(h)]


def hline(g, x0, x1, y, v=1):
    if not (0 <= y < len(g)):
        return
    for x in range(max(0, x0), min(len(g[0]) - 1, x1) + 1):
        g[y][x] = v


def vline(g, x, y0, y1, v=1):
    if not (0 <= x < len(g[0])):
        return
    for y in range(max(0, y0), min(len(g) - 1, y1) + 1):
        g[y][x] = v


def box(g, x0, y0, x1, y1, v=1):
    hline(g, x0, x1, y0, v)
    hline(g, x0, x1, y1, v)
    vline(g, x0, y0, y1, v)
    vline(g, x1, y0, y1, v)


def hstack(tiles, th):
    out = []
    for y in range(th):
        row = []
        for t in tiles:
            row += t[y]
        out.append(row)
    return out


def line(g, x0, y0, x1, y1, v=1):
    """整数 Bresenham 直线 (画瓦片纹理与图标用)。"""
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy
    H, W = len(g), len(g[0])
    while True:
        if 0 <= y0 < H and 0 <= x0 < W:
            g[y0][x0] = v
        if x0 == x1 and y0 == y1:
            break
        e2 = err * 2
        if e2 > -dy:
            err -= dy; x0 += sx
        if e2 < dx:
            err += dx; y0 += sy


def arc(g, cx, cy, r, v=1):
    """圆环 (画齿轮/太阳/传送门用)。"""
    x, y, d = r, 0, 1 - r
    while x >= y:
        for px, py in ((cx + x, cy + y), (cx + y, cy + x), (cx - y, cy + x), (cx - x, cy + y),
                       (cx - x, cy - y), (cx - y, cy - x), (cx + y, cy - x), (cx + x, cy - y)):
            if 0 <= py < len(g) and 0 <= px < len(g[0]):
                g[py][px] = v
        y += 1
        if d < 0:
            d += 2 * y + 1
        else:
            x -= 1
            d += 2 * (y - x) + 1


# ============================================================ 瓦片 (程序化重绘)
# ★ 2026-10-03 v6: **加变体** —— 这是"丑"的主因。
#   上一版整片地面是同一种 20px 点阵, 72×54 格看过去就是一张"打孔纸";
#   参考项目(Bombman/BnB)都是**同一地形 3~6 个变体**交替, 靠变体打破重复感。
#   材质区分原则: 靠**线宽/线密**, 不是靠"谁更黑"(全黑会变成一块糊)。
#   图集索引: 0-6 = 逻辑类型 (也是关卡里存的值)
#             7+  = 变体 (Lua 侧按格子哈希挑, 同格永远同款 → 重画不会闪)
TILE_KINDS = ("floor", "rubble", "rock", "treasure", "grass", "water", "exit",
              "floor_pebble", "floor_tuft", "floor_crack", "floor_slab",
              "rock_b", "rock_c")
T_FLOOR, T_RUBBLE, T_ROCK, T_TREASURE, T_GRASS, T_WATER, T_EXIT = 0, 1, 2, 3, 4, 5, 6
V_FP, V_FT, V_FC, V_FS, V_RB, V_RC = 7, 8, 9, 10, 11, 12


def make_tile(kind, T=TILE):
    g = blank(T, T)
    if kind == "floor":
        # 素面地板: 四角 + 中心小点 (只让格子看得见)
        for (px, py) in ((2, 2), (T - 4, 2), (2, T - 4), (T - 4, T - 4)):
            fill_box(g, px, py, px + 1, py + 1)
        g[T // 2][T // 2] = 1
    elif kind == "floor_pebble":                 # 碎石粒
        fill_box(g, 2, 2, 3, 3)
        fill_box(g, T - 5, T - 5, T - 4, T - 4)
        fill_box(g, T // 2 - 1, T // 2 + 2, T // 2, T // 2 + 3)
        g[3][T // 2] = 1
        g[T - 4][3] = 1
    elif kind == "floor_tuft":                   # 草簇
        for x0 in (5, 14):
            line(g, x0 - 3, 15, x0, 9)
            line(g, x0 + 3, 15, x0, 9)
            line(g, x0, 9, x0, 6)
    elif kind == "floor_crack":                  # 裂纹
        line(g, 2, 3, 7, 9)
        line(g, 7, 9, 5, 14)
        line(g, 7, 9, 13, 12)
        line(g, 13, 12, T - 3, T - 5)
    elif kind == "floor_slab":                   # 石板缝
        hline(g, 1, T - 2, T // 2)
        vline(g, T // 2 + 3, 1, T // 2 - 1)
        vline(g, T // 2 - 4, T // 2 + 1, T - 2)
    elif kind == "rubble":
        # 可炸碎石: 细密砌石线 (线细而密 → "碎")
        box(g, 0, 0, T - 1, T - 1)
        hline(g, 1, T - 2, 6)
        hline(g, 1, T - 2, 13)
        vline(g, 7, 1, 6)
        vline(g, 13, 7, 13)
        vline(g, 2, 14, T - 2)
    elif kind == "rock":
        # 岩壁: 2px 粗框 + 中心十字 → "厚重石块"。线粗 = 打不破, 一眼区分
        box(g, 0, 0, T - 1, T - 1)
        box(g, 1, 1, T - 2, T - 2)
        hline(g, 6, T - 7, T // 2 - 1)
        hline(g, 6, T - 7, T // 2)
        vline(g, T // 2 - 1, 6, T - 7)
        vline(g, T // 2, 6, T - 7)
    elif kind == "rock_b":                       # 岩壁变体: 斜纹
        box(g, 0, 0, T - 1, T - 1)
        box(g, 1, 1, T - 2, T - 2)
        line(g, 4, T - 5, T - 5, 4)
        line(g, 5, T - 4, T - 4, 5)
        line(g, 8, T - 5, T - 5, 8)
    elif kind == "rock_c":                       # 岩壁变体: 实心小方 (最厚的"核心岩")
        box(g, 0, 0, T - 1, T - 1)
        box(g, 1, 1, T - 2, T - 2)
        fill_box(g, 6, 6, T - 7, T - 7)
    elif kind == "treasure":
        box(g, 0, 0, T - 1, T - 1)
        hline(g, 1, T - 2, 6)
        hline(g, 1, T - 2, 13)
        vline(g, 7, 1, 6)
        vline(g, 13, 7, 13)
        hline(g, 7, 12, 9)
        hline(g, 8, 11, 10)
        hline(g, 9, 10, 11)
    elif kind == "grass":
        for x0 in (4, 12, 19):
            for k in (0, 1, 2):
                xx = x0 - 4 + k * 3
                line(g, xx, 17, xx + 2, 11)
                line(g, xx + 4, 17, xx + 2, 11)
                line(g, xx + 2, 11, xx + 2, 8)
    elif kind == "water":
        box(g, 0, 0, T - 1, T - 1)
        for y in (4, 7, 10, 13, 16):
            hline(g, 2, T - 3, y)
            g[y - 1][3] = 1
            g[y - 1][T - 4] = 1
    elif kind == "exit":
        box(g, 0, 0, T - 1, T - 1)
        box(g, 3, 3, T - 4, T - 4)
        fill_box(g, 8, 8, T - 9, T - 9)
    return g


def fill_box(g, x0, y0, x1, y1, v=1):
    for y in range(y0, y1 + 1):
        hline(g, x0, x1, y, v)


# ============================================================ 世界图标 (程序化)
# ⛔ 不要用原版 map/bigIcon 的 3D 渲染图: 缩到 40×36 二值化后必然是**一坨黑**
#    (用户原话"左侧的又特别黑")。改成按世界名程序化画**线描符号**, 墨量 ~15%, 一眼可辨。
def wicon(kind, W=WICON_W, H=WICON_H):
    g = blank(W, H)
    cx, cy = W // 2, H // 2
    if kind == "star":                          # 魔法王国1
        for r in (7, 12, 5):
            arc(g, cx, cy, r)
        line(g, cx, 5, cx, H - 6)
        line(g, 8, cy, W - 9, cy)
        line(g, 12, 9, W - 13, H - 10)
        line(g, W - 13, 9, 12, H - 10)
    elif kind == "crown":                       # 魔法王国2
        fill_box(g, 7, 26, W - 8, 29)
        for x0 in (8, 17, 26):
            line(g, x0, 25, x0 + 4, 8)
            line(g, x0 + 4, 8, x0 + 8, 25)
        arc(g, 12, 7, 2); arc(g, 20, 5, 2); arc(g, 28, 7, 2)
    elif kind == "gear":                        # 机械王国
        arc(g, cx, cy, 11)
        arc(g, cx, cy, 5)
        for k in range(8):
            import math as _m
            a = k * _m.pi / 4
            x0 = int(cx + 11 * _m.cos(a)); y0 = int(cy + 11 * _m.sin(a))
            x1 = int(cx + 16 * _m.cos(a)); y1 = int(cy + 16 * _m.sin(a))
            line(g, x0, y0, x1, y1)
    elif kind == "flame":                       # 黑龙王国
        line(g, cx, 4, cx - 8, 20)
        line(g, cx, 4, cx + 8, 20)
        line(g, cx - 8, 20, cx, 31)
        line(g, cx + 8, 20, cx, 31)
        line(g, cx - 5, 12, cx + 5, 12)
        hline(g, 14, 25, 22)
    elif kind == "tree":                        # 迷之森林
        line(g, cx, 6, 6, 20); line(g, cx, 6, W - 7, 20)
        hline(g, 6, W - 7, 20)
        line(g, cx, 14, 10, 26); line(g, cx, 14, W - 11, 26)
        hline(g, 10, W - 11, 26)
        fill_box(g, cx - 2, 26, cx + 1, 32)
    elif kind == "pyramid":                     # 迷之沙漠
        line(g, 6, 30, cx, 8); line(g, W - 7, 30, cx, 8)
        hline(g, 6, W - 7, 30)
        for y in (16, 21, 26):
            w = int((y - 8) * 1.3)
            hline(g, cx - w, cx + w, y)
    elif kind == "mine":                        # 迷之底
        arc(g, cx, cy - 4, 9)
        vline(g, cx - 9, cy - 4, 30); vline(g, cx + 9, cy - 4, 30)
        hline(g, cx - 9, cx + 9, 30)
        line(g, 8, 28, W - 9, 10); line(g, 8, 10, W - 9, 28)
    elif kind == "wave":                        # 风暴海湾
        for y in (12, 20, 28):
            for k in range(5):
                x = 5 + k * 6
                arc(g, x, y, 3)
    elif kind == "sun":                         # 极昼平原
        arc(g, cx, cy, 8)
        for k in range(8):
            import math as _m
            a = k * _m.pi / 4
            line(g, int(cx + 10 * _m.cos(a)), int(cy + 10 * _m.sin(a)),
                 int(cx + 16 * _m.cos(a)), int(cy + 16 * _m.sin(a)))
    elif kind == "bolt":                        # 怒风峡谷
        pts = [(24, 5), (13, 18), (20, 18), (12, 32), (28, 16), (21, 16), (29, 5)]
        for i in range(len(pts) - 1):
            line(g, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1])
    elif kind == "mount":                       # 守望山麓
        line(g, 3, 30, 14, 8); line(g, 14, 8, 25, 30)
        line(g, 18, 30, 28, 13); line(g, 28, 13, W - 4, 30)
        hline(g, 3, W - 4, 30)
        line(g, 11, 14, 14, 10); line(g, 17, 14, 14, 10)
    elif kind == "snow":                        # 永冬之颠
        for k in range(3):
            import math as _m
            a = k * _m.pi / 3
            line(g, int(cx - 15 * _m.cos(a)), int(cy - 15 * _m.sin(a)),
                 int(cx + 15 * _m.cos(a)), int(cy + 15 * _m.sin(a)))
        arc(g, cx, cy, 4)
    return g


# 12 个世界 → 符号 (顺序必须与 WORLDS 一致)
WICON_KINDS = ["star", "crown", "gear", "flame", "tree", "pyramid",
               "mine", "wave", "sun", "bolt", "mount", "snow"]


# ============================================================ 素材装载
def _img_rows(path):
    im = parse(path)
    if not im or not im["exact"]:
        return None
    g = decode_frame(im, 0)
    if not g:
        return None
    return g, im["dw"], im["dh"]


def load_seq(paths, idxs):
    out = []
    for i in idxs:
        if i >= len(paths):
            continue
        r = _img_rows(paths[i])
        if r:
            out.append((i, r[0], r[1], r[2]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="/tmp/qt")
    ap.add_argument("--out", default="apps/lua/qqtang")
    ap.add_argument("--preview", default="/tmp/qqt_pack_preview.png")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    # ---------- 1. 12 个世界背景 (整屏 400×300, 有序抖动网点) ----------
    made = []
    for i, (name, img, nstage, icon) in enumerate(WORLDS, 1):
        p = os.path.join(args.src, "map", img)
        if not os.path.exists(p):
            print("  ⚠ 缺背景 %s" % img)
            continue
        g, w, h = _img_rows(p)
        # ★ 背景是"衬底", 墨量必须低(0.13) —— 高了会把地板变成一片麻点, 地图就读不出来了
        bits = BW.to_bits(g, BG_W, BG_H, ink=0.07, gamma=0.72, lo=0.02, hi=0.98)
        write_bmp1(os.path.join(args.out, "bg%d.bmp" % i), bits, BG_W, BG_H)
        made.append((i, name, img, nstage, icon))
        print("  bg%-2d %-10s %-10s %dx%d  %d 关 [%s]" % (i, name, img, BG_W, BG_H, nstage, icon))

    # ---------- 2. 瓦片 @20px ----------
    tiles = [make_tile(t) for t in TILE_KINDS]
    sheet = hstack(tiles, TILE)
    write_bmp1(os.path.join(args.out, "tile.bmp"), sheet, TILE * len(tiles), TILE)
    print("  tile.bmp  %d 格 × %dpx = %dx%d" % (len(tiles), TILE, TILE * len(tiles), TILE))

    # ---------- 3. 吉祥物表情 @48px (黑底亮主体 → invert + 描边) ----------
    faces = load_seq(sorted(glob.glob(os.path.join(args.src, "res/uiRes/face/faces/*.img"))), FACE_IDX)
    if faces:
        fs = [BW.to_bits(g, FACE_PX, FACE_PX, ink=0.40, gamma=0.8, use_dither=False,
                         invert=True, outline=1) for _, g, w, h in faces]
        sheet = hstack(fs, FACE_PX)
        write_bmp1(os.path.join(args.out, "face.bmp"), sheet, FACE_PX * len(fs), FACE_PX)
        print("  face.bmp  %d 表情 %dx%d" % (len(fs), FACE_PX * len(fs), FACE_PX))

    # ---------- 4. 道具图标 @24px ----------
    ps = sorted(glob.glob(os.path.join(args.src, "res/uiRes/icon/item/*.img")))
    items = load_seq(ps, ITEM_IDX)
    if items:
        its = [BW.to_bits(g, ITEM_PX, ITEM_PX, ink=0.36, gamma=0.90, use_dither=False,
                          invert=True, outline=1) for _, g, w, h in items]
        sheet = hstack(its, ITEM_PX)
        write_bmp1(os.path.join(args.out, "item.bmp"), sheet, ITEM_PX * len(its), ITEM_PX)
        print("  item.bmp  %d 道具 %dx%d" % (len(its), ITEM_PX * len(its), ITEM_PX))

    # ---------- 5. 装备四槽 @24px ----------
    eq = load_seq(ps, EQUIP_IDX)
    if eq:
        es = [BW.to_bits(g, EQUIP_PX, EQUIP_PX, ink=0.36, gamma=0.90, use_dither=False,
                         invert=True, outline=1) for _, g, w, h in eq]
        sheet = hstack(es, EQUIP_PX)
        write_bmp1(os.path.join(args.out, "equip.bmp"), sheet, EQUIP_PX * len(es), EQUIP_PX)
        print("  equip.bmp %d 装备槽 %dx%d" % (len(es), EQUIP_PX * len(es), EQUIP_PX))

    # ---------- 6. 世界图标 @40×36 ----------
    # ★ 世界图标 = 程序化线描符号 (不用原版 3D bigIcon: 缩到 40×36 必然变黑疙瘩)
    ws = [wicon(WICON_KINDS[i % len(WICON_KINDS)]) for i in range(len(made))]
    if ws:
        sheet = hstack(ws, WICON_H)
        write_bmp1(os.path.join(args.out, "wicon.bmp"), sheet, WICON_W * len(ws), WICON_H)
        print("  wicon.bmp %d 世界图标 %dx%d" % (len(ws), WICON_W * len(ws), WICON_H))

    # ---------- 6b. 人物/怪物精灵图集 (程序化绘制, 见 qqt_spr.py) ----------
    sfr = SPR.build_frames()
    SPX = SPR.PX
    sheet = hstack(sfr, SPX)
    write_bmp1(os.path.join(args.out, "spr.bmp"), sheet, SPX * len(sfr), SPX)
    print("  spr.bmp   %d 帧 @%dpx (0-7 人物: dir*2+frame; 8-11 怪物 近战/元素/引爆/BOSS)"
          % (len(sfr), SPX))

    # ---------- 7. map.lua ----------
    L = ["-- qqtang/map.lua — 《QQ堂》冒险模式资源元信息 (由 tools/qqt_pack.py 生成, 不要手改)",
         "-- 设备端路径前缀 /store/qqtang/ ; 位图 = app.img.load(path) 后 blit",
         "--   tile.bmp  %d 格 @%dpx  = %s" % (len(tiles), TILE, " ".join(
             "%d%s" % (i, k) for i, k in enumerate(TILE_KINDS))),
         "--   face.bmp  %d 表情 @%dpx  0 是默认正脸" % (len(faces), FACE_PX),
         "--   item.bmp  %d 道具 @%dpx" % (len(items), ITEM_PX),
         "--   wicon.bmp %d 世界图标(程序化线描符号) @%dx%d" % (len(ws), WICON_W, WICON_H),
         "--   equip.bmp 4 装备槽 @%dpx  0头 1身 2翅膀 3泡泡" % EQUIP_PX,
         "--   sppr.bmp = 人物/怪物精灵; bg 墨量 0.07 (地板衬底, 很淡; 大地图下已基本不用)",
         "return {",
         "  dir = \"/store/qqtang/\",",
         "  tile = %d," % TILE,
         "  bg_w = %d, bg_h = %d," % (BG_W, BG_H),
         "  face_px = %d, item_px = %d, equip_px = %d, wicon_w = %d, wicon_h = %d,"
         % (FACE_PX, ITEM_PX, EQUIP_PX, WICON_W, WICON_H),
         "  spr_px = %d, spr_hero = 8, spr_mon = 4," % SPX,
         "  view_cols = %d, view_rows = %d," % (BG_W // TILE, BG_H // TILE),
         "  worlds = {"]
    for i, name, img, nstage, icon in made:
        L.append('    { id=%d, name="%s", bg="bg%d.bmp", stages=%d, icon="%s" },'
                 % (i, name, i, nstage, icon))
    L += ["  },", "}", ""]
    open(os.path.join(args.out, "map.lua"), "w", encoding="utf-8").write("\n".join(L))
    print("  map.lua   %d 个世界, 总 %d 关" % (len(made), sum(m[3] for m in made)))

    # ---------- 8. 预览: 背景 + 叠瓦片, 长什么样 ----------
    try:
        from PIL import Image
        bgp = os.path.join(args.out, "bg1.bmp")
        prev = Image.open(bgp).convert("L") if os.path.exists(bgp) else Image.new("L", (BG_W, BG_H), 255)
        px = prev.load()
        for gy in range(BG_H // TILE):
            for gx in range(BG_W // TILE):
                if gx == 0 or gy == 0 or gx == BG_W // TILE - 1 or gy == BG_H // TILE - 1:
                    t = tiles[2]                      # rock
                elif (gx * 5 + gy * 3) % 11 < 2:
                    t = tiles[2]                      # rock
                elif (gx * 7 + gy * 3) % 5 < 2:
                    t = tiles[1]                      # rubble
                elif (gx * 3 + gy * 5) % 13 == 0:
                    t = tiles[4]                      # grass
                else:
                    continue
                for y in range(TILE):
                    for x in range(TILE):
                        if t[y][x]:
                            px[gx * TILE + x, gy * TILE + y] = 0
        prev.save(args.preview)
        print("  预览 → %s" % args.preview)
    except Exception as e:
        print("  (跳过预览: %s)" % e)

    print("\n完成 → %s" % args.out)


if __name__ == "__main__":
    main()
