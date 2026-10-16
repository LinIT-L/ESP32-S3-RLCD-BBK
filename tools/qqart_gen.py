#!/usr/bin/env python3
"""qqart_gen.py — 把参考项目《QQ堂》的真实美术素材转成掌机 1bpp 精灵 (hex 文本)

为什么要有它 (linit 2026-10-03 明确要求):
    "你不要自己画呢, 去找完善的 QQ堂" —— 之前 qqtang.lua 里的精灵是我手写 "#./" 点阵,
    又丑又不还原。本脚本直接从 `qqtang-web` 的原始 PNG 切帧 + 缩放 + 抖动,
    生成 `-- @@ART-BEGIN@@ … -- @@ART-END@@` 之间的 Lua 数据段。

素材来源 (本地已克隆 /tmp/qqtang-ref/dist/static/res):
    走图/Done_body*_walk.png      400x400 = 4x4 格 100px   角色行走 (主角)
    物品/Cap*_walk.png            400x400 = 4x4 格 100px   各种怪物 (螃蟹等)
    泡泡/bubble_normal.png        219x84  = 3 帧 ~73px     泡泡球
    泡泡/bubbleboom.png           512x64  = 8 帧 64px      爆炸特效
    地图/paotile.png              600x240 = 4 行地形带      砖/草/水/石 等

转换策略 (1bpp 反射屏 = 白纸黑字):
    · 角色/泡泡/爆炸: 认 alpha 透明; 不透明的按亮度阈值二值化
    · 地形: 按亮度做 **Bayer 4x4 有序抖动** —— 直接阈值会让草地/水整格变黑,
      抖动后每格黑点约占 30~50%, 既分得出材质又不会糊成一块

用法:
    python3 tools/qqart_gen.py                 # 生成并注入 qqtang.lua
    python3 tools/qqart_gen.py --preview out.png   # 顺便存一张 1bpp 效果预览
"""
import argparse
import os
import re
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = "/tmp/qqtang-ref/dist/static/res"
GAME = os.path.expanduser("~/Desktop/LinTOS_Lua应用/src/qqtang.lua")
BEG, END = "-- @@ART-BEGIN@@", "-- @@ART-END@@"

TILE = 16          # 地形边长 (Kenney Tiny Dungeon 原生 16x16 → **1:1 不缩放, 不糊**)
ACT = 16           # 角色/泡泡边长 (正好一格)

# ★ 数据源换血 (2026-10-03, 版权原因):
#   原来读的是 qqtang-web 的素材, 但实测那批是**爱给网转载的盛大《泡泡堂》素材**, 版权不干净。
#   现改用 **Kenney Tiny Dungeon (CC0 公共领域, kenney.nl)** —— 132 个原生 16x16 图块,
#   而且**尺寸正好等于我们的瓦片**, 不需要任何缩放 → 观感立刻干净 (之前糊就是因为 100px 插画缩到 16px)。
#   泡泡与爆炸 CC0 包里没有 → 用**程序生成**(圆环 / 放射尖刺), 一样是原创, 无版权问题。
KENNEY = "/tmp/kenney_td/Tiles"
KENNEY_IDX = {
    "hero":  98,   # 棕发勇者 (正面朝下, 顶视角适用)
    "mob1":  87,   # 骷髅怪
    "mob2":  108,  # 绿史莱姆
    "mob3":  110,  # 红怪
    "boss":  123,  # 牛头 (当首领)
    "tile_brick": 14,   # 砖墙  → 可炸
    "tile_stone": 33,   # 灰色石块 → 硬墙 (38 其实是地板木纹, 选错过)
    "tile_crate": 66,   # 木桶/木箱
    "tile_water": 0,    # 深色泥地 → 当"水/坑" (必须抖动: 原图整块暗色, 直接阈值会变成纯黑像墙)
    "tile_stair": 80,   # 木梯  → 下层入口
    "item1": 101,  # 金币      (普通)
    "item2": 114,  # 绿药水    (精良)
    "item3": 116,  # 蓝药水    (稀有)
}

def kenney_bits(idx, size, target=0.34):
    """Kenney 图块: 原生 16x16 → 直接阈值, **完全不缩放**"""
    im = Image.open(os.path.join(KENNEY, "tile_%04d.png" % idx)).convert("RGBA")
    w, h = im.size
    g = Image.new("L", (w, h), 255); m = Image.new("L", (w, h), 0)
    gp, mp, px = g.load(), m.load(), im.load()
    for y in range(h):
        for x in range(w):
            r, gg, b, al = px[x, y]
            if al < 110: continue
            L = (r*299 + gg*587 + b*114)//1000
            gp[x, y] = L; mp[x, y] = 255
    vals = sorted(gp[x, y] for y in range(h) for x in range(w) if mp[x, y] > 0)
    if not vals:
        return size, size, [[0]*size for _ in range(size)]
    th = vals[min(len(vals)-1, int(len(vals)*target))]
    rows = [[1 if (mp[x, y] > 0 and gp[x, y] <= th) else 0 for x in range(size)] for y in range(size)]
    return size, size, rows

