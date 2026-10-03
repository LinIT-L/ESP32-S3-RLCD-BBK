#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""wuxia_pack.py — 把《英雄坛说》原版素材打包成固件能直接读的 1bpp BMP。

产出 (写到 apps/lua/wuxia/, 设备上路径 = /store/wuxia/)
------------------------------------------------------
  spr.bmp     32×32 精灵图集, **格号 = 素材 id** (0..511), 32 列 × 16 行 = 1024×512
              → 设备端 `app.img.blit(sprId, id*32 % 1024, id/32*32, …)` 直接取,
                不需要额外的 id→位置映射表 (这是刻意选的: 少一层查表就少一处出错)。
  player.bmp  玩家图集 32×48, 格号 = gender*12 + dir*3 + frame (8 列 × 3 行 = 256×144)
  map<N>.bmp  63 张地图整图, 原样搬运 (网站就是每张图一个预渲染 BMP, w*32 × h*32)
  以及 sprite_map.lua: 把"哪些 id 真的有图"告诉脚本 (空格子不必浪费 blit 调用)

合成规则 (必须与网站一致, 否则画出来是黑块或空白)
--------------------------------------------------
网站的 ensureTileSprite:  draw = mask ? mask位==1 : res位==0
                          ink  = res位==0
映射到我们的 1 位(1=墨, 0=透明)世界, 就是:
        **ink = ¬res ∩ (mask ? mask : 全1)**
   ⚠ 我第一版写成 mask ∩ res, 结果人物只剩脸上几条线、头是空的 —— 因为这两张 BMP 的
     调色板是 palette[0]=黑 / palette[1]=白, res 的 0 才是黑。

BMP 写出说明
------------
 1bpp BMP 的位=1 对应调色板[1]=白。而我们的固件加载器把位取反成"1=墨",
 所以这里写文件时要把 ink **取反**再写 (ink 存成 0)。两边约定对称, 读回来才对。

用法
----
    python3 tools/wuxia_pack.py --src /tmp/gmud_art --data /tmp --out apps/lua/wuxia
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wuxia_art_gen import load_1bpp          # 复用同一个 BMP 解码器

SPR_CELL = 32
SPR_MAX = 512
SPR_COLS = 32
PL_CELL_W, PL_CELL_H = 32, 48
PL_COLS = 8


def ink_of(res_rows, mask_rows, w, h):
    """ink = ¬res ∩ (mask ? mask : 全1)  —— 见文件头说明。"""
    out = []
    for y in range(h):
        r = res_rows[y]
        m = mask_rows[y] if mask_rows else None
        out.append([1 if ((not r[x]) and (m is None or m[x])) else 0 for x in range(w)])
    return out


def blank(w, h):
    return [[0] * w for _ in range(h)]


def paste(dst, dw, dh, src, sw, sh, ox, oy):
    for y in range(min(sh, dh - oy)):
        if oy + y < 0:
            continue
        for x in range(min(sw, dw - ox)):
            if ox + x < 0:
                continue
            dst[oy + y][ox + x] = 1 if src[y][x] else 0


