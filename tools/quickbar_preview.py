#!/usr/bin/env python3
"""quickbar_preview.py — 下拉快捷栏本机预览 (400×300 1bpp)

为什么有它: 改快捷栏布局时不该每次都烧机才能看对不对。
    几何    ← 逐条抄 components/system/os/services/svc_quickbar.c 的 QB_* 常量
    图标    ← 直接解析固件里的位图 (status_icons.inc / mp3_icons.h), 不手抄
    绘制    ← 复刻 qb_disc / qb_fill_round_rect / qb_ring_round_rect / qb_blit_icon

真实度: 逐像素对齐固件绘制逻辑; 不含 LCD 抖色。
用法:
    python3 tools/quickbar_preview.py                 # 默认音量 60 → /tmp/quickbar.png
    python3 tools/quickbar_preview.py --vol 20 --out /tmp/q.png
"""
import argparse
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ===== 几何常量: 与 svc_quickbar.c 保持一致 (改那里就要同步这里) =====
QB_H         = 40
QB_BTN_D     = 34
QB_BTN_R     = 5
QB_BTN_Y0    = 3
QB_BTN_LX    = 65
QB_BTN_RX    = 301
QB_VOL_X0    = 109
QB_VOL_X1    = 291
QB_SPK_X     = 109
QB_BAR_X0    = 155
QB_BAR_X1    = 291
QB_BAR_CY    = 20
QB_BAR_HW    = 5
QB_DOT_R     = 12
QB_PANEL_X0  = 57
QB_PANEL_X1  = 342

W, H = 400, 300


def load_c_array(path, symbol):
    """从 C 头/包含文件里抽出指定数组的所有 0xNN 字节。"""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    m = re.search(r"static\s+const\s+uint8_t\s+" + re.escape(symbol) + r"\s*\[[^]]*\]\s*=\s*\{(.*?)\};",
                  text, re.S)
    if not m:
        raise SystemExit(f"找不到数组 {symbol} @ {path}")
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


class Canvas:
    """400×300 灰度画布 (0=黑, 255=白), 纯标准库, 不依赖 Pillow。"""
    def __init__(self):
        self.buf = bytearray(b"\xff" * (W * H))

    def put(self, x, y, black=True):
        if 0 <= x < W and 0 <= y < H:
            self.buf[y * W + x] = 0 if black else 255

    def fill_rect(self, x0, y0, x1, y1, black=True):
        for y in range(max(0, y0), min(H - 1, y1) + 1):
            for x in range(max(0, x0), min(W - 1, x1) + 1):
                self.put(x, y, black)

    def disc(self, cx, cy, r, black=True):
        """与固件 qb_disc 同构: 每行按整数开方填横向矩形。"""
        for dy in range(-r, r + 1):
            dx = 0
            while (dx + 1) * (dx + 1) + dy * dy <= r * r:
                dx += 1
            self.fill_rect(cx - dx, cy + dy, cx + dx, cy + dy, black)

    def fill_round_rect(self, x0, y0, x1, y1, r, black=True):
        if r <= 0:
            self.fill_rect(x0, y0, x1, y1, black)
            return
        self.fill_rect(x0 + r, y0, x1 - r, y1, black)
        self.fill_rect(x0, y0 + r, x0 + r - 1, y1 - r, black)
        self.fill_rect(x1 - r + 1, y0 + r, x1, y1 - r, black)
        self.disc(x0 + r, y0 + r, r, black)
        self.disc(x1 - r, y0 + r, r, black)
        self.disc(x0 + r, y1 - r, r, black)
        self.disc(x1 - r, y1 - r, r, black)

    def ring_round_rect(self, x0, y0, x1, y1, r, black=True):
        self.fill_round_rect(x0, y0, x1, y1, r, black)
        self.fill_round_rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, max(0, r - 1), not black)

    def blit_1bpp(self, x, y, w, h, src, black=True):
        bpr = (w + 7) >> 3
        for dy in range(h):
            for dx in range(w):
                if src[dy * bpr + (dx >> 3)] & (0x80 >> (dx & 7)):
                    self.put(x + dx, y + dy, black)


