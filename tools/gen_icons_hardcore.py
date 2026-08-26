#!/usr/bin/env python3
"""生成 48x48 1-bit 像素画图标 (严格像素级绘制, 无抗锯齿).

完全模拟 Game Boy / 电子词典风格:
  - 仅黑/白两色
  - 1像素或2像素宽的线条
  - 硬朗的直角或阶梯状斜边
  - 干净锐利
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


def px(d, x, y, w=1, h=1):
    """画 1x1 像素点或小方块."""
    d.rectangle((x, y, x + w - 1, y + h - 1), fill=BLACK)

def hline(d, y, x0, x1, w=1):
    """画水平线."""
    d.line((x0, y, x1, y), fill=BLACK, width=w)

def vline(d, x, y0, y1, w=1):
    """画垂直线."""
    d.line((x, y0, x, y1), fill=BLACK, width=w)


def i_game():
    """词典游戏 - 像素小人 + 对话泡."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 小人
    px(d, 20, 10, 8, 6)     # 头
    px(d, 22, 16, 4, 8)     # 身体
    px(d, 21, 24, 2, 8)     # 左腿
    px(d, 25, 24, 2, 8)     # 右腿
    px(d, 16, 18, 4, 2)     # 左手
    px(d, 28, 18, 4, 2)     # 右手
    # 对话泡
    d.ellipse((28, 12, 42, 22), outline=BLACK, width=1)
    # 文字
    hline(d, 15, 31, 33)
    hline(d, 15, 36, 38)
    hline(d, 19, 33, 35)
    return img


def i_key():
    """键盘."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外壳
    hline(d, 10, 4, 43, 2)
    hline(d, 38, 4, 43, 2)
    vline(d, 4, 10, 38, 2)
    vline(d, 43, 10, 38, 2)
    # 按键
    for r in range(3):
        for c in range(5):
            x = 7 + c * 7
            y = 13 + r * 7
            px(d, x, y, 4, 4)
    return img


def i_bt():
    """蓝牙."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外框
    hline(d, 6, 6, 41, 2)
    hline(d, 41, 6, 41, 2)
    vline(d, 6, 6, 41, 2)
    vline(d, 41, 6, 41, 2)
    # B 符号 (像素化)
    # 上三角
    hline(d, 14, 20, 28)
    hline(d, 18, 22, 26)
    hline(d, 22, 24, 24)
    # 下三角
    hline(d, 30, 24, 24)
    hline(d, 34, 22, 26)
    hline(d, 36, 20, 28)
    # 中线
    vline(d, 24, 12, 36)
    return img


def i_pad():
    """手柄."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外壳
    hline(d, 14, 4, 43, 2)
    hline(d, 36, 4, 43, 2)
    vline(d, 4, 14, 36, 2)
    vline(d, 43, 14, 36, 2)
    # D-pad
    px(d, 9, 24, 7, 2)
    px(d, 12, 21, 2, 8)
    # A 按钮
    px(d, 28, 18, 5, 5)
    # B 按钮
    px(d, 35, 24, 5, 5)
    # 中间装饰
    hline(d, 24, 22, 26)
    return img


def i_vol():
    """音量喇叭."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 喇叭
    d.polygon([(8, 20), (18, 20), (26, 12), (26, 36), (18, 28), (8, 28)],
              outline=BLACK, fill=WHITE)
    # 声波 (阶梯状)
    px(d, 30, 16)
    px(d, 32, 18)
    px(d, 32, 20)
    px(d, 30, 22)
    px(d, 32, 24)
    px(d, 34, 20)
    px(d, 34, 22)
    # 第二波
    px(d, 38, 18)
    px(d, 40, 20)
    px(d, 40, 22)
    px(d, 38, 24)
    px(d, 42, 20)
    px(d, 42, 22)
    return img


