#!/usr/bin/env python3
"""One-shot: convert Desktop/电脑诊断 PNGs to 64x64 1bpp arrays.
Maps: DDEVNAME 0内存 1硬盘 2显卡 3主板 4电源 5系统.
"""
import os
from PIL import Image

SRC = "/Users/linit/Desktop/完美图标/应用管家/运维/电脑诊断"
W = H = 64
THRESHOLD = 128

MAP = [
    ("diag_icon_m2",  "m.2png.png"),   # 内存
    ("diag_icon_hdd", "硬盘.png"),
    ("diag_icon_gpu", "显卡.png"),
    ("diag_icon_mb",  "主板.png"),
    ("diag_icon_psu", "电源.png"),
    ("diag_icon_pc",  "电脑.png"),     # 系统
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
                if img.getpixel((x, y)) < THRESHOLD:
                    byte |= (1 << (7 - bit))
            out.append(byte)
    return out


rows = []
for name, fn in MAP:
    d = to_1bit(fn)
    hexs = ", ".join("0x%02X" % v for v in d)
    rows.append("const uint8_t %s[%d] = {\n    %s\n};" % (name, len(d), hexs))

names = ", ".join(m[0] for m in MAP)
rows.append("const uint8_t *diag_dev_icons[6] = { %s };" % names)

inc = "\n\n".join(rows) + "\n"
DST = "/Users/linit/AI项目/LinTOS0.2/components/os/pages/diag_dev_icons.inc"
open(DST, "w").write(inc)
print("written", DST, len(inc), "bytes")