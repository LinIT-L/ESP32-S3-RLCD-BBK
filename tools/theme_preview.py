#!/usr/bin/env python3
"""theme_preview.py — 把 os_theme.c 的内置主题按**固件真实几何**渲染成本机预览图。

为什么要它: 加/改主菜单主题时不该靠烧机才能看效果。本工具把
  · 图标位图      ← components/ui/ui_common/icons_main.inc (固件 64×64 真位图, MSB first)
  · 主题参数      ← components/system/os/kernel/os_theme.c (直接解析, 不手抄)
  · 菜单可见顺序  ← components/apps/apps_registry.c (非 hidden 项)
  · 图标尺寸/位置 ← 与 page_main.c 的 main_icon_size()/相机公式逐条对齐
  · 标签          ← 与 main_draw_label() 同款: font_zh(24) 采样缩到 sz(=settings_font_size 档 3 → 22px)
全部在本机复现, 输出 400×300 PNG + 一张 2×2 汇总图。
真实度: 只差 LCD 抖色; 相机静止 (选中居中), 不含动画中间态。

用法:
    python3 tools/theme_preview.py                 # 全部主题 → ~/Desktop/LinTOS_Lua应用/sim_out/theme/
    python3 tools/theme_preview.py --center 9      # 指定居中的图标下标
    python3 tools/theme_preview.py --out /tmp/tt
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fw_fonts import FwFonts                      # noqa: E402
from PIL import Image                             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 400, 300
ICON_W = ICON_H = 64
ICON_BYTES = (ICON_W * ICON_H) // 8


# ---------- 解析固件数据 ----------
def load_icons():
    """icons_main.inc → {数组名: bytes}, 以及 main_icons[] 的下标顺序。"""
    src = open(os.path.join(ROOT, "components/ui/ui_common/icons_main.inc"),
               encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"const uint8_t (main_icon_\w+)\[MAIN_ICON_BYTES\]\s*=\s*\{(.*?)\};",
                         src, re.S):
        vals = re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))
        b = bytes(int(v, 16) for v in vals)
        if len(b) == ICON_BYTES:
            out[m.group(1)] = b
    om = re.search(r"const uint8_t \*main_icons\[MAIN_ICON_COUNT\]\s*=\s*\{(.*?)\};", src, re.S)
    order = re.findall(r"(main_icon_\w+)", om.group(1)) if om else []
    return out, order


def load_enum():
    """icons_main_enum.h → [(ICON_XXX, idx)]"""
    src = open(os.path.join(ROOT, "components/ui/ui_common/icons_main_enum.h"),
               encoding="utf-8").read()
    return [(m.group(1), int(m.group(2)))
            for m in re.finditer(r"(ICON_\w+)\s*=\s*(\d+)", src)]


def _cstr(t):
    """C 字面量 (含 \\xNN) → 真字符串。

    ⚠ 不能逐字节 chr() 再拼 (那会得到 latin-1 乱码): 中文在 C 里是 \\xe9\\xbb\\x98...
    必须先把转义还原成**字节**, 最后整体按 UTF-8 解码。
    """
    out = bytearray()
    i = 0
    while i < len(t):
        if t[i] == "\\" and i + 3 < len(t) and t[i + 1] == "x":
            out.append(int(t[i + 2:i + 4], 16))
            i += 4
        elif t[i] == "\\" and i + 1 < len(t):
            out.append(ord(t[i + 1]))
            i += 2
        else:
            out.extend(t[i].encode("utf-8"))
            i += 1
    return out.decode("utf-8", "replace")


def load_themes():
    """os_theme.c → 主题参数列表 (解析而非手抄, 保证与固件同源)。"""
    src = open(os.path.join(ROOT, "components/system/os/kernel/os_theme.c"),
               encoding="utf-8").read()
    body = src.split("s_themes[] = {", 1)[1].split("\n};", 1)[0]
    themes = []
    for blk in re.finditer(r"\{(.*?)\n    \}", body, re.S):
        t = blk.group(1)

        def g(key, dflt=None):
            m = re.search(r"%s\s*=\s*([^,\n]+)" % key, t)
            return m.group(1).strip() if m else dflt
        name = _cstr(re.search(r'\.name\s*=\s*"((?:[^"\\]|\\.)*)"', t).group(1))
        themes.append({
            "name": name,
            "spacing": int(g("icon_spacing", "100")),
            "center_y": g("icon_center_y", "UI_SCREEN_H / 2"),
            "center": int(g("icon_center", "100")),
            "normal": int(g("icon_normal", "64")),
            "label_dy": int(g("label_dy", "68")),
            "frame": "th_bg_frame" in (g("draw_bg", "") or ""),
        })
    return themes


def load_labels():
    """apps_registry.c → icon_idx → 中文显示名 (含 hidden 项, 只为预览好看)"""
    src = open(os.path.join(ROOT, "components/apps/apps_registry.c"), encoding="utf-8").read()
    enum = dict(load_enum())
    out = {}
    for m in re.finditer(r'\.label="((?:[^"\\]|\\.)*)",\s*\.icon_idx=(ICON_\w+)', src):
        out[enum.get(m.group(2), -1)] = _cstr(m.group(1))
    return out


# ---------- 渲染 (与 page_main.c 对齐) ----------
class Canvas:
    def __init__(self, white=True):
        self.im = Image.new("1", (W, H), 1 if white else 0)
        self.px = self.im.load()

    def fill(self, black=True):
        self.im.paste(0 if black else 1, (0, 0, W, H))

    def rect(self, x0, y0, x1, y1, k=1):
        for i in range(k):
            for x in range(x0 + i, x1 - i + 1):
                for y in (y0 + i, y1 - i):
                    if 0 <= x < W and 0 <= y < H:
                        self.px[x, y] = 0
            for y in range(y0 + i, y1 - i + 1):
                for x in (x0 + i, x1 - i):
                    if 0 <= x < W and 0 <= y < H:
                        self.px[x, y] = 0

    def blit_scaled(self, cx, cy, size, bmp):
        """等价 draw_main_icon_stretched: 最近邻缩放 64→size, 以中心定位"""
        if size <= 0:
            return
        x0, y0 = cx - size // 2, cy - size // 2
        for dy in range(size):
            sy = dy * ICON_H // size
            for dx in range(size):
                sx = dx * ICON_W // size
                on = bmp[sy * (ICON_W // 8) + (sx >> 3)] & (0x80 >> (sx & 7))
                if on:
                    x, y = x0 + dx, y0 + dy
                    if 0 <= x < W and 0 <= y < H:
                        self.px[x, y] = 0

    def label(self, cx, y, s, sz, fw):
        """等价 main_draw_label: 每格不透明 (黑底白字/白底黑字), 汉字 sz 宽, ASCII sz*16/24 宽"""
        def width(text):
            w = 0
            for ch in text:
                w += sz if ord(ch) > 0x7F else sz * 16 // 24
            return w
        w = width(s)
        x = cx - w // 2
        if x < 0:
            x = 0
        if x + w > W:
            x = W - w
        for ch in s:
            if ord(ch) > 0x7F:                          # 汉字: font_zh(24) → sz
                for dy in range(sz):
                    for dx in range(sz):
                        on = fw.zh24_bit(ch, dy * 24 // sz, dx * 24 // sz)
                        self.px[x + dx, y + dy] = 0 if on else 1
                x += sz
            else:                                       # ASCII: font_zh ascii 同源
                aw = sz * 16 // 24
                has = fw.has_zh24_ascii(ch)
                for dy in range(sz):
                    for dx in range(aw):
                        on = (fw.zh24_ascii_bit(ch, dy * 24 // sz, dx * 24 // aw) if has else
                              fw.ascii8x16_bit(ch, dy * 12 // sz, dx * 8 // aw, rows=12))
                        self.px[x + dx, y + dy] = 0 if on else 1
                x += aw


def icon_size(ad, big, sml):
    """照抄 page_main.c main_icon_size()"""
    if ad <= 0.5:
        return big
    if ad >= 1.0:
        return sml
    t = (ad - 0.5) / 0.5
    ease = t * t * (3 - 2 * t)
    return big - int((big - sml) * ease)


def render(theme, icons, order, labels, enum, center, fw):
    c = Canvas()
    if theme["frame"]:
        c.fill(False)                                   # 白底 (fill(True)=黑, 别搞反)
        c.rect(0, 0, W - 1, H - 1, 2)                   # th_bg_frame: 四周 2px 黑框
    else:
        c.fill(False)
    cy = H // 2 if theme["center_y"].startswith("UI_SCREEN_H") else int(theme["center_y"])
    cam = float(center)
    n = len(order)
    for g in range(int(cam) - 3, int(cam) + 4):
        idx = g % n
        cx = W // 2 + round((g - cam) * theme["spacing"])
        size = icon_size(abs(g - cam), theme["center"], theme["normal"])
        if cx + size > 0 and cx - size < W:
            c.blit_scaled(cx, cy, size, icons[order[idx]])
    # 标签: 只画选中项 (照抄 p_main_render 的 sel 标签)
    ly = cy + theme["label_dy"]
    if ly > H - 28:
        ly = H - 28
    lab = labels.get(center % n)
    if not lab:
        lab = enum[center % n][0].replace("ICON_", "") if center % n < len(enum) else "?"
    c.label(W // 2, ly, lab, 22, fw)
    return c.im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.expanduser("~/Desktop/LinTOS_Lua应用/sim_out/theme"))
    ap.add_argument("--center", type=int, default=4, help="居中(选中)的图标下标")
    ap.add_argument("--scale", type=int, default=2)
    args = ap.parse_args()

    fw = FwFonts()
    icons, order = load_icons()
    enum = load_enum()
    labels = load_labels()
    themes = load_themes()
    os.makedirs(args.out, exist_ok=True)
    print("图标 %d 个 / 主题 %d 个 / 居中下标 %d (%s)"
          % (len(order), len(themes), args.center,
             labels.get(args.center, enum[args.center][0] if args.center < len(enum) else "?")))
    shots = []
    for t in themes:
        im = render(t, icons, order, labels, enum, args.center, fw)
        p = os.path.join(args.out, "theme_%s.png" % t["name"])
        im.convert("L").resize((W * args.scale, H * args.scale), Image.NEAREST).save(p)
        shots.append(p)
        print("  %-6s 格距%-4d 中央%-4d 两侧%-4d 标签+%-3d %s"
              % (t["name"], t["spacing"], t["center"], t["normal"], t["label_dy"],
                 " +四周黑框" if t["frame"] else ""))
    # 2×2 汇总
    cols = 2
    rows = (len(shots) + cols - 1) // cols
    sw, sh = W * args.scale, H * args.scale
    sheet = Image.new("L", (sw * cols + 8 * (cols + 1), sh * rows + 8 * (rows + 1)), 255)
    for i, p in enumerate(shots):
        r, cc = divmod(i, cols)
        sheet.paste(Image.open(p), (8 + cc * (sw + 8), 8 + r * (sh + 8)))
    sp = os.path.join(args.out, "themes_2x2.png")
    sheet.save(sp)
    print("汇总图: %s" % sp)


if __name__ == "__main__":
    main()