def write_png(path, buf, w, h, scale=2):
    """极简 PNG 编码 (8bit 灰度, 无滤波), 只依赖 zlib/struct。"""
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = bytearray()
    for y in range(h):
        row = buf[y * w:(y + 1) * w]
        big = bytearray()
        for v in row:                 # 水平放大
            big.extend((v,) * scale)
        for _ in range(scale):        # 垂直放大
            raw.append(0)             # filter: none
            raw.extend(big)

    ihdr = struct.pack(">IIBBBBB", w * scale, h * scale, 8, 0, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol", type=int, default=60)
    ap.add_argument("--out", default="/tmp/quickbar.png")
    a = ap.parse_args()

    inc = os.path.join(ROOT, "components/system/os/services/status_icons.inc")
    mp3 = os.path.join(ROOT, "components/apps/pages/mp3_icons.h")
    bt_bmp = load_c_array(inc, "status_icon_usbbt_32x16")      # 32x16
    wifi_bmp = load_c_array(inc, "status_icon_wifi_16")        # 16x16
    vol_bmp = load_c_array(mp3, "mp3_icon_vol_32")             # 32x32

    c = Canvas()
    # 模拟一层"下方页面内容"(灰条纹), 用来看白底范围是否收窄
    for y in range(0, H, 8):
        c.fill_rect(0, y, W - 1, y + 1)

    # ===== 抽屉 =====
    c.fill_rect(QB_PANEL_X0, 0, QB_PANEL_X1, QB_H - 1, black=False)   # 白底
    c.fill_rect(QB_PANEL_X0, QB_H - 1, QB_PANEL_X1, QB_H - 1, black=True)  # 底边线

    # 蓝牙 / Wi-Fi: 34×34 R5 方块 (开=实心黑+白图标)
    for x, bmp, bw, bh, crop16 in ((QB_BTN_LX, bt_bmp, 32, 16, True),
                                   (QB_BTN_RX, wifi_bmp, 16, 16, False)):
        x0, x1 = x, x + QB_BTN_D - 1
        y0, y1 = QB_BTN_Y0, QB_BTN_Y0 + QB_BTN_D - 1
        cx, cy = x + QB_BTN_D // 2, QB_BTN_Y0 + QB_BTN_D // 2
        c.fill_round_rect(x0, y0, x1, y1, QB_BTN_R, black=True)
        if crop16:   # 蓝牙: 取原图中间 16 列 (裁剪, 不缩放)
            sx0 = (bw - 16) // 2
            sub = bytearray(2 * bh)
            for dy in range(bh):
                row = dy * ((bw + 7) >> 3)
                for dx in range(16):
                    if bmp[row + ((sx0 + dx) >> 3)] & (0x80 >> ((sx0 + dx) & 7)):
                        sub[dy * 2 + (dx >> 3)] |= (0x80 >> (dx & 7))
            c.blit_1bpp(cx - 8, cy - 8, 16, bh, bytes(sub), black=False)
        else:
            c.blit_1bpp(cx - bw // 2, cy - bh // 2, bw, bh, bmp, black=False)

    # 音量图标 (32×32)
    c.blit_1bpp(QB_SPK_X, QB_BAR_CY - 16, 32, 32, vol_bmp, black=True)

    # 滑轨三层 + 圆球
    cy = QB_BAR_CY
    c.fill_rect(QB_BAR_X0, cy - QB_BAR_HW - 3, QB_BAR_X1, cy + QB_BAR_HW + 3, black=False)
    c.disc(QB_BAR_X0, cy, QB_BAR_HW + 3, black=False)
    c.disc(QB_BAR_X1, cy, QB_BAR_HW + 3, black=False)

    c.fill_rect(QB_BAR_X0, cy - QB_BAR_HW, QB_BAR_X1, cy + QB_BAR_HW, black=True)
    c.disc(QB_BAR_X0, cy, QB_BAR_HW, black=True)
    c.disc(QB_BAR_X1, cy, QB_BAR_HW, black=True)

    c.fill_rect(QB_BAR_X0, cy - QB_BAR_HW + 1, QB_BAR_X1, cy + QB_BAR_HW - 1, black=False)
    c.disc(QB_BAR_X0, cy, QB_BAR_HW - 1, black=False)
    c.disc(QB_BAR_X1, cy, QB_BAR_HW - 1, black=False)

    vol = max(0, min(100, a.vol))
    dot = QB_BAR_X0 + (QB_BAR_X1 - QB_BAR_X0) * vol // 100
    c.disc(dot, cy, QB_DOT_R + 3, black=False)
    c.disc(dot, cy, QB_DOT_R, black=True)

    write_png(a.out, c.buf, W, H, scale=2)
    print(f"OK → {a.out}  (vol={vol}, 白底 {QB_PANEL_X0}..{QB_PANEL_X1}, "
          f"音量区 {QB_VOL_X0}..{QB_VOL_X1} 中心={(QB_VOL_X0 + QB_VOL_X1)//2})")


if __name__ == "__main__":
    main()
