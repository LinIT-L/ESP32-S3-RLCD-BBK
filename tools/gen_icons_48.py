#!/usr/bin/env python3
"""生成 GB/GBC 复古阻力图标 (4 阶灰阶 + 有序抖动).

原理 = Game Boy / GBC 游戏模拟器的"屏幕优化":
  屏其实只有 1-bit 亮度, 但用有序抖动 (ordered dithering) 把
  白 / 浅灰 / 灰 / 深灰 / 黑 近似成黑白相间的点阵, 得到游戏机显示屏
  那种特有的灰阶质感. 本脚本: 每个图标先在 4 阶灰阶 'L' 画布上绘制,
  再用 Bayer 4x4 矩阵抖动成 1-bit, 风格与 GB/GBA 画面一致.

产物:
  - 模块/商店图标 48x48  (对应 xmb_icons[], 索引 0..22)
  - 迷你应用图标   32x32  (单独 mini_icons[], 索引 0..25)
输出覆盖 components/menu/icons_data.inc.

模块图标索引契约 (与 menu_system.c 的 icon_idx 一致):
  0  icon_game    - 词典 (Aa)
  1  icon_key     - 键盘
  2  icon_bt      - 蓝牙
  3  icon_pad     - 手柄
  4  icon_vol     - 音量
  5  icon_sd      - TF 卡
  6  icon_info    - 设置齿轮
  7  icon_play    - MP3
  8  icon_gb      - Game Boy
  9  icon_book    - 阅读
  10 icon_wall    - 壁纸
  11 icon_pomo    - 番茄钟
  12 icon_gbc     - GBC
  13 icon_nes     - NES
  14 icon_arduboy - Arduboy
  15 icon_mini_games - 迷你游戏
  16 icon_keyboard   - 键鼠
  17 icon_cat_engine - 引擎分类
  18 icon_cat_game   - 游戏分类
  19 icon_cat_tool   - 工具分类
  20 icon_cat_other  - 其他分类
  21 icon_app_store  - 应用商店
  22 icon_net        - 网络工具

迷你应用图标索引 (mini_icons[], 顺序与 mini_apps.c s_apps 一致, 去掉 NETTOOL):
  0 CALCULATOR 1 STOPWATCH 2 COUNTDOWN 3 CALENDAR 4 WHITEBOARD 5 DICE
  6 UNITS 7 SNAKE 8 TICTACTOE 9 MEMORY 10 GUESS 11 2048 12 PONG
  13 MINESWEEP 14 TETRIS 15 BREAKOUT 16 BLACKJACK 17 FISHING 18 ALARMCLK
  19 HANGMAN 20 DIVER 21 BINARY 22 REACT 23 MORA 24 RACE 25 NETSCAN
"""
import os
from PIL import Image, ImageDraw

MOD_SZ = 48
MINI_SZ = 32
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(SCRIPT_DIR, '..', 'components', 'menu', 'icons_data.inc')

# GB 灰阶 (0=黑, 255=白)
B = 0     # 黑
DG = 70   # 深灰 (约 25%)
LG = 175  # 浅灰 (约 68%)
W = 255   # 白

# Bayer 4x4 有序抖动阈值矩阵 (值 0..15)
BAYER = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]


def dither(img):
    """灰阶 'L' 图 -> 1-bit '1' 图 (有序抖动 / Bayer 4x4)."""
    w, h = img.size
    out = Image.new('1', (w, h), 1)
    px = img.load()
    op = out.load()
    for y in range(h):
        for x in range(w):
            v = px[x, y]
            b = BAYER[x % 4][y % 4]
            thr = ((b + 0.5) / 16.0) * 256.0
            if v < thr:
                op[x, y] = 0
    return out


def encode_1bit(img):
    data = []
    w, h = img.size
    for y in range(h):
        byte_val = 0
        bit = 0
        for x in range(w):
            if img.getpixel((x, y)) == 0:
                byte_val |= (1 << (7 - bit))
            bit += 1
            if bit == 8:
                data.append(byte_val)
                byte_val = 0
                bit = 0
        if bit > 0:
            data.append(byte_val)
    return data


