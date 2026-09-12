#!/usr/bin/env python3
"""把桌面「修改机.png / mini修改机.png」转为 1bpp C 数组.

- 修改机.png  (64x64)  → 追加到 components/ui_common/icons_main_extra.inc (main_icon_modtool)
- mini修改机.png (状态栏) → 32x16 用于 svc_modtool (modtool_icon 数组, 单独 .inc)

极性与 gen_main_icons_64.py 一致: 暗(<阈值)=1=黑, 透明/白=0.
用法: 只打印 C 数组文本, 由人工/脚本粘贴到对应 .inc.
"""
import os
from PIL import Image

SRC = "/Users/linit/Desktop/完美图标"
THRESHOLD = 128


def to_1bit_bytes(path, w, h):
    img = Image.open(os.path.join(SRC, path)).convert("RGBA")
    if img.size != (w, h):
        img = img.resize((w, h), Image.Resampling.LANCZOS)
    px = img.load()
    out = []
    for y in range(h):
        for b in range(w // 8):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                r, g, bl, a = px[x, y]
                # 透明 → 0; 非透明且整体暗 → 1
                if a >= 128 and r < THRESHOLD and g < THRESHOLD and bl < THRESHOLD:
                    byte |= (1 << (7 - bit))
            out.append(byte)
    return out


def fmt(data, per_line=12):
    lines = []
    for i in range(0, len(data), per_line):
        vals = [f"0x{b:02X}" for b in data[i:i + per_line]]
        lines.append("    " + ", ".join(vals) + ",")
    return "\n".join(lines)


# --- 64x64 主菜单/应用图标 ---
d64 = to_1bit_bytes("应用管家/独立游戏/修改机.png", 64, 64)
assert len(d64) == (64 * 64) // 8, len(d64)
print(f"/* 修改机 (main_icon_modtool) 64x64 — {len(d64)}B */")
print(f"const uint8_t main_icon_modtool[MAIN_ICON_BYTES] = {{")
print(fmt(d64))
print("};")

# --- 24x24 状态栏 mini 图标 (源 PNG 原生 24x24, 一比一不缩放) ---
W = H = 24
d24 = to_1bit_bytes("状态栏/mini修改机.png", 24, 24)
assert len(d24) == (24 * 24) // 8, len(d24)
print(f"\n/* mini 修改机 (svc_modtool 状态栏) 24x24 — {len(d24)}B, 源 PNG 一比一 */")
print(f"#define MODTOOL_MINI_W 24")
print(f"#define MODTOOL_MINI_H 24")
print(f"const uint8_t modtool_mini_icon[] = {{")
print(fmt(d24))
print("};")
