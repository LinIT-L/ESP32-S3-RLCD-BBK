#!/usr/bin/env python3
"""Regenerate icons_main.inc at 64x64 from the "完美图标" desktop PNG folder.

Source: /Users/linit/Desktop/完美图标
All source PNGs are already 64x64 (native), so no resize is performed;
icons are embedded at their original resolution to avoid up/down scaling.
The only exception is the main-menu scroll effect where the center icon is
highlighted/zoomed at draw time (handled by firmware, not this script).

Mapping (main_icons[] index -> desktop PNG, relative to 完美图标 root):
  0  game      -> 应用管理.png            (应用管理)
  1  key       -> 应用管理/运维/仿真键鼠.png
  2  bt        -> 状态栏/蓝牙.png         (蓝牙, 未在主菜单显示, 保留槽位)
  3  pad       -> 应用管理/程序/手柄.png
  4  vol       -> 状态栏/声音/喇叭2.png   (音量, 未在主菜单显示, 保留槽位)
  5  sd        -> 应用管理/程序/存储.png
  6  info      -> 设置.png
  7  play      -> 应用管理/程序/音乐.png
  8  gb        -> 应用管理/引擎/GB.png
  9  book      -> 应用管理/程序/阅读.png
  10 wall      -> 应用管理/程序/壁纸.png
  11 pomo      -> 应用管理/程序/番茄钟.png
  12 gbc       -> 应用管理/引擎/GBC.png
  13 nes       -> 应用管理/引擎/NES.png
  14 arduboy   -> 应用管理/引擎/ArduBoy.png
  15 mini      -> 应用管理/独立游戏/独立游戏1.png
  16 wq        -> 应用管理/引擎/文曲星.png
  17 cwj       -> 应用管理/引擎/暴龙机.png
  18 theme     -> 应用管理/程序/白板.png   (主题用白板替代)
  19 net       -> 应用管理/运维/网络工具.png
  20 diag      -> 应用管理/运维/电脑诊断.png
  21 bbk       -> 应用管理/引擎/步步高.png
  22 alarm     -> 应用管理/程序/闹钟.png   (占位: 暂无对应应用)
  23 terminal  -> 应用管理/运维/终端.png   (占位: 暂无对应应用)

Polarity: 源图暗(<threshold)=bit 1=黑像素. For main menu icons the original
is black-on-transparent/white, so keep that (invert=False).
"""
import os
from PIL import Image

SRC = "/Users/linit/Desktop/完美图标"
DST = os.path.join(os.path.dirname(__file__), "../../components/ui_common/icons_main.inc")
W = H = 64
THRESHOLD = 128

# USB 数据传输图标 (64x64), 独立数组, 供挂载/键鼠界面引用
TRANSFER_PNG = "状态栏/数据传输.png"
# 仿真键鼠图标
KEY_PNG = "应用管家/运维/仿真键鼠.png"

# (var_name, png_relative_to_SRC)  —— 顺序必须与 icons_main.inc 现有变量一致
MAP = [
    ("main_icon_game",      "应用管家.png"),
    ("main_icon_key",       KEY_PNG),
    ("main_icon_bt",        "状态栏/蓝牙.png"),
    ("main_icon_pad",       "应用管家/独立游戏/手柄.png"),
    ("main_icon_vol",       "状态栏/声音/喇叭2.png"),
    ("main_icon_sd",        "应用管家/应用/存储.png"),
    ("main_icon_info",      "设置.png"),
    ("main_icon_play",      "应用管家/应用/音乐.png"),
    ("main_icon_gb",        "应用管家/引擎/GB.png"),
    ("main_icon_book",      "应用管家/应用/阅读.png"),
    ("main_icon_wall",      "应用管家/应用/壁纸.png"),
    ("main_icon_pomo",      "应用管家/应用/番茄钟.png"),
    ("main_icon_gbc",       "应用管家/引擎/GBC.png"),
    ("main_icon_nes",       "应用管家/引擎/NES.png"),
    ("main_icon_arduboy",   "应用管家/引擎/ArduBoy.png"),
    ("main_icon_mini_games","应用管家/独立游戏.png"),
    ("main_icon_wq",        "应用管家/引擎/文曲星.png"),
    ("main_icon_cwj",       "应用管家/引擎/暴龙机.png"),
    ("main_icon_theme",     "应用管家/应用/白板.png"),
    ("main_icon_net",       "应用管家/运维/USB网卡.png"),   # 网络工具: 源无"网络工具.png", 用 USB网卡
    ("main_icon_diag",      "应用管家/运维/电脑诊断.png"),
    ("main_icon_bbk",       "应用管家/引擎/步步高.png"),
    ("main_icon_alarm",     "应用管家/应用/闹钟.png"),
    ("main_icon_terminal",  "应用管家/运维/终端.png"),
]