def canvas(sz):
    """白底 'L' 画布: 背景为纯白, 只保留图标本体形状 (黑色/灰阶笔画).
    使用时底色为白 -> 应用管理/主菜单白卡上即"无背景方框".
    图标内部阴影/灰阶用 DG/LG, 经 Bayer 抖动保留 GB 质感."""
    return Image.new('L', (sz, sz), W)


def face(d, x0, y0, x1, y1, tone=W, out=0, wd=2):
    """直角/圆角方框: 默认白底(浅灰等价) 黑描边. tone 填充, out 描边."""
    d.rectangle((x0, y0, x1, y1), fill=tone, outline=out, width=wd)


# =====================================================================
# 模块图标 48x48
# =====================================================================
def i_game():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    # 词典 Aa (纯字体轮廓, 无背景框)
    d.polygon([(7, 22), (16, 6), (25, 22)], outline=B, width=2)   # 大写 A 三角
    d.line((13, 14, 19, 14), fill=B, width=2)                      # A 横杠
    d.ellipse((31, 10, 45, 26), outline=B, width=2)               # 小写 a 圆
    d.line((38, 10, 38, 26), fill=B, width=2)                      # a 竖
    d.line((8, 40, 40, 40), fill=B, width=2)                       # 下划线
    return img

def i_key():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    # 键盘 (无外框)
    for r in range(4):
        for c in range(5):
            d.rectangle((6 + c * 8, 6 + r * 9, 11 + c * 8, 13 + r * 9), fill=B)
    return img

def i_bt():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.ellipse((5, 5, 42, 42), fill=W, outline=B, width=2)
    d.ellipse((9, 9, 38, 38), fill=DG, outline=B, width=2)
    d.polygon([(22, 13), (28, 20), (24, 21), (28, 27), (22, 34), (18, 30),
               (23, 24), (18, 18)], fill=B)
    return img

def i_pad():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.rounded_rectangle((4, 13, 43, 37), radius=4, fill=LG, outline=B, width=2)
    d.rectangle((9, 20, 13, 30), fill=B)
    d.rectangle((6, 23, 16, 27), fill=B)
    d.ellipse((27, 18, 32, 23), outline=B, width=2)
    d.ellipse((38, 19, 43, 24), outline=B, width=2)
    d.ellipse((30, 29, 35, 34), outline=B, width=2)
    return img

def i_vol():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.polygon([(6, 17), (16, 17), (26, 9), (26, 39), (16, 31), (6, 31)],
              fill=DG, outline=B, width=2)
    d.arc((24, 10, 44, 38), -45, 45, fill=B, width=2)
    d.arc((30, 16, 40, 32), -45, 45, fill=LG, width=2)
    return img

def i_sd():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    face(d, 10, 4, 38, 44)
    d.rectangle((14, 8, 34, 26), fill=LG, outline=B, width=2)
    d.rectangle((16, 32, 20, 40), fill=B)
    d.rectangle((22, 32, 26, 40), fill=B)
    d.rectangle((28, 32, 32, 40), fill=B)
    return img

def i_info():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    cx, cy = 24, 24
    for i in range(8):
        a = i * 3.14159 / 4
        tx = cx + 17 * __import__('math').cos(a); ty = cy + 17 * __import__('math').sin(a)
        d.rectangle((tx - 4, ty - 4, tx + 4, ty + 4), fill=B)
    d.ellipse((cx - 14, cy - 14, cx + 14, cy + 14), fill=DG, outline=B, width=2)
    d.ellipse((cx - 5, cy - 5, cx + 5, cy + 5), fill=W, outline=B, width=2)
    return img

def i_play():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.ellipse((4, 4, 43, 43), fill=LG, outline=B, width=2)
    d.polygon([(20, 13), (20, 35), (35, 24)], fill=B)
    return img

def i_gb():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.rounded_rectangle((8, 3, 40, 45), radius=4, fill=DG, outline=B, width=2)
    d.rounded_rectangle((12, 7, 36, 26), radius=2, fill=W, outline=B, width=2)
    d.rectangle((15, 10, 33, 23), fill=LG)
    d.rectangle((16, 16, 32, 22), fill=B)                      # 屏幕 "画面" 灰带
    d.rectangle((12, 30, 16, 39), fill=B)
    d.rectangle((10, 33, 18, 37), fill=B)
    d.ellipse((29, 30, 34, 35), outline=B, width=2)
    d.ellipse((35, 33, 40, 38), outline=B, width=2)
    return img

