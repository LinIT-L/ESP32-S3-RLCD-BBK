#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""wuxia_art_gen.py — 把《英雄坛说》(gmud.online) 的**原版素材**转成 Lua 位图表。

为什么单独一个工具
------------------
用户要求「一比一复刻这个网站的画面」，且明确禁止我自己画。所以这里做的只有一件事：
把站点上真实的 .bmp / 内联位图**原样搬进固件**，不做缩放、不做重绘、不做美化。

素材来源 (与网站一致, 已全部下载到 /tmp/gmud_art)
--------------------------------------------------
  img/menu/arrow.bmp        8x12  ▶ 菜单光标 (选中行前面那个三角)
  img/menu/finger.bmp      16x16  👆 手指指针 (文曲星经典)
  img/menu/arrow_dual.bmp  16x16  ◀▶ 左右可切
  img/menu/box.bmp          8x12  实心方块 (选中高亮块)
  img/menu/box1.bmp         8x12  空心方块
  img/menu/check.bmp        8x12  勾选 (实)
  img/menu/check1.bmp       8x12  勾选 (空)
  img/menu/radio.bmp        8x12  ⊙ 单选选中
  img/menu/radio1.bmp       8x12  ○ 单选未选
  img/menu/icon.bmp         8x12  实心小方 (战斗选择)
  img/menu/icon1.bmp        8x12  空心小方
  img/menu/pagedown.bmp     8x4   ▾▾ 可下翻
  img/menu/digit.bmp       48x5   数字条 (原版 5px 高窄字模)
  img/menu/panyan_ball.bmp  6x6   ⊙ 角标小球
  img/menu/panyan_arrow.bmp 64x16  ↑↓←→ + 确认 软键提示条 (原版底部盘眼)
  img/menu/panyan_arrow_flash.bmp 64x16 同上(高亮态)
  img/menu/exp_ball.bmp    16x16  经验球
  img/menu/ink.bmp          6x6  地形墨点 (原版地图纹理)
  img/menu/ink2.bmp         3x3  同上(细)
  img/player/{male,female}_head_{res,mask}.bmp  32x32 头像 (mask=轮廓, res=内容)
  内联: IMG_HP_BITS 16x8 (气血) / IMG_FP_BITS 16x8 (内力) / IMG_VS_BITS 24x8 (对决)

输出
----
把生成的 `-- @@WUXIA-ART-BEGIN@@ … -- @@WUXIA-ART-END@@` 块替换进 apps/lua/wuxia.lua
(或 ~/Desktop/LinTOS_Lua应用/src/wuxia.lua) 的 `local ART = { ... }` 处。

位图格式 = `app.bmp(x,y,w,h,hex)` 的 hex: 每行 `(w+7)//8` 字节, **MSB 先行, 1=黑, 0=透明**。

用法
----
    python3 tools/wuxia_art_gen.py --src /tmp/gmud_art --out /tmp/wuxia_art.lua
    python3 tools/wuxia_art_gen.py --src /tmp/gmud_art --inject ~/Desktop/.../wuxia.lua