def kenney_dither(idx, size, target=0.30):
    """Kenney 图块 → Bayer 抖动 (给"要材质感"的地形用)"""
    im = Image.open(os.path.join(KENNEY, "tile_%04d.png" % idx)).convert("RGBA")
    w, h = im.size
    rows = [[0]*size for _ in range(size)]
    px = im.load()
    for y in range(min(h, size)):
        for x in range(min(w, size)):
            r, g, b, a = px[x, y]
            if a < 110: continue
            L = (r*299 + g*587 + b*114)//1000
            if L < 255 * (BAYER4[y % 4][x % 4] + 0.5) / 16.0 * 0.9:
                rows[y][x] = 1
    return size, size, rows

def ring_bits(size, r_out=7.4, r_in=5.4, hi=True):
    """程序生成: 泡泡 (空心圆环 + 高光), 原创无版权"""
    rows = [[0]*size for _ in range(size)]
    c = (size - 1) / 2.0
    for y in range(size):
        for x in range(size):
            d = ((x-c)**2 + (y-c)**2) ** 0.5
            if r_in <= d <= r_out: rows[y][x] = 1
    if hi:
        for y in range(2, 6):
            for x in range(3, 7):
                if (x-4)**2 + (y-4)**2 <= 3: rows[y][x] = 0   # 高光留白
    return size, size, rows

def burst_bits(size, rad, spokes=8, thick=1.6):
    """程序生成: 爆炸放射 (原创)"""
    rows = [[0]*size for _ in range(size)]
    c = (size - 1) / 2.0
    import math as _m
    for k in range(spokes):
        a = k * 2 * _m.pi / spokes
        for t in range(2, int(rad) + 1):
            for w in (0, 0.5, -0.5):
                for s2 in (-thick/2, 0, thick/2):
                    x = int(round(c + (t + s2) * _m.cos(a) + w * _m.cos(a + 1.5708)))
                    y = int(round(c + (t + s2) * _m.sin(a) + w * _m.sin(a + 1.5708)))
                    if 0 <= x < size and 0 <= y < size: rows[y][x] = 1
    for y in range(size):
        for x in range(size):
            if (x-c)**2 + (y-c)**2 <= 4: rows[y][x] = 1          # 中心实心
    return size, size, rows

BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def lum(px):
    r, g, b = px[0], px[1], px[2]
    return (r * 299 + g * 587 + b * 114) // 1000


def load(p):
    im = Image.open(os.path.join(REF, p))
    return im.convert("RGBA"), im.size


def cell(im, col, row, n):
    """从 n×n 网格里取一格"""
    w, h = im.size
    cw, ch = w // n, h // n
    return im.crop((col * cw, row * ch, (col + 1) * cw, (row + 1) * ch))


def bits(im, size, mode="auto", target=0.32, crop_subject=True, white_cut=246):
    """→ (w, h, rows);  1 = 画黑, 0 = 不画

    三次迭代后的最终算法 (前两次的失败都记在这里, 免得后人重走):
      ① "先缩到 16px 再按 128 二值化" → 细描边插值成灰 → 精灵几乎空白;
      ② "按固定亮度阈值 (L<120) 画黑" → 原素材是**光泽感彩色插画**, 角色/爆炸的中暗部一大片
         L<120 → 糊成实心黑块 (预览两次都很丑)。
      ★ 正解 = **自适应阈值 (按墨点占比)**:
         先把"全分辨率亮度图 + 透明度掩码"一起 BOX 缩到目标尺寸, 拿到每格的平均暗度;
         再取"第 target 分位"的暗度当阈值 → **不管原图多亮多暗, 每张精灵都恰好 target 比例的黑点**。
         于是轮廓保留、不会糊块、也不会空。target≈0.30 在 1bpp 上最像"线稿+阴影"。
      mode="dither" 仍保留给地形 (要的是"材质纹理"而不是轮廓)。
    """
    im = im.convert("RGBA")
    w0, h0 = im.size
    g = Image.new("L", (w0, h0), 255)
    m = Image.new("L", (w0, h0), 0)
    gp, mp, px = g.load(), m.load(), im.load()
    for y in range(h0):
        for x in range(w0):
            r, gg, b, al = px[x, y]
            if al < 110:
                continue
            L = (r * 299 + gg * 587 + b * 114) // 1000
            if L >= white_cut:
                continue
            gp[x, y] = L
            mp[x, y] = 255
    if mode == "dither":
        bw = Image.new("L", (w0, h0), 0)
        bp = bw.load()
        for y in range(h0):
            for x in range(w0):
                if mp[x, y] and gp[x, y] < 255 * (BAYER4[y % 4][x % 4] + 0.5) / 16.0:
                    bp[x, y] = 255
        if crop_subject:
            bb = bw.getbbox()
            if bb:
                bw = bw.crop(bb)
        small = bw.resize((size, size), Image.Resampling.BOX)
        return size, size, [[1 if small.getpixel((x, y)) > 40 else 0 for x in range(size)]
                            for y in range(size)]
    # ---- 自适应阈值 ----
    if crop_subject:
        bb = m.getbbox()
        if bb:
            g = g.crop(bb); m = m.crop(bb)
    gs = g.resize((size, size), Image.Resampling.BOX)
    ms = m.resize((size, size), Image.Resampling.BOX)
    vals = []
    for y in range(size):
        for x in range(size):
            if ms.getpixel((x, y)) > 90:
                vals.append(gs.getpixel((x, y)))
    if not vals:
        return size, size, [[0] * size for _ in range(size)]
    vals.sort()
    k = min(len(vals) - 1, max(0, int(len(vals) * target)))
    th = vals[k]
    return size, size, [[1 if (ms.getpixel((x, y)) > 90 and gs.getpixel((x, y)) <= th) else 0
                         for x in range(size)] for y in range(size)]