def i_book():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    # 翻开的书 (无背景框)
    d.line((24, 5, 24, 43), fill=B, width=2)
    d.polygon([(5, 8), (22, 5), (22, 43), (5, 41)], outline=B, width=2)
    d.polygon([(43, 8), (26, 5), (26, 43), (43, 41)], outline=B, width=2)
    for y in (15, 23, 31):
        d.line((9, y + 4, 18, y + 2), fill=B, width=1)
        d.line((30, y + 2, 39, y + 4), fill=B, width=1)
    return img

def i_wall():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    # 山 + 太阳 (无背景框)
    d.polygon([(4, 40), (18, 10), (32, 40)], outline=B, width=2)
    d.polygon([(26, 40), (35, 22), (44, 40)], outline=B, width=2)
    d.line((6, 42, 42, 42), fill=B, width=2)
    d.ellipse((28, 6, 38, 16), fill=B)
    return img

def i_pomo():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.ellipse((8, 10, 40, 42), fill=W, outline=B, width=2)
    d.ellipse((11, 13, 37, 39), fill=LG, outline=B, width=1)
    d.line((24, 24, 24, 14), fill=B, width=2)
    d.line((24, 24, 31, 28), fill=B, width=2)
    d.rectangle((21, 3, 27, 11), fill=B)
    return img

def i_gbc():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.rounded_rectangle((7, 3, 41, 23), radius=3, fill=DG, outline=B, width=2)
    d.rectangle((11, 7, 37, 19), fill=LG)
    d.rectangle((13, 16, 28, 19), fill=B)
    d.rounded_rectangle((7, 25, 41, 45), radius=3, fill=W, outline=B, width=2)
    d.rectangle((12, 33, 20, 38), fill=B)
    d.rectangle((28, 31, 33, 36), outline=B, width=2)
    d.rectangle((35, 33, 40, 38), outline=B, width=2)
    return img

def i_nes():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.rounded_rectangle((4, 14, 43, 33), radius=3, fill=LG, outline=B, width=2)
    d.rectangle((9, 20, 13, 28), fill=B)
    d.rectangle((6, 23, 16, 25), fill=B)
    d.ellipse((22, 24, 26, 28), outline=B, width=1)
    d.ellipse((29, 24, 33, 28), outline=B, width=1)
    d.ellipse((37, 17, 42, 22), outline=B, width=2)
    d.rectangle((10, 4, 38, 12), fill=DG, outline=B, width=2)
    return img

def i_arduboy():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.rounded_rectangle((12, 3, 36, 45), radius=4, fill=DG, outline=B, width=2)
    d.rounded_rectangle((15, 7, 33, 21), radius=1, fill=LG, outline=B, width=1)
    d.rectangle((17, 18, 28, 20), fill=B)
    d.rectangle((16, 28, 20, 36), fill=B); d.rectangle((14, 31, 22, 33), fill=B)
    d.ellipse((27, 28, 31, 32), outline=B, width=2)
    return img

def i_mini_games():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    # 手柄 + 肩键 (无背景框)
    d.rounded_rectangle((4, 20, 44, 36), radius=4, fill=LG, outline=B, width=2)
    d.rounded_rectangle((6, 3, 18, 14), radius=2, fill=LG, outline=B, width=2)
    d.rounded_rectangle((30, 3, 42, 14), radius=2, fill=LG, outline=B, width=2)
    d.rectangle((10, 24, 14, 32), fill=B); d.rectangle((7, 27, 17, 29), fill=B)
    d.ellipse((25, 24, 31, 30), outline=B, width=2)
    d.ellipse((34, 28, 40, 34), outline=B, width=2)
    return img

def i_keyboard():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    face(d, 4, 22, 28, 40, tone=LG)
    for r in range(3):
        for c in range(4):
            d.rectangle((6 + c * 5, 24 + r * 5, 9 + c * 5, 27 + r * 5), fill=B)
    d.ellipse((30, 10, 42, 40), fill=LG, outline=B, width=2)
    d.line((36, 14, 36, 24), fill=B, width=2)
    return img

