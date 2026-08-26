#!/usr/bin/env python3
"""Regenerate cat_icons.inc (应用管理侧栏四分类图标) at 64x64 from "完美图标".

侧栏四分类 (顺序与 app_category_t 一致):
  0 引擎       -> 应用管理/引擎/引擎.png
  1 独立游戏   -> 应用管理/独立游戏/独立游戏1.png
  2 程序       -> 应用管理/程序/程序.png
  3 运维       -> 应用管理/运维/运维.png

Polarity: 源图暗(<threshold)=bit 1=黑像素, 黑底白字/线稿, 保持原样 (invert=False).
"""
import os
from PIL import Image

SRC = "/Users/linit/Desktop/完美图标"
DST = os.path.join(os.path.dirname(__file__), "../../components/menu/cat_icons.inc")
W = H = 64
THRESHOLD = 128

MAP = [
    ("cat_icon_engine",   "应用管理/引擎/引擎.png"),
    ("cat_icon_game",     "应用管理/独立游戏/独立游戏1.png"),
    ("cat_icon_tool",     "应用管理/程序/程序.png"),
    ("cat_icon_other",    "应用管理/运维/运维.png"),
]


def to_1bit(path):
    img = Image.open(os.path.join(SRC, path)).convert("L")
    if img.size != (W, H):
        img = img.resize((W, H), Image.Resampling.LANCZOS)
    out = []
    for y in range(H):
        for b in range(W // 8):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                v = img.getpixel((x, y))
                if v < THRESHOLD:
                    byte |= (1 << (7 - bit))
            out.append(byte)
    return out


def fmt(data):
    lines = []
    for i in range(0, len(data), 12):
        vals = [f"0x{b:02X}" for b in data[i:i + 12]]
        lines.append("    " + ", ".join(vals) + ",")
    return "\n".join(lines)


bytes_per_icon = (W * H) // 8
arrays = []
var_names = []
for var, png in MAP:
    data = to_1bit(png)
    assert len(data) == bytes_per_icon, (var, len(data))
    var_names.append(var)
    arrays.append(f"const uint8_t {var}[CAT_ICON_BYTES] = {{\n{fmt(data)}\n}};")

header = f"""/* ============================================================
 * 应用管理侧栏四分类专用图标 (64x64 1bpp, MSB first) 由脚本批量生成
 * 来源: 桌面「完美图标」 (见 tools/image_convert/gen_cat_icons_64.py)
 * 顺序与 app_category_t 一致: 0引擎 1独立游戏 2程序 3运维.
 * 注意: 这些是应用管理侧栏专属图标, 不放主菜单.
 * ============================================================
 */
#define CAT_ICON_W {W}
#define CAT_ICON_H {H}
#define CAT_ICON_BYTES (({W}*{H})/8)
#define CAT_ICON_COUNT {len(MAP)}

"""

body = "\n\n".join(arrays)
icon_list = "\n".join(f"    {v}," for v in var_names)
footer = f"""
const uint8_t *cat_icons[CAT_ICON_COUNT] = {{
{icon_list}
}};
"""

with open(DST, "w") as f:
    f.write(header + body + "\n\n" + footer + "\n")
print(f"Wrote {DST}: {W}x{H}, {len(MAP)} icons, {bytes_per_icon} B each")