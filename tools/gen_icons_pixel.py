#!/usr/bin/env python3
"""生成 48x48 1-bit 像素画图标 (Game Boy / 电子词典风格, 无抗锯齿).

严格控制像素, 1px/2px 线条, 干净利落.
"""
import os
import math
from PIL import Image, ImageDraw

SZ = 48
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(SCRIPT_DIR, '..', 'components', 'menu', 'icons_data.inc')

BLACK = 0
WHITE = 1


def new_canvas():
    return Image.new('1', (SZ, SZ), WHITE)


def encode_1bit(img):
    data = []
    for y in range(SZ):
        row_bytes = []
        byte_val = 0
        bit = 0
        for x in range(SZ):
            p = img.getpixel((x, y))
            if p == BLACK:
                byte_val |= (1 << (7 - bit))
            bit += 1
            if bit == 8:
                row_bytes.append(byte_val)
                byte_val = 0
                bit = 0
        if bit > 0:
            row_bytes.append(byte_val)
        data.extend(row_bytes)
    return data


def i_game():
    """词典游戏 - 像素小人 + 对话泡"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 小人头
    d.rectangle((16, 8, 24, 16), fill=BLACK)
    # 身体
    d.rectangle((18, 17, 22, 28), fill=BLACK)
    # 腿
    d.rectangle((18, 29, 20, 36), fill=BLACK)
    d.rectangle((22, 29, 24, 36), fill=BLACK)
    # 对话泡
    d.ellipse((28, 10, 42, 24), outline=BLACK, width=2)
    d.line((36, 24, 38, 30), fill=BLACK, width=2)
    # 文字 Ab
    d.text((32, 14), "A", fill=BLACK)
    return img


def i_key():
    """键盘 (像素方格)"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外壳
    d.rectangle((4, 10, 43, 38), outline=BLACK, width=2)
    # 按键网格
    for r in range(3):
        for c in range(5):
            x = 7 + c * 7
            y = 13 + r * 7
            d.rectangle((x, y, x + 4, y + 4), fill=BLACK)
    return img


def i_bt():
    """蓝牙"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外框 (圆角方)
    d.rounded_rectangle((6, 6, 41, 41), radius=4, outline=BLACK, width=2)
    # B 符号 (像素)
    pts = [(20, 12), (28, 20), (22, 20), (28, 28), (20, 36), (16, 32), (22, 24), (16, 16)]
    for i in range(0, len(pts) - 1, 2):
        d.line((pts[i], pts[i+1]), fill=BLACK, width=2)
    # 填充中间
    d.rectangle((22, 22, 26, 26), fill=BLACK)
    return img


def i_pad():
    """手柄"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外壳
    d.rounded_rectangle((4, 14, 43, 38), radius=4, outline=BLACK, width=2)
    # 十字
    d.rectangle((9, 21, 13, 29), fill=BLACK)
    d.rectangle((6, 24, 16, 26), fill=BLACK)
    # AB 按钮
    d.ellipse((28, 18, 33, 23), fill=BLACK)
    d.ellipse((35, 24, 40, 29), fill=BLACK)
    # 中间小灯
    d.ellipse((24, 23, 26, 25), fill=BLACK)
    return img


def i_vol():
    """音量喇叭"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 喇叭
    d.polygon([(6, 18), (16, 18), (26, 10), (26, 38), (16, 30), (6, 30)],
              fill=BLACK)
    # 声波 (1px 弧)
    d.arc((28, 14, 40, 34), start=-30, end=30, fill=BLACK, width=2)
    d.arc((32, 18, 38, 30), start=-30, end=30, fill=BLACK, width=1)
    return img


def i_sd():
    """TF 卡"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外框
    d.rectangle((10, 4, 38, 44), outline=BLACK, width=2)
    # 芯片区
    d.rectangle((14, 8, 34, 26), outline=BLACK, width=2)
    # 引脚 (8 个)
    for i in range(4):
        x = 15 + i * 4
        d.line((x, 9, x, 12), fill=BLACK, width=1)
        d.line((x + 1, 9, x + 1, 12), fill=BLACK, width=1)
    # 下方触点
    for i in range(3):
        x = 16 + i * 6
        d.rectangle((x, 32, x + 4, 36), fill=BLACK)
    return img