def i_cat_engine():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    cx, cy = 24, 24
    for i in range(8):
        a = i * 3.14159 / 4
        tx = cx + 17 * __import__('math').cos(a); ty = cy + 17 * __import__('math').sin(a)
        d.rectangle((tx - 4, ty - 4, tx + 4, ty + 4), fill=B)
    d.ellipse((cx - 14, cy - 14, cx + 14, cy + 14), fill=LG, outline=B, width=2)
    d.ellipse((cx - 5, cy - 5, cx + 5, cy + 5), fill=W, outline=B, width=2)
    return img

def i_cat_game():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.rounded_rectangle((6, 16, 41, 34), radius=4, fill=LG, outline=B, width=2)
    d.rectangle((11, 21, 15, 29), fill=B); d.rectangle((8, 24, 18, 26), fill=B)
    d.ellipse((26, 19, 32, 25), outline=B, width=2)
    d.ellipse((34, 24, 40, 30), outline=B, width=2)
    return img

def i_cat_tool():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.polygon([(13, 27), (18, 22), (38, 42), (33, 47)], fill=LG, outline=B, width=2)
    d.ellipse((5, 5, 25, 25), fill=DG, outline=B, width=2)
    d.ellipse((11, 11, 20, 20), fill=W, outline=B, width=2)
    d.rectangle((14, 2, 18, 11), fill=W)
    return img

def i_cat_other():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.ellipse((12, 4, 36, 28), fill=LG, outline=B, width=2)
    d.rectangle((16, 28, 32, 37), fill=DG, outline=B, width=2)
    for i in range(4):
        a = i * 3.14159 / 2 + 0.7
        d.line((24, 12, 24 + 10 * __import__('math').cos(a),
                12 + 10 * __import__('math').sin(a)), fill=B, width=2)
    return img

def i_app_store():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.polygon([(4, 16), (24, 3), (44, 16)], fill=LG, outline=B, width=2)  # 遮阳篷
    d.line((6, 17, 42, 17), fill=B, width=2)
    d.line((8, 18, 8, 44), fill=B, width=2)      # 左侧墙
    d.line((40, 18, 40, 44), fill=B, width=2)    # 右侧墙
    d.rectangle((22, 28, 30, 44), fill=B)        # 门
    d.rectangle((12, 22, 18, 28), outline=B, width=2)   # 左窗
    d.rectangle((30, 22, 36, 28), outline=B, width=2)   # 右窗
    return img

def i_net():
    img = canvas(MOD_SZ); d = ImageDraw.Draw(img)
    d.arc((8, 8, 40, 40), 210, 330, fill=B, width=2)
    d.arc((13, 13, 35, 35), 210, 330, fill=DG, width=2)
    d.arc((18, 18, 30, 30), 210, 330, fill=LG, width=2)
    d.ellipse((21, 21, 27, 27), fill=B)
    return img


MODULE_ICONS = [
    ('icon_game', i_game), ('icon_key', i_key), ('icon_bt', i_bt),
    ('icon_pad', i_pad), ('icon_vol', i_vol), ('icon_sd', i_sd),
    ('icon_info', i_info), ('icon_play', i_play), ('icon_gb', i_gb),
    ('icon_book', i_book), ('icon_wall', i_wall), ('icon_pomo', i_pomo),
    ('icon_gbc', i_gbc), ('icon_nes', i_nes), ('icon_arduboy', i_arduboy),
    ('icon_mini_games', i_mini_games), ('icon_keyboard', i_keyboard),
    ('icon_cat_engine', i_cat_engine), ('icon_cat_game', i_cat_game),
    ('icon_cat_tool', i_cat_tool), ('icon_cat_other', i_cat_other),
    ('icon_app_store', i_app_store), ('icon_net', i_net),
]

# =====================================================================
# 迷你应用图标 32x32  (顺序与 mini_apps.c s_apps 一致)
# =====================================================================
def fc(d, x0, y0, x1, y1, tone=W, out=0, wd=1):
    d.rectangle((x0, y0, x1, y1), fill=tone, outline=out, width=wd)

def m_calc():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    fc(d, 4, 4, 27, 28)
    d.rectangle((6, 6, 25, 12), fill=B)
    d.rectangle((7, 16, 13, 22), fill=LG, outline=B); d.rectangle((7, 16, 13, 18), fill=B)
    d.rectangle((15, 16, 21, 22), fill=LG, outline=B); d.rectangle((15, 16, 21, 18), fill=B)
    d.rectangle((7, 23, 13, 26), fill=B); d.rectangle((15, 23, 21, 26), fill=B)
    return img

