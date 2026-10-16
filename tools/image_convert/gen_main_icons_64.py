#!/usr/bin/env python3
"""Regenerate icons_main.inc (64x64 1bpp) + icons_main_enum.h from desktop icon folders.

Sources (2026-09-20 重整后):
  /Users/linit/Desktop/完美图标/     结构化应用图标集
  /Users/linit/Desktop/其他图标/     散装/成品库补充

规范化 (2026-09-20):
  - 仅编译"被应用注册表 os_pages.c 实际引用"的图标 (linit: 不用的图标不进固件).
    下面 MAP 就是精简后的 33 个, 已剔除 蓝牙(2)/音量(4)/网络工具(19)/闹钟(22)/
    Meshtastic(24)/下载程序(28) 共 6 个占位图标.
  - 语义化 ID: icons_main_enum.h 自动生成 ICON_* 枚举, 值 = main_icons[] 下标.
    os_pages.c 等一律写 .icon_idx = ICON_XXX, 不再写魔法数字.
  - 单一入口: 运行期一律走 draw_main_icon_stretched() / ui_main_icon_bitmap(),
    二者现在都直接用 main_icons[icon_idx] (main_icon_index() 已退化为直通, 兼容 tbl 删除).

MAP 顺序即 main_icons[] 下标, 亦即 ICON_* 枚举值. 新增图标请在此追加 (顺序即下标).
"""
import os
from PIL import Image

SRC = "/Users/linit/Desktop/LinTOS"   # 2026-10-15 linit 桌面重构: 完美图标/其他图标 移入 LinTOS/
DST_INC = os.path.join(os.path.dirname(__file__), "../../components/ui/ui_common/icons_main.inc")
DST_H   = os.path.join(os.path.dirname(__file__), "../../components/ui/ui_common/icons_main_enum.h")
W = H = 64
THRESHOLD = 128