def i_info():
    """设置齿轮"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    cx, cy = 24, 24
    # 齿
    for i in range(8):
        a = math.radians(45 * i)
        tx = cx + 18 * math.cos(a)
        ty = cy + 18 * math.sin(a)
        d.rectangle((tx-3, ty-3, tx+3, ty+3), fill=BLACK)
    # 圆
    d.ellipse((10, 10, 38, 38), outline=BLACK, width=2)
    d.ellipse((20, 20, 28, 28), outline=BLACK, width=2)
    return img


def i_play():
    """MP3 播放"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 圆
    d.ellipse((4, 4, 43, 43), outline=BLACK, width=2)
    # 播放三角
    d.polygon([(20, 14), (20, 34), (34, 24)], fill=BLACK)
    return img


def i_gb():
    """Game Boy 掌机"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外壳 (圆角矩形)
    d.rounded_rectangle((12, 4, 36, 44), radius=4, outline=BLACK, width=2)
    # 屏幕框
    d.rectangle((15, 8, 33, 24), outline=BLACK, width=2)
    # 屏幕内容
    d.rectangle((17, 10, 31, 22), fill=BLACK)
    # 像素内容 (类似 Tetris 方块)
    d.rectangle((18, 11, 20, 13), fill=WHITE)
    d.rectangle((22, 11, 24, 13), fill=WHITE)
    d.rectangle((26, 11, 28, 13), fill=WHITE)
    d.rectangle((18, 15, 20, 17), fill=WHITE)
    d.rectangle((18, 19, 20, 21), fill=WHITE)
    d.rectangle((26, 15, 28, 17), fill=WHITE)
    # 方向键 (D-pad)
    d.rectangle((16, 28, 18, 36), fill=BLACK)
    d.rectangle((14, 30, 20, 34), fill=BLACK)
    # A/B 键 (圆形)
    d.ellipse((28, 28, 33, 33), fill=BLACK)
    d.ellipse((33, 31, 38, 36), fill=BLACK)
    return img


def i_book():
    """阅读/书"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 书外框
    d.rectangle((6, 6, 41, 42), outline=BLACK, width=2)
    # 书脊
    d.line((24, 6, 24, 42), fill=BLACK, width=2)
    # 文字行 (左页)
    for y in [12, 17, 22, 27, 32, 37]:
        d.line((9, y, 22, y), fill=BLACK, width=1)
    # 文字行 (右页)
    for y in [12, 17, 22, 27, 32, 37]:
        d.line((26, y, 39, y), fill=BLACK, width=1)
    return img


def i_wall():
    """壁纸 - 风景"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 相框
    d.rectangle((4, 6, 43, 42), outline=BLACK, width=2)
    # 地面
    d.line((4, 36, 43, 36), fill=BLACK, width=2)
    # 山 (三角)
    d.polygon([(10, 34), (20, 20), (30, 34)], outline=BLACK, fill=WHITE)
    # 太阳
    d.ellipse((30, 10, 36, 16), fill=BLACK)
    return img


def i_pomo():
    """番茄钟 - 时钟"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 圆
    d.ellipse((6, 6, 41, 41), outline=BLACK, width=2)
    # 点 (12 点)
    for i in range(12):
        a = math.radians(30 * i - 90)
        x1 = 24 + 17 * math.cos(a)
        y1 = 24 + 17 * math.sin(a)
        x2 = 24 + 20 * math.cos(a)
        y2 = 24 + 20 * math.sin(a)
        d.line((x1, y1, x2, y2), fill=BLACK, width=1)
    # 指针
    d.line((24, 24, 24, 12), fill=BLACK, width=2)
    d.line((24, 24, 32, 28), fill=BLACK, width=1)
    # 中心点
    d.ellipse((23, 23, 25, 25), fill=BLACK)
    return img


def i_gbc():
    """GBC 彩色掌机 (上屏下键)"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 上半屏外壳
    d.rectangle((8, 4, 40, 24), outline=BLACK, width=2)
    # 屏幕
    d.rectangle((12, 8, 36, 20), fill=BLACK)
    # 屏幕像素
    for y in [10, 12, 14, 16, 18]:
        d.line((14, y, 34, y), fill=WHITE, width=1)
    # 下半键位外壳
    d.rectangle((8, 26, 40, 44), outline=BLACK, width=2)
    # D-pad
    d.rectangle((12, 32, 14, 40), fill=BLACK)
    d.rectangle((10, 34, 16, 38), fill=BLACK)
    # A/B
    d.ellipse((30, 30, 35, 35), fill=BLACK)
    d.ellipse((36, 32, 41, 37), fill=BLACK)
    return img


def i_nes():
    """NES 红白机 (控制器)"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 控制器外壳
    d.rounded_rectangle((4, 14, 43, 34), radius=3, outline=BLACK, width=2)
    # D-pad
    d.rectangle((9, 20, 13, 28), fill=BLACK)
    d.rectangle((6, 23, 16, 25), fill=BLACK)
    # Select / Start
    d.ellipse((22, 24, 26, 28), fill=BLACK)
    d.ellipse((29, 24, 33, 28), fill=BLACK)
    # A/B
    d.ellipse((36, 18, 41, 23), fill=BLACK)
    d.ellipse((38, 26, 43, 31), fill=BLACK)
    # 顶部主机边缘
    d.line((10, 6, 38, 6), fill=BLACK, width=2)
    d.line((12, 6, 12, 12), fill=BLACK, width=2)
    d.line((36, 6, 36, 12), fill=BLACK, width=2)
    d.rectangle((14, 8, 36, 10), fill=BLACK)
    return img