def i_sd():
    """TF 卡."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外框
    hline(d, 4, 10, 38, 2)
    hline(d, 44, 10, 38, 2)
    vline(d, 10, 4, 44, 2)
    vline(d, 38, 4, 44, 2)
    # 芯片
    hline(d, 8, 14, 34, 1)
    hline(d, 26, 14, 34, 1)
    vline(d, 14, 8, 26, 1)
    vline(d, 34, 8, 26, 1)
    # 引脚
    for i in range(4):
        x = 16 + i * 4
        vline(d, x, 10, 12)
        vline(d, x, 24, 26)
    # 金属触点
    px(d, 16, 32, 4, 6)
    px(d, 24, 32, 4, 6)
    px(d, 32, 32, 4, 6)
    return img


def i_info():
    """设置齿轮."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    cx, cy = 24, 24
    # 齿 (8 个)
    for i in range(8):
        a = math.radians(45 * i)
        tx = int(cx + 18 * math.cos(a))
        ty = int(cy + 18 * math.sin(a))
        if 0 <= tx < SZ and 0 <= ty < SZ:
            px(d, tx - 2, ty - 2, 4, 4)
    # 圆 (像素阶梯)
    for y in range(10, 39):
        dy = y - cy
        r2 = 20 * 20 - dy * dy
        if r2 >= 0:
            dx = int(math.sqrt(r2))
            vline(d, cx - dx, y, y, 1)
            vline(d, cx + dx, y, y, 1)
    # 中心
    for y in range(20, 29):
        dy = y - cy
        r2 = 4 * 4 - dy * dy
        if r2 >= 0:
            dx = int(math.sqrt(r2))
            vline(d, cx - dx, y, y, 1)
            vline(d, cx + dx, y, y, 1)
    return img


def i_play():
    """MP3 播放."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    cx, cy = 24, 24
    r = 20
    for y in range(cy - r, cy + r + 1):
        dy = y - cy
        r2 = r * r - dy * dy
        if r2 >= 0:
            dx = int(math.sqrt(r2))
            vline(d, cx - dx, y, y, 1)
            vline(d, cx + dx, y, y, 1)
    # 三角形
    d.polygon([(20, 16), (20, 32), (32, 24)], fill=BLACK)
    return img


def i_gb():
    """Game Boy 掌机."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外壳
    hline(d, 4, 12, 36, 2)
    hline(d, 44, 12, 36, 2)
    vline(d, 12, 4, 44, 2)
    vline(d, 36, 4, 44, 2)
    # 屏幕框
    hline(d, 8, 15, 33, 1)
    hline(d, 24, 15, 33, 1)
    vline(d, 15, 8, 24, 1)
    vline(d, 33, 8, 24, 1)
    # 屏幕内容 (像素行)
    for y in range(10, 23):
        hline(d, y, 16, 32)
    # 屏幕空白内容
    px(d, 18, 12, 2, 2)
    px(d, 22, 12, 2, 2)
    px(d, 26, 12, 2, 2)
    px(d, 18, 16, 6, 2)
    # D-pad
    px(d, 16, 30, 6, 2)
    px(d, 18, 28, 2, 6)
    # A/B
    px(d, 28, 30, 4, 4)
    px(d, 33, 32, 4, 4)
    return img


def i_book():
    """阅读/书."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外框
    hline(d, 6, 6, 41, 1)
    hline(d, 42, 6, 41, 1)
    vline(d, 6, 6, 42, 1)
    vline(d, 41, 6, 42, 1)
    # 书脊
    vline(d, 24, 6, 42, 2)
    # 文字行
    for y in [11, 16, 21, 26, 31, 36]:
        hline(d, y, 9, 22)
    for y in [11, 16, 21, 26, 31, 36]:
        hline(d, y, 26, 39)
    return img


def i_wall():
    """壁纸."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外框
    hline(d, 6, 4, 43, 1)
    hline(d, 42, 4, 43, 1)
    vline(d, 4, 6, 42, 1)
    vline(d, 43, 6, 42, 1)
    # 地面
    hline(d, 36, 4, 43, 2)
    # 山 (像素三角)
    for i in range(12):
        hline(d, 34 - i, 14 - i, 34 + i)
    # 太阳
    px(d, 34, 10, 4, 4)
    return img