def to_hex(w, h, rows):
    """1bpp MSB 在前, 行按 (w+7)/8 字节对齐 → hex 文本"""
    bpr = (w + 7) // 8
    buf = bytearray()
    for y in range(h):
        for b in range(bpr):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                if x < w and rows[y][x]:
                    byte |= 1 << (7 - bit)
            buf.append(byte)
    return buf.hex()


def tile_bands(im):
    """地形带自动分行: 找出颜色突变的行边界 → 返回 4 条带的 y 区间"""
    w, h = im.size
    key = []
    for y in range(h):
        # 采样该行 8 个点做指纹
        s = []
        for k in range(8):
            x = min(w - 1, int((k + 0.5) * w / 8))
            r, g, b, a = im.convert("RGBA").getpixel((x, y))
            s.append((r // 32, g // 32, b // 32))
        key.append(s)
    bounds = [0]
    for y in range(1, h):
        if key[y] != key[y - 1]:
            if y - bounds[-1] > h // 8:      # 忽略细碎噪声
                bounds.append(y)
    bounds.append(h)
    return bounds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview")
    a = ap.parse_args()

    if not os.path.isdir(REF):
        sys.exit("✗ 找不到参考素材目录 %s (先 git clone qqtang-web)" % REF)

    art = {}
    # ---- Kenney CC0 图块 (1:1, 不缩放) ----
    for name, idx in KENNEY_IDX.items():
        sz = TILE
        if name == "tile_water":
            art[name] = kenney_dither(idx, sz, target=0.30)   # 水面用抖动 → 有纹理感, 不会糊成黑墙
        else:
            art[name] = kenney_bits(idx, sz, target=0.30 if name.startswith("tile") else 0.34)
    # ---- 程序生成的泡泡 / 爆炸 (CC0 包里没有, 自己画 = 原创) ----
    art["bubble"] = ring_bits(ACT)
    art["boom1"] = burst_bits(TILE, 6, 8, 1.4)
    art["boom2"] = burst_bits(TILE, 7, 10, 1.6)
    art["boom3"] = burst_bits(TILE, 8, 12, 1.8)

    # ---- 落盘的 Lua 数据段 ----
    lines = [BEG,
             "-- 由 tools/qqart_gen.py 生成 (素材来自 qqtang-web), **不要手改**.",
             "-- 每项 = {w=,h=,d=\"hex\"}: app.bmp(x,y,w,h,d), 位=1 画黑, 0 透明.",
             "local ART = {"]
    for k in sorted(art.keys()):
        w, h, rows = art[k]
        lines.append('    %s = { w = %d, h = %d, d = "%s" },' % (k, w, h, to_hex(w, h, rows)))
    lines += ["}", END, ""]
    block = "\n".join(lines)

    src = open(GAME, encoding="utf-8").read()
    if BEG in src and END in src:
        src = re.sub(re.escape(BEG) + r".*?" + re.escape(END) + r"\n?", lambda _m: block, src, flags=re.S)
    else:
        sys.exit("✗ qqtang.lua 里没有 %s / %s 标记 (先补标记再跑)" % (BEG, END))
    open(GAME, "w", encoding="utf-8").write(src)

    print("✓ 已注入 %d 个精灵到 %s" % (len(art), os.path.basename(GAME)))
    for k in sorted(art.keys()):
        w, h, _ = art[k]
        print("   %-10s %dx%d  %d B" % (k, w, h, (w + 7) // 8 * h))

    if a.preview:
        cols = 6
        rows = (len(art) + cols - 1) // cols
        S = 64
        out = Image.new("L", (cols * S, rows * S), 255)
        for i, k in enumerate(sorted(art.keys())):
            w, h, rr = art[k]
            tile = Image.new("L", (w, h), 255)
            for y in range(h):
                for x in range(w):
                    if rr[y][x]:
                        tile.putpixel((x, y), 0)
            tile = tile.resize((S - 8, S - 8), Image.NEAREST)
            out.paste(tile, ((i % cols) * S + 4, (i // cols) * S + 4))
        out = out.resize((out.width * 2, out.height * 2), Image.NEAREST)
        out.save(a.preview)
        print("   预览 →", a.preview)


if __name__ == "__main__":
    main()