def i_arduboy():
    """Arduboy 小屏"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外壳
    d.rounded_rectangle((12, 4, 36, 44), radius=4, outline=BLACK, width=2)
    # 屏幕
    d.rectangle((15, 8, 33, 22), outline=BLACK, width=2)
    d.rectangle((17, 10, 31, 20), fill=BLACK)
    # 屏幕像素
    d.rectangle((19, 12, 21, 14), fill=WHITE)
    d.rectangle((25, 12, 27, 14), fill=WHITE)
    d.rectangle((19, 16, 29, 18), fill=WHITE)
    # D-pad
    d.rectangle((16, 28, 20, 36), fill=BLACK)
    d.rectangle((14, 30, 22, 34), fill=BLACK)
    # A/B
    d.ellipse((27, 28, 31, 32), fill=BLACK)
    d.ellipse((32, 30, 36, 34), fill=BLACK)
    # 指示灯
    d.ellipse((23, 40, 25, 42), fill=BLACK)
    return img


def i_mini_games():
    """迷你游戏 (像素小人跑步)"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外框
    d.rectangle((4, 4, 43, 43), outline=BLACK, width=2)
    # 地面
    d.line((4, 40, 43, 40), fill=BLACK, width=2)
    # 小人 (跑步姿势)
    # 头
    d.rectangle((20, 12, 24, 16), fill=BLACK)
    # 身体
    d.rectangle((19, 17, 25, 24), fill=BLACK)
    # 腿 (前)
    d.rectangle((24, 25, 26, 36), fill=BLACK)
    # 腿 (后)
    d.rectangle((18, 28, 20, 34), fill=BLACK)
    # 手臂
    d.rectangle((15, 18, 18, 22), fill=BLACK)
    d.rectangle((26, 20, 29, 24), fill=BLACK)
    # 眼睛
    d.rectangle((21, 13, 22, 14), fill=WHITE)
    return img


def i_keyboard():
    """USB 键鼠"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 键盘
    d.rectangle((4, 22, 28, 40), outline=BLACK, width=2)
    # 按键
    for r in range(3):
        for c in range(4):
            d.rectangle((6 + c*5, 24 + r*5, 9 + c*5, 27 + r*5), fill=BLACK)
    # 鼠标
    d.ellipse((30, 12, 42, 40), outline=BLACK, width=2)
    # 鼠标按键线
    d.line((36, 14, 36, 24), fill=BLACK, width=1)
    # 连接线
    d.line((28, 28, 32, 22), fill=BLACK, width=2)
    return img


def i_cat_engine():
    """引擎 - 齿轮 (像素)"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    cx, cy = 24, 24
    # 8 齿
    for i in range(8):
        a = math.radians(45 * i)
        tx = cx + 18 * math.cos(a)
        ty = cy + 18 * math.sin(a)
        d.rectangle((tx-4, ty-4, tx+4, ty+4), fill=BLACK)
    # 齿轮圆
    d.ellipse((10, 10, 38, 38), outline=BLACK, width=2)
    # 中心孔
    d.ellipse((19, 19, 29, 29), fill=WHITE, outline=BLACK, width=2)
    return img


def i_cat_game():
    """游戏 - 手柄 (像素)"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外壳
    d.rounded_rectangle((6, 16, 41, 34), radius=4, outline=BLACK, width=2)
    # D-pad
    d.rectangle((11, 21, 15, 29), fill=BLACK)
    d.rectangle((8, 24, 18, 26), fill=BLACK)
    # AB 按钮
    d.ellipse((26, 19, 31, 24), fill=BLACK)
    d.ellipse((33, 24, 38, 29), fill=BLACK)
    return img


def i_cat_tool():
    """工具 - 扳手 (像素)"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 扳手头 (大圆)
    d.ellipse((6, 6, 22, 22), outline=BLACK, width=2)
    # 中间孔
    d.ellipse((11, 11, 17, 17), fill=WHITE, outline=BLACK, width=2)
    # 手柄 (倾斜)
    d.line((16, 16, 36, 36), fill=BLACK, width=4)
    # 手柄端部
    d.rectangle((34, 34, 42, 42), fill=BLACK)
    return img


