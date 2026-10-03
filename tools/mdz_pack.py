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
    """道具图标 → fs×fs 的墨点。三步，缺一步就会得到"空白格"或"只剩一角"：

    ① **先裁到 alpha 边界** —— 官方图里图标往往只占一小块（武器帧 39 尤其明显：
       32×32 的格里只有一条 20×6 的刀身）。不裁就居中缩放，结果缩得比蚂蚁还小，
       看起来像"没画出来"。
    ② 等比缩放进 (fs-2*pad) 的框：**放大用 NEAREST**（像素画不插值），**缩小用 LANCZOS**。
       ⛔ 旧版只做了 `max(1, ...)` 的整数放大 ⇒ 大图（如 128×544 的武器表）s 恒为 1,
       于是把图片**左上角 32×32 直接糊进格子里** —— 这就是"图标全是垃圾/空白"的原因。
    ③ 居中贴进 fs 画布，再按"alpha 不透明 且 亮度 < 150"落墨。
    """
    im = im.convert("RGBA")
    bbox = im.getchannel("A").getbbox()                 # ① 去掉四周全透明的边
    if bbox:
        im = im.crop(bbox)
    w, h = im.size
    inner = max(1, fs - pad * 2)
    if w > inner or h > inner:                          # ② 大图 → 等比缩
        sc = min(inner / w, inner / h)
        im = im.resize((max(1, round(w * sc)), max(1, round(h * sc))), Image.LANCZOS)
    else:                                               # 小图 → 整数倍最近邻放大
        sc = max(1, min(inner // max(1, w), inner // max(1, h)))
        im = im.resize((w * sc, h * sc), Image.NEAREST)
    canvas = Image.new("RGBA", (fs, fs), (0, 0, 0, 0))
    canvas.paste(im, ((fs - im.size[0]) // 2, (fs - im.size[1]) // 2))
    px = canvas.load()
    # ⚠ 阈值必须能自愈: 有的图标本体是**浅色**的(子弹是黄铜色, 亮度 ~170),
    #   固定 150 会把整张判成"白" ⇒ 出来一个**空图标**(6/7 号就中过)。
    #   所以逐档放宽, 一旦有墨就停; 全档都空才返回。
    bits = None
    for thresh in (150, 180, 205):
        bits = [[0] * fs for _ in range(fs)]
        for y in range(fs):
            for x in range(fs):
                r, g, b, a = px[x, y]
                if a < 110:
                    continue
                if 0.299 * r + 0.587 * g + 0.114 * b < thresh:
                    bits[y][x] = 1
        if any(any(row) for row in bits):
            break
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


def make_tile(kind, T=20, v=0):
    """地形瓦片。v = 变体号。

    ⛔ 为什么必须给变体: 同一种地形只有一张图时, 20px 的小草簇会在屏幕上**整齐排成网格**,
       整片地图看起来"全是一个样"(2026-10-03 实测就踩了, 用户看到的是一片整齐的点阵)。
       每个地形给 2~4 个变体, 由 Lua 侧**确定性哈希**挑(不能用随机: 静态层会重建, 随机会满屏闪)。

    区分地形靠"**线宽/线密**", 不靠"谁更黑":
      草地=稀疏草簇(8~12px 一个) / 路面=**密实细颗粒**(像沥青, 明显更深)
      水=横向波纹线 / 沙=中等散点 / 墙=粗框砖缝 / 室内地板=板缝线 / 树=实心树冠+树干
    """
    g = blank(T, T)
    if kind == "grass":                                    # 4 变体: 草簇位置疏密不同
        sets = (
            ((3, 5), (12, 3), (7, 13), (16, 10), (2, 16)),
            ((6, 8), (15, 6), (4, 16), (11, 12)),
            ((9, 4), (2, 10), (14, 15), (18, 8), (7, 17)),
            ((5, 6), (13, 11), (1, 14), (17, 17), (10, 15), (8, 2)),
        )
        for (x, y) in sets[v % len(sets)]:
            if 0 <= x < T and 0 <= y < T:
                g[y][x] = 1
            if 0 <= x - 1 and 0 <= y - 1:
                g[y - 1][x - 1] = 1
            if x + 1 < T and 0 <= y - 1:
                g[y - 1][x + 1] = 1
    elif kind == "dirt":                                   # 土: 中等散点
        for (x, y) in ((4, 4), (13, 7), (6, 14), (15, 15), (9, 9), (2, 11), (17, 3)):
            g[y][x] = 1
    elif kind == "tree":                                   # 3 变体: 树冠大小/位置
        r  = (7, 6, 8)[v % 3]
        cx = (9, 10, 8)[v % 3]
        for k in range(-r, r + 1):
            yy = 9 - int((r * r - k * k) ** 0.5)
            if 0 <= yy < T and 0 <= cx + k < T:
                g[yy][cx + k] = 1
        for i in range(4, 10):
            hline(g, cx - 1 - (9 - i) // 3, cx + 1 + (9 - i) // 3, i)
        vline(g, cx, 13, 18); vline(g, cx + 1, 13, 18)
    elif kind == "water":                                  # 波纹: 线最密
        for y in (3, 6, 9, 12, 15, 18):
            hline(g, 2, 17, y)
            g[y - 1][3] = 1; g[y - 1][16] = 1; g[y - 1][10] = 1
    elif kind == "wall":                                   # 墙: 粗框 + 砖缝 (最"实")
        box(g, 0, 0, T - 1, T - 1)
        box(g, 1, 1, T - 2, T - 2)
        hline(g, 2, T - 3, 9); hline(g, 2, T - 3, 10)
        vline(g, 13, 2, 9); vline(g, 6, 11, T - 3)
    elif kind == "floor":                                  # 2 变体: 室内板缝
        if v % 2 == 0:
            hline(g, 0, T - 1, 6); hline(g, 0, T - 1, 13)
        else:
            vline(g, 6, 0, T - 1); vline(g, 13, 0, T - 1)
        g[3][3] = 1; g[16][16] = 1
    elif kind == "sand":                                   # 沙: 比草稍密
        for (x, y) in ((5, 3), (14, 6), (3, 11), (11, 15), (17, 12), (8, 8), (16, 17)):
            g[y][x] = 1
            if y + 1 < T:
                g[y + 1][x] = 1
    elif kind == "road":
        # 5 种"位置" × 3 种颗粒 = 15 个变体。
        #   v//3: 0=路中(纯颗粒) 1=左边线 2=右边线 3=上边线 4=下边线
        #   v%3 : 3 套颗粒花纹(打破沿路重复)
        # ⛔ 别在路格上画"中心虚线" —— 那是原版 **60px 一格** 才有意义的设计;
        #   我们的格子是 20px, 每格一条虚线会在路上排出一整排竖线, 看着像**梯子**
        #   (2026-10-03 实测踩过, 用户报"路像格子")。
        #   正确做法: 路缘线由"格子处在路的第几列"决定 → 见 Lua 侧 disp_tile 的 T_ROAD 分支。
        grains = (
            ((2, 3), (6, 3), (10, 3), (14, 3), (18, 3), (4, 7), (8, 7), (12, 7), (16, 7),
             (2, 11), (6, 11), (10, 11), (14, 11), (18, 11), (4, 15), (8, 15), (12, 15), (16, 15)),
            ((3, 1), (7, 1), (11, 1), (15, 1), (19, 1), (1, 5), (5, 5), (9, 5), (13, 5), (17, 5),
             (3, 9), (7, 9), (11, 9), (15, 9), (19, 9), (1, 13), (5, 13), (9, 13), (13, 13), (17, 13)),
            ((1, 2), (5, 2), (9, 2), (13, 2), (17, 2), (3, 6), (7, 6), (11, 6), (15, 6), (19, 6),
             (1, 10), (5, 10), (9, 10), (13, 10), (17, 10), (3, 14), (7, 14), (11, 14), (15, 14), (19, 14)),
        )
        for (x, y) in grains[v % 3]:
            if 0 <= x < T and 0 <= y < T:
                g[y][x] = 1
        m = v // 3
        if m == 1:                                         # 左边线 (2px)
            vline(g, 0, 0, T - 1); vline(g, 1, 0, T - 1)
        elif m == 2:                                       # 右边线
            vline(g, T - 1, 0, T - 1); vline(g, T - 2, 0, T - 1)
        elif m == 3:                                       # 上边线
            hline(g, 0, T - 1, 0); hline(g, 0, T - 1, 1)
        elif m == 4:                                       # 下边线
            hline(g, 0, T - 1, T - 1); hline(g, 0, T - 1, T - 2)
    return g


# 每种地形给几个变体 (顺序 = 图集里的排列顺序; 下标 = atlas 序号)
TILE_VARIANTS = {
    "grass": 4, "dirt": 1, "tree": 3, "water": 1,
    "wall": 1, "floor": 2, "sand": 1,
    "road": 15,          # 5 种位置 x 3 种颗粒 (见 make_tile 的 road 分支)
}


# ⚠ 武器图标**不要自己画** —— 官方取法已经查到了 (2026-10-03):
#   item/durability_item.gd:  SPRITE_HFRAMES = 4 / SPRITE_INIT_FRAME = 39 / MELEE_WEAPON_VFRAMES = 17
#   item/melee_weapon.gd:     hframes=4, vframes=17, frame=39  ⇒ 图标 = 4×17 网格的第 39 帧
#                             (col = 39%4 = 3, row = 39//4 = 9 ⇒ 区域 (96, 288, 32, 32))
#   item/ranged_weapon.gd:    只设 hframes=4, 不设 frame ⇒ 图标 = 第 0 帧
#                             (gui_pistol.png 实际是 82×82 的 4列×3行, 第 0 帧 = 左上角那把)
# 这些帧里的图**很小**(只占一格中一小块), 所以下面 glyph_icon 会先裁到墨点边界再放大。
WPN_FRAME = (96, 288, 32, 32)          # 近战: 帧 39 在 128×544 表里的区域
PISTOL_FRAME = (0, 0, 82, 82)          # 手枪: 帧 0 (4列×3行 @82px 的左上)


TILE_KINDS = ("grass", "dirt", "tree", "water", "wall", "floor", "sand", "road")


# ---- 道具精选表: 顺序 = item.bmp 里的图标序号 = Lua 侧 items[i].icon ----
# ⛔ 别再用 `sorted(glob(item/*.png))[:64]` —— 那是**字母序前 64 个**(0~z), 结果全是
#    弹药 / 浆果 / 种子 / 瞄具, **一把武器都没有** ⇒ "打僵尸"根本没家伙用。
#    改成按用途精选; 武器不在 `item/` 而在 `equipment/weapon/`。
# 字段: (相对 asset/images 的路径, 中文名, 类别, 数值)
#   类别 melee=近战(val=伤害) / gun=枪(val=伤害, 消耗 pistol_ammo) / food / drink / med / tool
ITEM_PICK = [
    # ---- 武器 ----  (第 5 项 = 在精灵表里抠的区域; 不写 = 整张图就是图标)
    ("equipment/weapon/melee/army_knife.png",     "军刀",     "melee", 3, WPN_FRAME),
    ("equipment/weapon/melee/hunter_knife.png",   "猎刀",     "melee", 4, WPN_FRAME),
    ("equipment/weapon/melee/butcher_knife.png",  "屠刀",     "melee", 4, WPN_FRAME),
    ("equipment/weapon/melee/pickaxe.png",        "铁镐",     "melee", 5, WPN_FRAME),
    ("equipment/weapon/melee/fire_axe.png",       "消防斧",   "melee", 7, WPN_FRAME),
    ("equipment/weapon/pistol/gui_pistol.png",    "手枪",     "gun",   8, PISTOL_FRAME),
    # ---- 弹药 ----
    ("equipment/weapon/bullet/pistol_bullet.png", "手枪弹",   "tool",  1),
    ("equipment/weapon/bullet/rifle_bullet.png",  "步枪弹",   "tool",  1),
    # ---- 吃的 ----
    ("item/apple.png",          "苹果",     "food", 8),
    ("item/orange.png",         "橘子",     "food", 7),
    ("item/banana.png",         "香蕉",     "food", 8),
    ("item/canned_bean.png",    "豆罐头",   "food", 18),
    ("item/canned_tuna.png",    "鱼罐头",   "food", 20),
    ("item/herring.png",        "鲱鱼",     "food", 12),
    ("item/tactical_bacon.png", "培根",     "food", 22),
    ("item/mre_ration.png",     "军用口粮", "food", 35),
    ("item/cooked_steak.png",   "熟肉排",   "food", 28),
    # ---- 喝的 ----
    ("item/canteen.png",        "军用水壶", "drink", 30),
    ("item/water_bottle.png",   "矿泉水",   "drink", 25),
    ("item/nota_cola.png",      "可乐",     "drink", 12),
    ("item/kvas.png",           "格瓦斯",   "drink", 10),
    ("item/energy_drink.png",   "能量饮料", "drink", 15),
    # ---- 药 ----
    ("item/bandage.png",        "绷带",     "med", 12),
    ("item/saline_bag.png",     "生理盐水", "med", 25),
    ("item/blood_bag.png",      "血袋",     "med", 40),
    ("item/morphine.png",       "吗啡",     "med", 20),
    ("item/tetracycline.png",   "四环素",   "med", 15),
    ("item/vitamins.png",       "维生素",   "med", 8),
    ("item/heat_pack.png",      "暖贴",     "med", 6),
    # ---- 杂项 ----
    ("item/matches.png",        "火柴",     "tool", 0),
    ("item/battery.png",        "电池",     "tool", 0),
    ("item/rope.png",           "绳索",     "tool", 0),
    ("item/duct_tape.png",      "胶带",     "tool", 0),
    ("item/radio.png",          "收音机",   "tool", 0),
    ("item/car_toolbox.png",    "工具箱",   "tool", 0),
    ("item/cleaning_kit.png",   "清洁套件", "tool", 0),
    ("item/burlap_sack.png",    "麻袋",     "tool", 0),
]


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

    # ---------- 2. 道具 (按 ITEM_PICK 精选, 顺序 = 图标序号) ----------
    icons, items, miss = [], [], []
    for ent in ITEM_PICK:
        rel, cn, kind, val = ent[0], ent[1], ent[2], ent[3]
        region = ent[4] if len(ent) > 4 else None
        p = os.path.join(args.src, rel)
        if not os.path.exists(p):
            miss.append(rel)
            continue           # ⛔ 缺一张就**跳过它** —— 不能留空洞, 否则后面所有图标序号整体错位
        src = load_rgba(p)
        if region:             # 武器 = 从精灵表里抠官方那一帧 (见 WPN_FRAME 注释)
            x, y, w, h = region
            src = src.crop((x, y, x + w, y + h))
        icons.append(glyph_icon(src))
        items.append((cn, len(icons) - 1, kind, val))
    if not icons:
        raise SystemExit("一件道具素材都没找到 —— 检查 --src")
    write_bmp1(os.path.join(args.out, "item.bmp"), hstack(icons, FRAME), FRAME * len(icons), FRAME)
    print("  item.bmp %d 道具 @%dpx (武器 %d / 食 %d / 饮 %d / 药 %d / 杂 %d)"
          % (len(icons), FRAME,
             sum(1 for it in items if it[2] in ("melee", "gun")),
             sum(1 for it in items if it[2] == "food"),
             sum(1 for it in items if it[2] == "drink"),
             sum(1 for it in items if it[2] == "med"),
             sum(1 for it in items if it[2] == "tool")))
    if miss:
        print("  ⚠ 缺素材 %d 个(已跳过): %s" % (len(miss), ", ".join(m[:40] for m in miss[:6])))

    # ---------- 3. 地形瓦片 (每类带变体, 打破"整齐网格"观感) ----------
    tiles, disp = [], {}
    for ti, k in enumerate(TILE_KINDS):
        n = TILE_VARIANTS.get(k, 1)
        disp[ti] = [len(tiles) + i for i in range(n)]
        for v in range(n):
            tiles.append(make_tile(k, 20, v))
    write_bmp1(os.path.join(args.out, "tile.bmp"), hstack(tiles, 20), 20 * len(tiles), 20)
    print("  tile.bmp %d 格 @20px (变体: %s)"
          % (len(tiles), " ".join("%s%d" % (k, TILE_VARIANTS.get(k, 1)) for k in TILE_KINDS)))

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
    L += ["  },", "  items = {"]
    for cn, ic, kind, val in items:
        L.append('    { name="%s", icon=%d, kind="%s", val=%d },' % (cn, ic, kind, val))
    L += ["  },", "  -- disp[地形id+1] = tile.bmp 里的图集序号(变体); 地形: " + " ".join(TILE_KINDS),
          "  disp = {"]
    for ti in range(len(TILE_KINDS)):
        L.append("    { %s }," % ", ".join(str(x) for x in disp[ti]))
    L += ["  },",
          "  -- 路面: 位置组首号 (m=路中 l=左缘 r=右缘 u=上缘 d=下缘); 组内 +0..2 是颗粒变体",
          "  road_disp = { m = %d, l = %d, r = %d, u = %d, d = %d },"
          % (disp[7][0], disp[7][3], disp[7][6], disp[7][9], disp[7][12]),
          "  tile_n = %d," % len(tiles),
          "}", ""]
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