def write_bmp1(path, rows, w, h):
    """写 1bpp BMP (自下而上, 位=1 表示白, 所以 ink 写成位=0)。"""
    stride = ((w + 31) // 32) * 4
    pix = bytearray()
    for y in range(h - 1, -1, -1):                    # BMP 正高度 = 自下而上
        line = bytearray([0xFF] * stride)             # 默认白
        for x in range(w):
            if rows[y][x]:                            # ink → 位清 0 (黑)
                line[x >> 3] &= (~(0x80 >> (x & 7))) & 0xFF
        pix += line
    off = 14 + 40 + 8
    fh = b"BM" + struct.pack("<IHHI", off + len(pix), 0, 0, off)
    dh = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 1, 0, len(pix), 2835, 2835, 2, 2)
    pal = bytes((0, 0, 0, 0)) + bytes((255, 255, 255, 0))   # 调色板[0]=黑 [1]=白
    open(path, "wb").write(fh + dh + pal + bytes(pix))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="/tmp/gmud_art")
    ap.add_argument("--data", default="/tmp", help="maps.json 所在目录")
    ap.add_argument("--out", default="apps/lua/wuxia")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    # ---------- 1. 精灵图集 (格号 = id) ----------
    AW, AH = SPR_COLS * SPR_CELL, (SPR_MAX // SPR_COLS) * SPR_CELL
    atlas = blank(AW, AH)
    matlas = blank(AW, AH)
    have = []
    blank_ids = []
    for i in range(SPR_MAX):
        rp = os.path.join(a.src, "img/res/resmap%03d.bmp" % i)
        if not os.path.exists(rp):
            continue
        mp = os.path.join(a.src, "img/mask/resmapmask%03d.bmp" % i)
        w, h, res = load_1bpp(rp)
        mask = None
        if os.path.exists(mp):
            _, _, mask = load_1bpp(mp)
        ink = ink_of(res, mask, w, h)
        # 底版 = 这张精灵的"可见区" (mask)。画的时候先按底版刷白、再压墨点,
        # 人物背上就不会透出地图 —— 原站 makeNpcSprite 就是这么做的 (mask 内黑白都画)。
        region = mask if mask else ink
        if not any(any(r) for r in ink):
            blank_ids.append(i)
            continue
        cx, cy = (i % SPR_COLS) * SPR_CELL, (i // SPR_COLS) * SPR_CELL
        paste(atlas, AW, AH, ink, w, h, cx, cy)
        paste(matlas, AW, AH, region, w, h, cx, cy)
        have.append(i)
    write_bmp1(os.path.join(a.out, "spr.bmp"), atlas, AW, AH)
    write_bmp1(os.path.join(a.out, "sprm.bmp"), matlas, AW, AH)
    print("spr.bmp   %dx%d  %d 格有图 (空白格 %d 个已剔除)  (+ sprm.bmp 底版)" % (AW, AH, len(have), len(blank_ids)))

    # ---------- 2. 玩家图集 ----------
    PW_, PH_ = PL_COLS * PL_CELL_W, 3 * PL_CELL_H
    patlas = blank(PW_, PH_)
    pmatlas = blank(PW_, PH_)
    pmissing = []
    for gi, g in enumerate(("male", "female")):
        for d in range(4):
            for f in range(3):
                base = os.path.join(a.src, "player", g, "d%d_f%d" % (d, f))
                rp, mp = base + "_res.bmp", base + "_mask.bmp"
                if not (os.path.exists(rp) and os.path.exists(mp)):
                    pmissing.append("%s d%d f%d" % (g, d, f))
                    continue
                w, h, res = load_1bpp(rp)
                _, _, mask = load_1bpp(mp)
                ink = ink_of(res, mask, w, h)
                cell = gi * 12 + d * 3 + f
                paste(patlas, PW_, PH_, ink, w, h,
                      (cell % PL_COLS) * PL_CELL_W, (cell // PL_COLS) * PL_CELL_H)
                paste(pmatlas, PW_, PH_, mask, w, h,
                      (cell % PL_COLS) * PL_CELL_W, (cell // PL_COLS) * PL_CELL_H)
    write_bmp1(os.path.join(a.out, "player.bmp"), patlas, PW_, PH_)
    write_bmp1(os.path.join(a.out, "playerm.bmp"), pmatlas, PW_, PH_)
    print("player.bmp %dx%d  24 格%s  (+ playerm.bmp 底版)" % (PW_, PH_, ("  缺 " + str(pmissing)) if pmissing else ""))

    # ---------- 3. 地图 ----------
    maps = json.load(open(os.path.join(a.data, "maps.json"), encoding="utf-8"))["maps"]
    mp_ok, mp_missing, total = 0, [], 0
    meta = []
    for m in maps:
        i = m["id"]
        s = os.path.join(a.src, "map", "map%d.bmp" % i)
        if not os.path.exists(s):
            mp_missing.append(i)
            continue
        d = open(s, "rb").read()
        open(os.path.join(a.out, "map%d.bmp" % i), "wb").write(d)
        total += len(d)
        mp_ok += 1
        meta.append((i, m["name"], m["w"], m["h"]))
    print("地图 %d/%d 张, %.1f MB%s" % (mp_ok, len(maps), total / 1048576.0,
          ("  缺 " + str(mp_missing)) if mp_missing else ""))

    # ---------- 4. 元信息 (给 Lua 用) ----------
    L = ["-- wuxia/map.lua — 地图与精灵元信息 (由 tools/wuxia_pack.py 生成, 不要手改)",
         "-- 设备端路径前缀: /store/wuxia/", "",
         "return {",
         "  dir      = \"/store/wuxia/\",",
         "  spr_w    = %d, spr_h = %d, spr_cols = %d," % (SPR_CELL, SPR_CELL, SPR_COLS),
         "  pl_w     = %d, pl_h = %d, pl_cols = %d," % (PL_CELL_W, PL_CELL_H, PL_COLS),
         "  spr_has  = {" + ",".join("[%d]=1" % i for i in have) + "},",
         "  maps = {"]
    for i, name, w, h in sorted(meta):
        L.append('    [%d] = { name = "%s", w = %d, h = %d },' % (i, name, w, h))
    L += ["  },", "}", ""]
    open(os.path.join(a.out, "map.lua"), "w", encoding="utf-8").write("\n".join(L))
    print("map.lua  %d 张地图元信息" % len(meta))


if __name__ == "__main__":
    main()