def m_sw():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.ellipse((6, 8, 26, 28), fill=LG, outline=B, width=2)
    d.rectangle((12, 4, 20, 9), fill=B)
    d.rectangle((14, 2, 18, 6), fill=B)
    d.line((16, 18, 21, 13), fill=B, width=2)
    return img

def m_cd():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.ellipse((6, 6, 26, 26), fill=LG, outline=B, width=2)
    d.line((16, 16, 16, 9), fill=B, width=2)
    d.line((16, 16, 20, 19), fill=B, width=2)
    return img

def m_cal():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    fc(d, 5, 5, 27, 27)
    d.rectangle((7, 5, 9, 11), fill=B); d.rectangle((23, 5, 25, 11), fill=B)
    d.rectangle((8, 9, 24, 13), fill=B)
    d.line((8, 17, 24, 17), fill=B); d.line((8, 21, 24, 21), fill=B)
    return img

def m_wb():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    fc(d, 5, 5, 27, 24, tone=LG)
    d.rectangle((8, 8, 24, 21), fill=W, outline=B)
    d.line((8, 24, 24, 27), fill=B, width=2)
    return img

def m_dice():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.rounded_rectangle((5, 5, 27, 27), radius=2, fill=DG, outline=B, width=2)
    d.ellipse((9, 9, 13, 13), fill=B); d.ellipse((19, 9, 23, 13), fill=B)
    d.ellipse((9, 19, 13, 23), fill=B); d.ellipse((19, 19, 23, 23), fill=B)
    d.ellipse((14, 14, 18, 18), fill=B)
    return img

def m_units():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    fc(d, 4, 7, 28, 25, tone=LG)
    d.line((6, 7, 26, 25), fill=B, width=2)
    for i in range(5):
        d.line((8 + i*5, 20, 12 + i*5, 24), fill=B, width=1)
    return img

def m_snake():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.rectangle((6, 6, 10, 26), fill=B)
    d.rectangle((10, 22, 26, 26), fill=B)
    d.rectangle((22, 14, 26, 22), fill=B)
    d.rectangle((8, 8, 10, 10), fill=W)  # 眼
    d.ellipse((24, 12, 27, 15), fill=LG)
    return img

def m_tt():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    fc(d, 5, 5, 27, 27)
    d.line((5, 12, 27, 12), fill=B, width=1); d.line((5, 19, 27, 19), fill=B, width=1)
    d.line((12, 5, 12, 27), fill=B, width=1); d.line((19, 5, 19, 27), fill=B, width=1)
    d.line((7, 8, 10, 11), fill=B, width=2); d.line((10, 8, 7, 11), fill=B, width=2)
    d.ellipse((14, 14, 17, 17), outline=B, width=2)
    d.line((21, 21, 25, 25), fill=B, width=2); d.line((25, 21, 21, 25), fill=B, width=2)
    return img

def m_memory():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    for r in range(2):
        for c in range(2):
            d.rounded_rectangle((6 + c*8, 6 + r*10, 12 + c*8, 14 + r*10), radius=1,
                                fill=LG, outline=B, width=2)
    d.rectangle((9, 20, 15, 26), fill=B)  # 背面
    return img

def m_guess():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.ellipse((6, 6, 26, 26), fill=LG, outline=B, width=2)
    d.line((11, 10, 16, 15), fill=B, width=2); d.line((21, 10, 16, 15), fill=B, width=2)
    d.line((16, 15, 16, 19), fill=B, width=2); d.line((13, 24, 19, 24), fill=B, width=2)
    return img

def m_2048():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    # 九宫格 (无外框)
    d.line((4, 11, 28, 11), fill=B); d.line((4, 21, 28, 21), fill=B)
    d.line((16, 4, 16, 28), fill=B)
    for p in [(6, 5), (17, 5), (9, 12)]: d.rectangle((p[0], p[1], p[0]+3, p[1]+3), fill=B)
    return img

def m_pong():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.rectangle((5, 8, 8, 18), fill=B); d.rectangle((5, 18, 8, 24), fill=B)
    d.rectangle((24, 8, 27, 18), fill=B); d.rectangle((24, 18, 27, 24), fill=B)
    d.rectangle((15, 14, 17, 16), fill=B); d.line((5, 3, 27, 3), fill=B, width=1)
    return img