def i_cat_other():
    """其他 - 灯泡 (像素)"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 灯泡
    d.ellipse((12, 4, 36, 28), outline=BLACK, width=2)
    # 内部灯丝
    d.arc((16, 10, 32, 24), start=0, end=180, fill=BLACK, width=2)
    # 螺纹
    d.rectangle((16, 28, 32, 36), outline=BLACK, width=2)
    d.line((16, 30, 32, 30), fill=BLACK, width=1)
    d.line((16, 33, 32, 33), fill=BLACK, width=1)
    # 灯光射线
    for a in range(0, 360, 60):
        ar = math.radians(a)
        x1 = 24 + 20 * math.cos(ar)
        y1 = 14 + 20 * math.sin(ar)
        x2 = 24 + 24 * math.cos(ar)
        y2 = 14 + 24 * math.sin(ar)
        d.line((x1, y1, x2, y2), fill=BLACK, width=2)
    return img


def i_app_store():
    """应用商店 - 商店 (像素)"""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 屋顶 (三角)
    d.polygon([(4, 20), (24, 4), (44, 20)], outline=BLACK, width=2, fill=WHITE)
    # 房子
    d.rectangle((6, 20, 42, 44), outline=BLACK, width=2)
    # 门
    d.rectangle((20, 30, 28, 44), outline=BLACK, width=2)
    # 橱窗
    d.rectangle((10, 24, 18, 32), outline=BLACK, width=2)
    d.rectangle((30, 24, 38, 32), outline=BLACK, width=2)
    # 招牌
    d.rectangle((18, 12, 30, 18), fill=BLACK)
    return img


ICONS = [
    ('icon_game', i_game, False),
    ('icon_key', i_key, False),
    ('icon_bt', i_bt, False),
    ('icon_pad', i_pad, False),
    ('icon_vol', i_vol, False),
    ('icon_sd', i_sd, False),
    ('icon_info', i_info, False),
    ('icon_play', i_play, False),
    ('icon_gb', i_gb, False),
    ('icon_book', i_book, False),
    ('icon_wall', i_wall, True),
    ('icon_pomo', i_pomo, True),
    ('icon_gbc', i_gbc, False),
    ('icon_nes', i_nes, False),
    ('icon_arduboy', i_arduboy, False),
    ('icon_mini_games', i_mini_games, True),
    ('icon_keyboard', i_keyboard, True),
    ('icon_cat_engine', i_cat_engine, False),
    ('icon_cat_game', i_cat_game, False),
    ('icon_cat_tool', i_cat_tool, False),
    ('icon_cat_other', i_cat_other, False),
    ('icon_app_store', i_app_store, False),
]


def main():
    print(f"生成 {SZ}x{SZ} 像素画图标 (1-bit), 共 {len(ICONS)} 个 ...")
    lines = [
        f"/* 像素风格 (Game Boy / 电子词典) 图标 {SZ}x{SZ} */",
        f"#define XMB_ICON_W {SZ}",
        f"#define XMB_ICON_H {SZ}",
        f"#define XMB_ICON_BYTES (({SZ}*{SZ})/8)",
        f"#define XMB_ICON_COUNT {len(ICONS)}",
        "",
    ]
    for name, fn, is_static in ICONS:
        img = fn()
        data = encode_1bit(img)
        print(f"  {name}: {len(data)} 字节")
        prefix = f"static const uint8_t {name}[XMB_ICON_BYTES] = {{" if is_static \
                 else f"const uint8_t {name}[XMB_ICON_BYTES] = {{"
        lines.append(prefix)
        for i, b in enumerate(data):
            lines.append(f"0x{b:02X}," + ("\n" if (i + 1) % 16 == 0 else " "))
        last = lines[-1].rstrip()
        if last.endswith(","):
            last = last[:-1]
        lines[-1] = last
        lines.append("};")
        lines.append("")

    lines.append("const uint8_t *xmb_icons[XMB_ICON_COUNT] = {")
    for name, _, _ in ICONS:
        lines.append(f"    {name},")
    lines.append("};")
    lines.append("")

    with open(OUT, 'w') as f:
        f.write("\n".join(lines))
    print(f"\nOK: 写入 {OUT}")


if __name__ == '__main__':
    main()