# (enum_name, png_relative_to_SRC) —— 顺序即 main_icons[] 下标, 亦即 ICON_* 枚举值
MAP = [
    ("ICON_APP_MGR",     "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6.png"),                          # 0  \u5e94\u7528\u5fc3
    ("ICON_USB_HID",     "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u8fd0\u7ef4/\u4eff\u771f\u952e\u9f20.png"),  # 1  \u4eff\u771f\u952e\u9f20
    ("ICON_GAMEPAD",     "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u72ec\u7acb\u6e38\u620f/\u624b\u67c4.png"),    # 2  \u624b\u67c4
    ("ICON_STORAGE",     "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u5b58\u50a8.png"),        # 3  \u5b58\u50a8
    ("ICON_SETTINGS",    "\u5b8c\u7f8e\u56fe\u6807/\u8bbe\u7f6e.png"),                                # 4  \u8bbe\u7f6e
    ("ICON_MP3",         "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u97f3\u4e50.png"),        # 5  \u97f3\u4e50
    ("ICON_GB",          "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5f15\u64ce/GB.png"),           # 6  GB
    ("ICON_BOOK",        "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u9605\u8bfb.png"),        # 7  \u9605\u8bfb\u5668
    ("ICON_WALLPAPER",   "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u58c1\u7eb8.png"),      # 8  \u58c1\u7eb8
    ("ICON_POMODORO",    "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u756a\u8304\u949f.png"),    # 9  \u756a\u8304\u949f
    ("ICON_GBC",         "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5f15\u64ce/GBC.png"),         # 10 GBC
    ("ICON_NES",         "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5f15\u64ce/NES.png"),         # 11 NES
    ("ICON_ARDUBOY",     "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5f15\u64ce/ArduBoy.png"),      # 12 ArduBoy
    ("ICON_MINI_GAMES",  "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u72ec\u7acb\u6e38\u620f.png"),          # 13 \u72ec\u7acb\u6e38\u620f
    ("ICON_WQX",         "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5f15\u64ce/\u6587\u66f2\u661f.png"),      # 14 \u6587\u66f2\u661f
    ("ICON_VPET",        "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5f15\u64ce/\u66b4\u9f99\u673a.png"),      # 15 \u66b4\u9f99\u673a
    ("ICON_WHITEBOARD",  "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u767d\u677f.png"),      # 16 \u767d\u677f
    ("ICON_DIAG",        "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u8fd0\u7ef4/\u7535\u8111\u8bca\u65ad.png"),  # 17 \u7535\u8111\u8bca\u65ad
    ("ICON_BBK",         "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5f15\u64ce/\u6b65\u6b65\u9ad8.png"),      # 18 \u6b65\u6b65\u9ad8
    ("ICON_TERMINAL",    "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u8fd0\u7ef4/\u7ec8\u7aef.png"),        # 19 \u7ec8\u7aef
    ("ICON_USB_BT",      "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u8fd0\u7ef4/USB-\u84dd\u7259.png"), # 20 USB\u84dd\u7259
    ("ICON_USB_NET",     "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u8fd0\u7ef4/USB\u7f51\u5361.png"),   # 21 USB\u7f51\u5361
    ("ICON_WINASSIST",   "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u8fd0\u7ef4/Windows\u52a9\u624b.png"), # 22 \u8fd0\u7ef4\u52a9\u624b
    ("ICON_MODTOOL",     "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u72ec\u7acb\u6e38\u620f/\u4fee\u6539\u673a.png"),  # 23 \u4fee\u6539\u673a
    ("ICON_LOGDIAG",     "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u8fd0\u7ef4/\u65e5\u5fd7\u8bca\u65ad.png"),  # 24 \u65e5\u5fd7\u8bca\u65ad
    ("ICON_SECURE",      "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u79c1\u4eba\u5bc6\u7801\u673a.png"), # 25 \u5bc6\u94a5\u7ba1\u7406\u5668
    ("ICON_CALC",        "\u5176\u4ed6\u56fe\u6807/\u8ba1\u7b97\u5668.png"),                            # 26 \u8ba1\u7b97\u5668
    ("ICON_NETDECT",     "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u8fd0\u7ef4/\u7f51\u7edc\u63a2\u6d4b.png"),  # 27 \u7f51\u7edc\u68c0\u6d4b
    ("ICON_MORSE",       "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u6469\u65af\u5bc6\u7801.png"),  # 28 \u6469\u65af\u5bc6\u7801
    ("ICON_WIFIPROBE",   "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u65e0\u7ebf\u63a2\u6d4b.png"),  # 29 \u65e0\u7ebf\u63a2\u6d4b
    ("ICON_MD",          "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5f15\u64ce/MD.png"),           # 30 MD
    ("ICON_SMS",         "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5f15\u64ce/SMS.png"),         # 31 SMS
    ("ICON_FAV",         "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u6536\u85cf\u5939.png"),      # 32 \u6536\u85cf\u5939
    # 33 码表 (自行车码表, bikecomp.lua). 图标是程序化线描 (tools 里生成后存桌面), 不是缩图.
    ("ICON_BIKE",        "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u5e94\u7528/\u7801\u8868.png"),        # 33 \u7801\u8868
    # 34 频谱仪 (SDR, 2026-10-15 linit: 原来借用 ICON_WIFIPROBE=29 的"无线探测"图标, 现独立出来)
    ("ICON_SDR",         "\u5b8c\u7f8e\u56fe\u6807/\u5e94\u7528\u7ba1\u5bb6/\u8fd0\u7ef4/\u9891\u8c31\u4eea.png"),  # 34 \u9891\u8c31\u4eea
    # 35 BIOS 模拟器 (biossim.lua, 2026-10-15: 原借 ICON_DIAG=17 电脑诊断图标, 现独立)
    ("ICON_BIOSSIM",     "完美图标/应用管家/运维/BIOS模拟器.png"),  # 35 BIOS模拟器
    # 36 航班雷达 (planeradar.lua, 2026-10-15 linit: 原借 ICON_NETDECT=27 网络检测图标, 现独立)
    ("ICON_PLANERADAR",  "完美图标/应用管家/运维/航班雷达.png"),  # 36 航班雷达
]

TRANSFER_PNG = "\u5b8c\u7f8e\u56fe\u6807/\u72b6\u6001\u680f/\u6570\u636e\u4f20\u8f93.png"


