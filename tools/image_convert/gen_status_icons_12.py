#!/usr/bin/env python3
"""Regenerate status-bar 1-bit PNG icons from 完美图标/状态栏 and write
- status_icons12.inc  (声音4档 / 数据传输 / 迷你手柄 / WiFi)
- bt_icon_png.h       (蓝牙, 12x12)

每个图标保留原始 PNG 尺寸 (不强制缩放到 12x12):
  声音:   完美图标/状态栏/声音/喇叭1-4.png  (24x12)
  数据传输: 完美图标/状态栏/数据传输.png (24x12)
  迷你手柄: 完美图标/状态栏/迷你手柄.png (24x12)
  WiFi:   完美图标/状态栏/wifi.png      (12x12)
  蓝牙:   Desktop/图标/蓝牙.png         (12x12)
"""
import os
from PIL import Image

PERFECT_SB = "/Users/linit/Desktop/完美图标/状态栏"
SOUND_DIR  = os.path.join(PERFECT_SB, "声音")
BAT_DIR    = os.path.join(PERFECT_SB, "电池")
SRC_ICONS  = "/Users/linit/Desktop/图标"
T = 128


def load_white_bg(path):
    """RGBA 图标透明背景合成为白色, 避免透明像素被 convert('L') 判为黑色. 返回灰度."""
    img = Image.open(path).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    bg = Image.alpha_composite(bg, img)
    return bg.convert("L")


def to_1bit(path):
    """按原始 PNG 尺寸生成 1bpp MSB-first 位图数据. 返回 (data, w, h).
    V1.0.9x 修复: 按每 8 列一组从最高字节开始正确打包 (col0..7→byte0, ...),
    旧实现把所有列挤进同一低位字节, 导致 >8px 图标只剩最右 8px(变 8 宽)."""
    img = load_white_bg(path)
    w, h = img.size
    BPR = (w + 7) // 8          # bytes per row
    out = []
    for y in range(h):
        for g in range(BPR):
            byte = 0
            for bit in range(8):
                x = g * 8 + bit
                if x < w and img.getpixel((x, y)) < T:
                    byte |= 1 << (7 - bit)
            out.append(byte)
    return out, w, h


def fmt(data, per_line=12):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("   " + ",".join(f"0x{b:02X}" for b in data[i:i + per_line]) + ",")
    return "\n".join(lines)


# 蓝牙: overwrite existing bt_icon_png.h (完美图标状态栏蓝牙.png 16x16)
bt, bt_w, bt_h = to_1bit(os.path.join(PERFECT_SB, "蓝牙.png"))
with open(os.path.join(os.path.dirname(__file__),
                       "../../components/menu/bt_icon_png.h"), "w") as f:
    f.write(f"""/* 蓝牙图标 (桌面图标文件夹 蓝牙.png -> 12x12 1bit, MSB-first, 黑=1)
 * 由 tools/image_convert/gen_status_icons_12.py 生成 */
#ifndef BT_ICON_PNG_H
#define BT_ICON_PNG_H

#define BT_ICON_W {bt_w}
#define BT_ICON_H {bt_h}

static const uint8_t bt_icon_png[{len(bt)}] = {{
    {", ".join(f"0x{b:02X}" for b in bt)},
}};

#endif
""")

# 声音 4 档 (喇叭1=最低 音量, 喇叭4=最大)
snd_icons = []
for i in range(1, 5):
    d, w, h = to_1bit(os.path.join(SOUND_DIR, f"喇叭{i}.png"))
    snd_icons.append((d, w, h))
snd_w, snd_h = snd_icons[0][1], snd_icons[0][2]

# 数据传输 / 迷你手柄 / WiFi 保留原始尺寸
data, data_w, data_h = to_1bit(os.path.join(PERFECT_SB, "数据传输.png"))
pad,  pad_w,  pad_h  = to_1bit(os.path.join(PERFECT_SB, "迷你手柄.png"))
wifi, wifi_w, wifi_h = to_1bit(os.path.join(PERFECT_SB, "wifi.png"))

lines = [
    "/* 状态栏 1bpp PNG 图标 (声音4档 / 数据传输 / 迷你手柄 / WiFi / 电池), 由 gen_status_icons_12.py 生成",
    " * 各图标保留原始尺寸, MSB-first, 黑=1 */",
    "#ifndef STATUS_ICONS12_INC",
    "#define STATUS_ICONS12_INC",
    "",
]

# 电池文件名: 0-5 按电量递增, 满=满电. 数据顺序: 电池0/1/2/3/4/5/满
bat_files = ["电池0.png","电池1.png","电池2.png","电池3.png","电池4.png","电池5.png","电池满.png"]
bat_icons = []
for fn in bat_files:
    d, w, h = to_1bit(os.path.join(BAT_DIR, fn))
    bat_icons.append((d, w, h))
bat_w, bat_h = bat_icons[0][1], bat_icons[0][2]
bat_lines = [
    f"/* 电池: {len(bat_icons)}档 (电池0=空 -> 电池满=100%) */",
    f"#define BAT_ICON_W {bat_w}",
    f"#define BAT_ICON_H {bat_h}",
    f"#define BAT_ICON_N {len(bat_icons)}",
    f"static const uint8_t bat_icon[BAT_ICON_N][((BAT_ICON_W+7)/8)*BAT_ICON_H] = {{",
]
for d, w, h in bat_icons:
    bat_lines.append(f"{{\n{fmt(d)}}},")
bat_lines += [ "};", "" ]
lines += bat_lines

lines += [
    f"/* 喇叭: 音量分 4 档, 喇叭1=最低, 喇叭4=最大. 静音/音量0 时不显示 */",
    f"#define SND_ICON_W {snd_w}",
    f"#define SND_ICON_H {snd_h}",
    f"#define SND_ICON_N 4",
    f"static const uint8_t snd12_icon[SND_ICON_N][((SND_ICON_W+7)/8)*SND_ICON_H] = {{",
]
for d, w, h in snd_icons:
    lines.append(f"{{\n{fmt(d)}}},")
lines += [
    "};",
    "",
    f"#define DATA_ICON_W {data_w}",
    f"#define DATA_ICON_H {data_h}",
    f"static const uint8_t data12_icon[((DATA_ICON_W+7)/8)*DATA_ICON_H] = {{\n{fmt(data)}}};",
    "",
    f"#define PAD_ICON_W {pad_w}",
    f"#define PAD_ICON_H {pad_h}",
    f"static const uint8_t pad12_icon[((PAD_ICON_W+7)/8)*PAD_ICON_H] = {{\n{fmt(pad)}}};",
    "",
    f"#define WIFI_ICON_W {wifi_w}",
    f"#define WIFI_ICON_H {wifi_h}",
    f"static const uint8_t wifi12_icon[((WIFI_ICON_W+7)/8)*WIFI_ICON_H] = {{\n{fmt(wifi)}}};",
    "",
    "#endif",
    "",
]

with open(os.path.join(os.path.dirname(__file__),
                       "../../components/menu/status_icons12.inc"), "w") as f:
    f.write("\n".join(lines))

print("bt", bt_w, bt_h, len(bt),
      "snd", snd_w, snd_h, [len(x[0]) for x in snd_icons],
      "data", data_w, data_h, len(data),
      "pad", pad_w, pad_h, len(pad),
      "wifi", wifi_w, wifi_h, len(wifi))