def to_1bit(path):
    img = Image.open(os.path.join(SRC, path)).convert("L")
    # 源图均已是 64x64 原生尺寸, 仅在非目标大小时才缩放 (尽量不放大缩小)
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
    arrays.append(f"const uint8_t {var}[MAIN_ICON_BYTES] = {{\n{fmt(data)}\n}};")

# 脚本生成的图标数 (纯完美图标库来源)
N_GEN = len(MAP)
# 额外手工图标 (Meshtastic/USB蓝牙/USB网卡/运维助手/编程/修改机), 位于 icons_main_extra.inc
EXTRA = ["main_icon_meshtastic", "main_icon_usbbt", "main_icon_usbnet",
         "main_icon_winassist", "main_icon_code", "main_icon_modtool"]
N_EXTRA = len(EXTRA)
N_TOTAL = N_GEN + N_EXTRA

header = f"""/* ============================================================
 * 主菜单专用图标 (64x64 1bpp, MSB first)  由脚本批量生成
 * 来源: 桌面「完美图标」 (见 tools/image_convert/gen_main_icons_64.py)
 * 原生 64x64 嵌入, 不缩放.
 * 前 {N_GEN} 个由脚本从「完美图标」生成; 后 {N_EXTRA} 个 (Meshtastic/
 * USB蓝牙/USB网卡/运维助手/编程) 为手工图标, 定义见 icons_main_extra.inc.
 * ============================================================
 */
#define MAIN_ICON_W {W}
#define MAIN_ICON_H {H}
#define MAIN_ICON_BYTES (({W}*{H})/8)
#define MAIN_ICON_COUNT {N_TOTAL}

"""

body = "\n\n".join(arrays)
icon_list = "\n".join(f"    {v}," for v in var_names + EXTRA)
footer = f"""
const uint8_t *main_icons[MAIN_ICON_COUNT] = {{
{icon_list}
}};
"""

# 手工图标定义 (外部 .inc, 由 #include 引入, 脚本不重新生成).
# 必须位于 main_icons[] 表之前, 否则表引用不到这些变量.
extra_arr = f"""
/* ===== 手工图标 (icons_main_extra.inc, 非脚本生成) ===== */
#include "icons_main_extra.inc"
"""

# USB 数据传输图标: 独立数组 (不进入 main_icons, 供挂载/键鼠界面直接引用)
transfer_data = to_1bit(TRANSFER_PNG)
assert len(transfer_data) == bytes_per_icon, (TRANSFER_PNG, len(transfer_data))
transfer_arr = (
    "/* USB 数据传输图标 64x64 (挂载/键鼠同步界面专用) */\n"
    f"const uint8_t icon_transfer[MAIN_ICON_BYTES] = {{\n{fmt(transfer_data)}\n}};\n"
)

with open(DST, "w") as f:
    f.write(header + extra_arr + "\n" + body + "\n\n" + footer + "\n\n" + transfer_arr + "\n")
print(f"Wrote {DST}: {W}x{H}, {N_TOTAL} icons ({N_GEN} 脚本 + {N_EXTRA} 手工), {bytes_per_icon} B each")