def to_1bit(rel):
    img = Image.open(os.path.join(SRC, rel)).convert("L")
    if img.size != (W, H):
        img = img.resize((W, H), Image.Resampling.LANCZOS)
    out = []
    for y in range(H):
        for b in range(W // 8):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                if img.getpixel((x, y)) < THRESHOLD:
                    byte |= (1 << (7 - bit))
            out.append(byte)
    return out


def fmt(data):
    lines = []
    for i in range(0, len(data), 12):
        vals = [f"0x{b:02X}" for b in data[i:i + 12]]
        lines.append("    " + ", ".join(vals) + ",")
    return "\n".join(lines)


assert len({v for v, _ in MAP}) == len(MAP), "枚举名重复"
bytes_per_icon = (W * H) // 8
arrays, var_names, enum_names = [], [], []
for name, png in MAP:
    data = to_1bit(png)
    assert len(data) == bytes_per_icon, (name, png, len(data))
    var = "main_icon_" + name[5:].lower()   # ICON_APP_MGR -> main_icon_app_mgr
    var_names.append(var)
    enum_names.append(name)
    arrays.append(f"const uint8_t {var}[MAIN_ICON_BYTES] = {{\n{fmt(data)}\n}};")

N = len(MAP)
enum_lines = "\n".join(f"    {n} = {i}," for i, n in enumerate(enum_names))
enum_header = f"""/* ============================================================
 * \u4e3b\u83dc\u5355/\u5e94\u7528\u7ba1\u5bb6 \u56fe\u6807\u679a\u4e3e + \u5c3a\u5bf8\u5b8f \u2014\u2014 \u81ea\u52a8\u751f\u6210 (gen_main_icons_64.py)
 * \u52ff\u624b\u5de5\u4fee\u6539. \u6bcf\u4e2a ICON_* \u7684\u503c = \u5176\u5728 main_icons[] \u4e2d\u7684\u4e0b\u6807.
 * ============================================================
 */
#ifndef ICONS_MAIN_ENUM_H
#define ICONS_MAIN_ENUM_H

#define MAIN_ICON_W {W}
#define MAIN_ICON_H {H}
#define MAIN_ICON_BYTES (({W}*{H})/8)
#define MAIN_ICON_COUNT {N}

typedef enum {{
{enum_lines}
}} icon_id_t;

#endif /* ICONS_MAIN_ENUM_H */
"""

inc_header = f"""/* ============================================================
 * \u4e3b\u83dc\u5355/\u5e94\u7528\u7ba1\u5bb6 \u56fe\u6807\u4f4d\u56fe (64x64 1bpp, MSB first) \u2014\u2014 \u81ea\u52a8\u751f\u6210 (gen_main_icons_64.py)
 * \u6765\u6e90: \u684c\u9762\u300c\u5b8c\u7f8e\u56fe\u6807\u300d(\u7ed3\u6784\u5316) + \u300c\u5176\u4ed6\u56fe\u6807\u300d(\u8865\u5145)
 * \u679a\u4e3e/\u5c3a\u5bf8\u5b8f\u89c1 icons_main_enum.h (\u672c\u6587\u4ef6\u4f9d\u8d56\u5b83).
 * \u26a0 \u52ff\u624b\u5de5\u4fee\u6539\u672c\u6587\u4ef6; \u6539\u56fe\u6807\u8bf7\u6539\u684c\u9762 PNG \u540e\u91cd\u8dd1\u811a\u672c.
 * ============================================================
 */
#include "icons_main_enum.h"

"""

body = "\n\n".join(arrays)
icon_list = "\n".join(f"    {v}," for v in var_names)
footer = f"""
const uint8_t *main_icons[MAIN_ICON_COUNT] = {{
{icon_list}
}};
"""

td = to_1bit(TRANSFER_PNG)
assert len(td) == bytes_per_icon, (TRANSFER_PNG, len(td))
transfer_arr = (
    "/* USB \u6570\u636e\u4f20\u8f93\u56fe\u6807 64x64 (\u6302\u8f7d/\u952e\u9f20\u540c\u6b65\u754c\u9762\u4e13\u7528, \u4e0d\u8fdb main_icons) */\n"
    f"const uint8_t icon_transfer[MAIN_ICON_BYTES] = {{\n{fmt(td)}\n}};\n"
)

with open(DST_INC, "w") as f:
    f.write(inc_header + body + "\n\n" + footer + "\n\n" + transfer_arr + "\n")
with open(DST_H, "w") as f:
    f.write(enum_header)
print(f"Wrote {DST_INC}: {W}x{H}, {N} icons")
print(f"Wrote {DST_H}: {N} ICON_* enums")