"""
import argparse
import os
import struct
import sys

# ---- 网站上内联在 JS 里的三个 HUD 位图 (原样抄, 16x8 / 16x8 / 24x8) ----
IMG_HP_BITS = [0xff, 0xf0, 0xb6, 0x10, 0xb6, 0xd0, 0xb6, 0xd0,
               0x86, 0x10, 0xb6, 0xf0, 0xb6, 0xf0, 0xff, 0xf0]
IMG_FP_BITS = [0xff, 0xf0, 0x86, 0x10, 0xbe, 0xd0, 0xbe, 0xd0,
               0x8e, 0x10, 0xbe, 0xf0, 0xbe, 0xf0, 0xff, 0xf0]
IMG_VS_BITS = [0xf3, 0x81, 0xc0, 0x61, 0x03, 0x20, 0x61, 0x03,
               0x00, 0x32, 0x01, 0x80, 0x32, 0x00, 0xc0, 0x1c,
               0x00, 0x60, 0x1c, 0x02, 0x60, 0x08, 0x01, 0xc0]

BEGIN = "-- @@WUXIA-ART-BEGIN@@"
END = "-- @@WUXIA-ART-END@@"


# ------------------------------------------------------------------ BMP 解码
def load_1bpp(path):
    """读 1bpp/24bpp BMP → (w, h, rows[list[list[int]]])。1=黑, 0=透明。"""
    d = open(path, "rb").read()
    if d[:2] != b"BM":
        raise ValueError("不是 BMP: %s" % path)
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    flip = h > 0                     # 正高度: 自下而上存储
    h = abs(h)
    rows = [[0] * w for _ in range(h)]
    if bpp == 1:
        stride = ((w + 31) // 32) * 4
        for y in range(h):
            base = off + y * stride
            for x in range(w):
                bit = (d[base + (x >> 3)] >> (7 - (x & 7))) & 1
                rows[(h - 1 - y) if flip else y][x] = bit
    elif bpp == 24:
        stride = ((w * 3 + 3) // 4) * 4
        for y in range(h):
            base = off + y * stride
            for x in range(w):
                b, g, r = d[base + x * 3], d[base + x * 3 + 1], d[base + x * 3 + 2]
                lum = 0.299 * r + 0.587 * g + 0.114 * b
                rows[(h - 1 - y) if flip else y][x] = 1 if lum < 128 else 0
    else:
        raise ValueError("暂不支持 %d bpp: %s" % (bpp, path))
    return w, h, rows


def rows_of_bits(bits, w, h):
    """把按行打包的 byte 数组 + 宽度拆成 rows。"""
    per = (w + 7) // 8
    rows = []
    for y in range(h):
        r = []
        for x in range(w):
            r.append((bits[y * per + (x >> 3)] >> (7 - (x & 7))) & 1)
        rows.append(r)
    return rows


def pack_hex(rows, w, h):
    """rows → app.bmp 要的 hex (每行 (w+7)//8 字节, MSB 先行)。"""
    per = (w + 7) // 8
    out = []
    for y in range(h):
        for b in range(per):
            v = 0
            for k in range(8):
                x = b * 8 + k
                if x < w and rows[y][x]:
                    v |= 0x80 >> k
            out.append("%02x" % v)
    return "".join(out)


def is_blank(rows):
    return not any(any(r) for r in rows)


def crop(rows, w, h):
    """裁掉四周全透明的行列 (原版素材普遍有留白, 裁掉才好定位)。返回 (x0,y0,w,h,rows)。"""
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            if rows[y][x]:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    if x1 < 0:
        return 0, 0, w, h, rows
    return x0, y0, x1 - x0 + 1, y1 - y0 + 1, [r[x0:x1 + 1] for r in rows[y0:y1 + 1]]


# ------------------------------------------------------------------ 组装
# 名字 → (相对路径, 是否裁剪留白)
MENU = {
    "arrow":     ("img/menu/arrow.bmp", True),
    "finger":    ("img/menu/finger.bmp", True),
    "arrowdual": ("img/menu/arrow_dual.bmp", True),
    "box":       ("img/menu/box.bmp", True),
    "box1":      ("img/menu/box1.bmp", True),
    "check":     ("img/menu/check.bmp", True),
    "check1":    ("img/menu/check1.bmp", True),
    "radio":     ("img/menu/radio.bmp", True),
    "radio1":    ("img/menu/radio1.bmp", True),
    "icon":      ("img/menu/icon.bmp", True),
    "icon1":     ("img/menu/icon1.bmp", True),
    "pagedown":  ("img/menu/pagedown.bmp", True),
    "digit":     ("img/menu/digit.bmp", True),
    "ball":      ("img/menu/panyan_ball.bmp", True),
    "panyan":    ("img/menu/panyan_arrow.bmp", False),
    "panyanf":   ("img/menu/panyan_arrow_flash.bmp", False),
    "expball":   ("img/menu/exp_ball.bmp", False),
    "ink":       ("img/menu/ink.bmp", False),
    "ink2":      ("img/menu/ink2.bmp", False),
    "mhead":     ("img/player/male_head_mask.bmp", False),
    "mface":     ("img/player/male_head_res.bmp", False),
    "fhead":     ("img/player/female_head_mask.bmp", False),
    "fface":     ("img/player/female_head_res.bmp", False),
}


def and_rows(a, b, w, h):
    """两张同尺寸位图取交集。"""
    return [[1 if (a[y][x] and b[y][x]) else 0 for x in range(w)] for y in range(h)]


def head_ink(mask_rows, res_rows, w, h):
    """头像线稿 = mask ∩ ¬res。

    ⚠ 别写反: 网站是 `res.palette[res.pixels[y][x]]`, 而这两张 BMP 的调色板是
      palette[0]=黑 / palette[1]=白 —— 也就是 **res 的 0 是黑、1 是白**。
      mask 只圈定"可见区(头部轮廓)", 所以纸上留下的墨 = mask 之内的 res==0 处。
      第一版我写成 mask ∩ res, 画出来只剩几条脸上的线、头是空的 (已修正)。
    """
    return [[1 if (mask_rows[y][x] and not res_rows[y][x]) else 0 for x in range(w)]
            for y in range(h)]


def build(src):
    # ⚠ 极性修正 (2026-10-03, linit 实测"进入界面左右头像颜色反了")：
    #   male/female_head_res.bmp 的调色板是 palette[0]=黑 / palette[1]=白，
    #   而 load_1bpp 取的是**调色板索引** ⇒ `res` 原样读出来是"1 = 白"。
    #   但 pack_hex / app.bmp 的语义是"**1 = 黑(墨)**, 0 = 透明" —— 两者相反，
    #   原样存下去会在白底反射屏上画成"黑底白图"(整块反相)。
    #   ⇒ 这两张必须取 ¬res 再存 (= 与 head_ink 的 `mask ∩ ¬res` 同一套极性)。
    #   `mhead/fhead`(mask) 与 `mport/fport`(mask ∩ ¬res) 本来就对，不动。
    INVERT = {"mface", "fface"}
    art, meta = {}, []
    for name, (rel, do_crop) in MENU.items():
        p = os.path.join(src, rel)
        if not os.path.exists(p):
            print("  ⚠ 缺素材, 跳过: %s" % rel, file=sys.stderr)
            continue
        w, h, rows = load_1bpp(p)
        if name in INVERT:
            rows = [[1 - v for v in row] for row in rows]
        ox = oy = 0
        if do_crop and not is_blank(rows):
            ox, oy, w, h, rows = crop(rows, w, h)
        art[name] = (w, h, pack_hex(rows, w, h))
        meta.append((name, rel, w, h, ox, oy))

    # 头像: mask 只给"可见区", res 给"墨色", 二者相与才是能画的线稿 (单画 res 是整块黑)。
    for tag, hd, fc in (("m", "mhead", "mface"), ("f", "fhead", "fface")):
        if hd in art and fc in art:
            w, h = art[hd][0], art[hd][1]
            _, _, mrows = load_1bpp(os.path.join(src, MENU[hd][0]))
            _, _, rrows = load_1bpp(os.path.join(src, MENU[fc][0]))
            comp = head_ink(mrows, rrows, w, h)
            art[tag + "port"] = (w, h, pack_hex(comp, w, h))
            meta.append((tag + "port", "mask ∩ ¬res (合成线稿)", w, h, 0, 0))

    # HUD 内联位图
    art["hp"] = (16, 8, pack_hex(rows_of_bits(IMG_HP_BITS, 16, 8), 16, 8))
    art["fp"] = (16, 8, pack_hex(rows_of_bits(IMG_FP_BITS, 16, 8), 16, 8))
    art["vs"] = (24, 8, pack_hex(rows_of_bits(IMG_VS_BITS, 24, 8), 24, 8))
    meta += [("hp", "内联 IMG_HP_BITS", 16, 8, 0, 0),
             ("fp", "内联 IMG_FP_BITS", 16, 8, 0, 0),
             ("vs", "内联 IMG_VS_BITS", 24, 8, 0, 0)]
    return art, meta


def emit_lua(art):
    """生成 Lua 表:  {w=,h=,d=}  — 用 app.bmp(x,y,w,h,d) 直接贴。"""
    L = [BEGIN,
         "-- 《英雄坛说》原版素材直搬 (来自 gmud.online/img/**/*.bmp + 内联 HUD 位图)。",
         "-- 由 tools/wuxia_art_gen.py 生成, **不要手改**; 素材改了重跑生成器。",
         "-- 格式: ART.<名> = {w, h, \"hex\"}; hex 为行优先 MSB, 1=黑 0=透明 → app.bmp(x,y,w,h,d)。",
         "local ART = {"]
    for k in sorted(art):
        w, h, d = art[k]
        L.append('  %-9s = { %2d, %2d, "%s" },' % (k, w, h, d))
    L += ["}", END]
    return "\n".join(L)


def inject(target, block):
    txt = open(target, encoding="utf-8").read()
    i, j = txt.find(BEGIN), txt.find(END)
    if i < 0 or j < 0:
        raise SystemExit("目标文件里没有 %s / %s 标记, 无法注入" % (BEGIN, END))
    new = txt[:i] + block + txt[j + len(END):]
    open(target, "w", encoding="utf-8").write(new)
    return len(block) - (j + len(END) - i)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="/tmp/gmud_art", help="素材根目录 (含 img/)")
    ap.add_argument("--out", default="/tmp/wuxia_art.lua", help="输出 Lua 片段")
    ap.add_argument("--inject", help="直接替换目标 wuxia.lua 里的 ART 块")
    a = ap.parse_args()

    art, meta = build(a.src)
    print("素材 %d 项:" % len(meta))
    for name, rel, w, h, ox, oy in meta:
        print("  %-9s %-42s %2dx%-2d  %s" % (name, rel, w, h,
              ("原图裁到 (x%d,y%d)" % (ox, oy)) if (ox or oy) else ""))
    block = emit_lua(art)
    if a.inject:
        delta = inject(a.inject, block)
        print("\n✓ 已注入 %s (净变化 %+d 字节)" % (a.inject, delta))
    else:
        open(a.out, "w", encoding="utf-8").write(block + "\n")
        print("\n✓ 已写出 %s (%d 字节)" % (a.out, len(block)))


if __name__ == "__main__":
    main()