def m_minesweep():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.ellipse((7, 7, 25, 25), fill=LG, outline=B, width=2)
    d.line((9, 9, 23, 23), fill=B, width=2); d.line((22, 10, 10, 22), fill=B, width=2)
    for a in (0.8, 1.9, 3.0, 4.1, 5.2):
        x = 16 + 11 * __import__('math').cos(a); y = 16 + 11 * __import__('math').sin(a)
        d.line((9 + (x-9)*0.2, 9 + (y-9)*0.2, x, y), fill=B, width=1)
    return img

def m_tetris():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    for i in range(4): d.rectangle((5, 7+i*5, 9, 10+i*5), fill=B)
    d.rectangle((10, 17, 14, 20), fill=B); d.rectangle((15, 22, 19, 25), fill=B)
    d.rectangle((10, 22, 14, 25), fill=DG)
    return img

def m_breakout():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    for i in range(4): d.rectangle((4+i*7, 6, 9+i*7, 10), fill=B)
    d.rectangle((5, 15, 12, 18), fill=B); d.rectangle((20, 15, 27, 18), fill=DG)
    d.rectangle((11, 25, 21, 27), fill=B)
    return img

def m_blackjack():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.rounded_rectangle((5, 5, 19, 16), radius=1, fill=W, outline=B, width=2)
    d.line((6, 5, 18, 16), fill=B, width=1)
    d.rounded_rectangle((13, 14, 27, 25), radius=1, fill=LG, outline=B, width=2)
    d.ellipse((19, 22, 23, 26), fill=B)
    return img

def m_fishing():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.line((4, 6, 28, 4), fill=B, width=2)
    d.line((28, 4, 14, 20), fill=B, width=2)
    d.polygon([(12, 22), (16, 22), (14, 26)], outline=B, width=1)
    d.ellipse((21, 18, 27, 24), fill=LG, outline=B, width=1)
    d.ellipse((23, 20, 24, 21), fill=B)
    return img

def m_alarm():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.ellipse((7, 8, 25, 26), fill=LG, outline=B, width=2)
    d.line((16, 17, 16, 11), fill=B, width=2); d.line((16, 17, 20, 19), fill=B, width=2)
    return img

def m_hangman():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.line((5, 25, 27, 25), fill=B, width=2); d.line((19, 5, 19, 25), fill=B, width=2)
    d.line((19, 5, 10, 5), fill=B, width=1); d.line((10, 5, 10, 8), fill=B, width=1)
    d.ellipse((7, 8, 13, 14), fill=LG, outline=B, width=2)
    d.line((10, 14, 10, 21), fill=B, width=2); d.line((10, 16, 6, 20), fill=B, width=1)
    d.line((10, 16, 14, 20), fill=B, width=1)
    return img

def m_diver():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.ellipse((11, 6, 17, 12), outline=B, width=2)          # 头盔
    d.rectangle((13, 12, 15, 20), fill=B)                     # 身体
    d.rectangle((9, 14, 11, 22), fill=B); d.rectangle((17, 14, 19, 22), fill=B)
    d.line((5, 4, 27, 24), fill=B, width=1)                   # 水面线
    return img

def m_binary():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.rectangle((5, 6, 12, 16), fill=B); d.line((5, 10, 12, 10), fill=B, width=1)
    d.rectangle((6, 17, 11, 26), fill=LG, outline=B)
    d.ellipse((20, 8, 26, 14), outline=B, width=2)
    d.rectangle((19, 21, 24, 25), fill=DG)
    return img

def m_react():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.ellipse((5, 5, 27, 27), fill=W, outline=B, width=2)
    d.ellipse((9, 9, 23, 23), fill=LG, outline=B, width=2)
    d.ellipse((14, 14, 18, 18), fill=B)
    for a in (0, 1.57, 3.14, 4.71):
        x = 16 + 15 * __import__('math').cos(a); y = 16 + 15 * __import__('math').sin(a)
        d.line((16, 16, x, y), fill=B, width=1)
    return img