def i_pomo():
    """时钟."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    cx, cy = 24, 24
    r = 17
    # 圆
    for y in range(cy - r, cy + r + 1):
        dy = y - cy
        r2 = r * r - dy * dy
        if r2 >= 0:
            dx = int(math.sqrt(r2))
            vline(d, cx - dx, y, y, 1)
            vline(d, cx + dx, y, y, 1)
    # 刻度
    for i in range(12):
        a = math.radians(30 * i - 90)
        x1 = int(cx + 14 * math.cos(a))
        y1 = int(cy + 14 * math.sin(a))
        x2 = int(cx + 17 * math.cos(a))
        y2 = int(cy + 17 * math.sin(a))
        d.line((x1, y1, x2, y2), fill=BLACK, width=1)
    # 指针
    vline(d, cx, cy, cy - 10)
    hline(d, cy + 4, cx, cx + 8)
    # 中心
    px(d, cx - 1, cy - 1, 2, 2)
    return img


def i_gbc():
    """GBC 彩色掌机."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 上半部
    hline(d, 4, 8, 40, 2)
    hline(d, 24, 8, 40, 2)
    vline(d, 8, 4, 24, 2)
    vline(d, 40, 4, 24, 2)
    # 屏幕
    hline(d, 8, 12, 36, 1)
    hline(d, 22, 12, 36, 1)
    vline(d, 12, 8, 22, 1)
    vline(d, 36, 8, 22, 1)
    # 下半部
    hline(d, 26, 8, 40, 2)
    hline(d, 44, 8, 40, 2)
    vline(d, 8, 26, 44, 2)
    vline(d, 40, 26, 44, 2)
    # D-pad
    px(d, 12, 32, 6, 2)
    px(d, 14, 30, 2, 6)
    # A/B
    px(d, 30, 30, 4, 4)
    px(d, 35, 32, 4, 4)
    return img


def i_nes():
    """NES 红白机."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 顶部主机
    hline(d, 4, 10, 38, 2)
    vline(d, 10, 4, 12, 2)
    vline(d, 38, 4, 12, 2)
    # 主机槽
    hline(d, 8, 12, 36, 1)
    # 控制器
    hline(d, 16, 4, 43, 2)
    hline(d, 34, 4, 43, 2)
    vline(d, 4, 16, 34, 2)
    vline(d, 43, 16, 34, 2)
    # D-pad
    px(d, 9, 22, 7, 2)
    px(d, 12, 19, 2, 8)
    # Select / Start
    px(d, 22, 22, 4, 4)
    px(d, 29, 22, 4, 4)
    # A/B
    px(d, 36, 18, 4, 4)
    px(d, 38, 26, 4, 4)
    return img


def i_arduboy():
    """Arduboy 小屏."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外壳
    hline(d, 4, 12, 36, 2)
    hline(d, 44, 12, 36, 2)
    vline(d, 12, 4, 44, 2)
    vline(d, 36, 4, 44, 2)
    # 屏幕
    hline(d, 8, 15, 33, 1)
    hline(d, 22, 15, 33, 1)
    vline(d, 15, 8, 22, 1)
    vline(d, 33, 8, 22, 1)
    # 屏幕内容
    for y in range(10, 21):
        hline(d, y, 16, 32)
    px(d, 18, 12, 2, 2)
    px(d, 22, 12, 2, 2)
    px(d, 26, 12, 4, 2)
    # D-pad
    px(d, 15, 28, 6, 2)
    px(d, 17, 26, 2, 6)
    # A/B
    px(d, 27, 28, 4, 4)
    px(d, 32, 30, 4, 4)
    # LED
    px(d, 22, 40, 2, 2)
    return img


def i_mini_games():
    """迷你游戏."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外框
    hline(d, 4, 4, 43, 1)
    hline(d, 43, 4, 43, 1)
    vline(d, 4, 4, 43, 1)
    vline(d, 43, 4, 43, 1)
    # 地面
    hline(d, 40, 4, 43, 2)
    # 小人 (跑步)
    px(d, 20, 14, 4, 4)   # 头
    px(d, 19, 18, 6, 6)   # 身体
    px(d, 18, 24, 2, 6)   # 后腿
    px(d, 26, 26, 2, 8)   # 前腿
    px(d, 14, 20, 4, 2)   # 后手
    px(d, 26, 22, 4, 2)   # 前手
    return img


def i_keyboard():
    """键鼠."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 键盘
    hline(d, 22, 4, 28, 2)
    hline(d, 40, 4, 28, 2)
    vline(d, 4, 22, 40, 2)
    vline(d, 28, 22, 40, 2)
    # 按键
    for r in range(3):
        for c in range(3):
            px(d, 7 + c*6, 24 + r*6, 4, 4)
    # 鼠标
    hline(d, 12, 30, 42, 2)
    hline(d, 40, 30, 42, 2)
    vline(d, 30, 12, 40, 2)
    vline(d, 42, 12, 42, 2)
    # 中间滚轮
    vline(d, 36, 22, 32)
    return img


def i_cat_engine():
    """引擎齿轮."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    cx, cy = 24, 24
    # 齿 (8 个)
    for i in range(8):
        a = math.radians(45 * i)
        tx = int(cx + 18 * math.cos(a))
        ty = int(cy + 18 * math.sin(a))
        if 0 <= tx < SZ and 0 <= ty < SZ:
            px(d, tx - 3, ty - 3, 6, 6)
    # 圆 (像素)
    for y in range(10, 39):
        dy = y - cy
        r2 = 14 * 14 - dy * dy
        if r2 >= 0:
            dx = int(math.sqrt(r2))
            vline(d, cx - dx, y, y, 1)
            vline(d, cx + dx, y, y, 1)
    # 中心孔
    for y in range(19, 30):
        dy = y - cy
        r2 = 4 * 4 - dy * dy
        if r2 >= 0:
            dx = int(math.sqrt(r2))
            vline(d, cx - dx, y, y, 1)
            vline(d, cx + dx, y, y, 1)
    return img


def i_cat_game():
    """游戏手柄."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 外壳
    hline(d, 18, 6, 41, 2)
    hline(d, 32, 6, 41, 2)
    vline(d, 6, 18, 32, 2)
    vline(d, 41, 18, 32, 2)
    # D-pad
    px(d, 11, 23, 7, 2)
    px(d, 14, 20, 2, 8)
    # A/B
    px(d, 27, 19, 5, 5)
    px(d, 34, 24, 5, 5)
    return img


def i_cat_tool():
    """工具扳手."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 扳手头
    d.polygon([(6, 6), (22, 6), (22, 22), (6, 22)], outline=BLACK, fill=WHITE)
    # 内部孔
    for y in range(10, 19):
        dy = y - 14
        r2 = 3 * 3 - dy * dy
        if r2 >= 0:
            dx = int(math.sqrt(r2))
            vline(d, 14 - dx, y, y, 1)
            vline(d, 14 + dx, y, y, 1)
    # 手柄
    hline(d, 26, 18, 40, 2)
    vline(d, 40, 18, 40, 4)
    hline(d, 40, 18, 40, 2)
    return img


def i_cat_other():
    """其他灯泡."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    cx, cy = 24, 16
    r = 12
    # 灯泡
    for y in range(cy - r, cy + r + 1):
        dy = y - cy
        r2 = r * r - dy * dy
        if r2 >= 0:
            dx = int(math.sqrt(r2))
            vline(d, cx - dx, y, y, 1)
            vline(d, cx + dx, y, y, 1)
    # 螺纹
    hline(d, 28, 16, 32, 1)
    hline(d, 32, 16, 32, 1)
    vline(d, 16, 28, 34, 1)
    vline(d, 32, 28, 34, 1)
    # 灯丝
    d.arc((16, 10, 32, 24), start=0, end=180, fill=BLACK, width=1)
    return img


def i_app_store():
    """应用商店."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    # 屋顶
    d.polygon([(4, 20), (24, 4), (44, 20)], outline=BLACK, width=1)
    # 房身
    hline(d, 4, 6, 42, 1)
    hline(d, 44, 6, 42, 1)
    vline(d, 6, 4, 44, 1)
    vline(d, 42, 4, 44, 1)
    # 门
    hline(d, 34, 20, 28, 1)
    hline(d, 44, 20, 28, 1)
    vline(d, 20, 34, 44, 1)
    vline(d, 28, 34, 44, 1)
    # 橱窗
    hline(d, 24, 10, 18, 1)
    hline(d, 32, 10, 18, 1)
    vline(d, 10, 24, 32, 1)
    vline(d, 18, 24, 32, 1)
    hline(d, 24, 30, 38, 1)
    hline(d, 32, 30, 38, 1)
    vline(d, 30, 24, 32, 1)
    vline(d, 38, 24, 32, 1)
    # 招牌
    hline(d, 10, 18, 30, 2)
    vline(d, 18, 10, 12, 2)
    vline(d, 30, 10, 12, 2)
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
    print(f"生成 {SZ}x{SZ} 严格像素画 (1-bit), 共 {len(ICONS)} 个 ...")
    lines = [
        f"/* 像素风格 (Game Boy / 电子词典) 图标 {SZ}x{SZ} - 1像素线条 */",
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