def m_mora():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.ellipse((6, 8, 26, 28), fill=LG, outline=B, width=2)
    d.line((16, 13, 16, 20), fill=B, width=2)
    d.line((16, 20, 11, 24), fill=B, width=2); d.line((16, 20, 21, 24), fill=B, width=2)
    return img

def m_race():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.rectangle((4, 7, 12, 14), fill=B); d.ellipse((7, 14, 10, 17), fill=LG, outline=B)
    d.ellipse((17, 14, 20, 17), fill=LG, outline=B)
    d.line((12, 10, 24, 10), fill=B, width=2)
    d.polygon([(24, 6), (24, 14), (28, 10)], fill=B)
    return img

def m_netscan():
    img = canvas(MINI_SZ); d = ImageDraw.Draw(img)
    d.arc((6, 6, 26, 26), 210, 330, fill=B, width=1)
    d.arc((11, 11, 21, 21), 210, 330, fill=LG, width=1)
    d.line((16, 16, 20, 7), fill=B, width=1)
    d.ellipse((14, 14, 18, 18), fill=B)
    return img


MINI_ICONS = [
    ('mini_calc', m_calc), ('mini_sw', m_sw), ('mini_cd', m_cd),
    ('mini_cal', m_cal), ('mini_wb', m_wb), ('mini_dice', m_dice),
    ('mini_units', m_units), ('mini_snake', m_snake), ('mini_tt', m_tt),
    ('mini_memory', m_memory), ('mini_guess', m_guess), ('mini_2048', m_2048),
    ('mini_pong', m_pong), ('mini_minesweep', m_minesweep), ('mini_tetris', m_tetris),
    ('mini_breakout', m_breakout), ('mini_blackjack', m_blackjack), ('mini_fishing', m_fishing),
    ('mini_alarm', m_alarm), ('mini_hangman', m_hangman), ('mini_diver', m_diver),
    ('mini_binary', m_binary), ('mini_react', m_react), ('mini_mora', m_mora),
    ('mini_race', m_race), ('mini_netscan', m_netscan),
]


def emit_array(lines, name, size, data):
    nbytes = (size * size) // 8
    lines.append(f"static const uint8_t {name}[{size}*{size}/8] = {{")
    cnt = 0
    for b in data:
        lines.append(f"0x{b:02X},")
        cnt += 1
        if cnt % 16 == 0:
            lines.append("\n")
    # 去掉最后一个逗号
    last = lines[-1].rstrip()
    if last.endswith(","):
        last = last[:-1] + "\n"
    lines[-1] = last
    lines.append("};")
    lines.append("")


def main():
    lines = [
        "/* GB/GBC 复古阻力图标 - 有序抖动生成, 勿手改 (tools/gen_icons_48.py) */",
        f"#define XMB_ICON_W {MOD_SZ}",
        f"#define XMB_ICON_H {MOD_SZ}",
        f"#define XMB_ICON_BYTES (({MOD_SZ}*{MOD_SZ})/8)",
        f"#define XMB_ICON_COUNT {len(MODULE_ICONS)}",
        "",
    ]
    for name, fn in MODULE_ICONS:
        img = dither(fn())
        emit_array(lines, name, MOD_SZ, encode_1bit(img))

    lines.append(f"#define MINI_ICON_W {MINI_SZ}")
    lines.append(f"#define MINI_ICON_H {MINI_SZ}")
    lines.append(f"#define MINI_ICON_BYTES (({MINI_SZ}*{MINI_SZ})/8)")
    lines.append(f"#define MINI_ICON_COUNT {len(MINI_ICONS)}")
    lines.append("")
    for name, fn in MINI_ICONS:
        img = dither(fn())
        emit_array(lines, name, MINI_SZ, encode_1bit(img))

    lines.append("const uint8_t *xmb_icons[XMB_ICON_COUNT] = {")
    for name, _ in MODULE_ICONS:
        lines.append(f"    {name},")
    lines.append("};")
    lines.append("")
    lines.append("const uint8_t *mini_icons[MINI_ICON_COUNT] = {")
    for name, _ in MINI_ICONS:
        lines.append(f"    {name},")
    lines.append("};")
    lines.append("")

    with open(OUT, 'w') as f:
        f.write("\n".join(lines))
    print(f"OK: 写入 {OUT}  模块={len(MODULE_ICONS)}  迷你={len(MINI_ICONS)}")


if __name__ == '__main__':
    main()