#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fb_png.py — 把设备串口输出的 `fbdump` 抓屏数据解成 PNG (纯标准库, 无第三方依赖)

配套固件: main/remcon.c 的 `fbdump` 命令
--------------------------------------------------------------------
设备侧输出格式:
    FB_BEGIN 400 300 15000 <crc32-8hex>
    <base64, 每行 76 字符, 共 264 行>
    FB_END

帧缓冲不是逐行 1bpp, 而是 ST7305 面板原生的"2 列 × 4 行 / 字节"打包
(见 components/drivers/st7305/st7305.c 的 st7305_draw_pixel):
    idx = (x >> 1) * 75 + ((299 - y) >> 2)      # 75 = H/4, 共 200 个字节列
    bit = 7 - ((((299 - y) & 3) << 1) | (x & 1))
    位 = 1 → 白 (反射式 LCD 背景), 位 = 0 → 黑 (笔画)
y 是上下翻转的: 字节行 0 对应屏幕最底部 4 行。

用法
--------------------------------------------------------------------
    # 从串口日志里解出最后一张图 (默认放大 2 倍便于查看)
    python3 tools/fb_png.py cap.log -o shot.png

    # 一次跑出多张 (每个应用一张), 全部解出来
    python3 tools/fb_png.py cap.log --all -o out/  --scale 2

    # 从标准输入读
    cat cap.log | python3 tools/fb_png.py - -o shot.png

    # 只在终端看 ASCII 预览 (不开图)
    python3 tools/fb_png.py cap.log --ascii

退出码: 0 正常; 1 没找到数据; 2 数据损坏 (长度/CRC 不符)
"""
import argparse
import base64
import binascii
import os
import re
import struct
import sys
import zlib

W_DEF, H_DEF = 400, 300
FB_BYTES_DEF = (W_DEF // 2) * (H_DEF // 4)      # 200 × 75 = 15000
B64_LINE = 76

BEGIN_RE = re.compile(rb"FB_BEGIN\s+(\d+)\s+(\d+)\s+(\d+)\s+([0-9a-fA-F]{8})")
END_RE = re.compile(rb"FB_END")


# ------------------------------------------------------------------ 抓取数据块
def extract_blocks(data: bytes):
    """从日志里抽出所有 FB_BEGIN..FB_END 块 → [(w, h, nbytes, crc, payload_bytes)]"""
    blocks = []
    pos = 0
    while True:
        m = BEGIN_RE.search(data, pos)
        if not m:
            break
        w, h, n = int(m.group(1)), int(m.group(2)), int(m.group(3))
        crc = int(m.group(4), 16)
        body_start = m.end()
        e = END_RE.search(data, body_start)
        if not e:
            break                      # 不完整 (抓屏被截断)
        body = data[body_start:e.start()]
        # 去掉空白, 拼回 base64
        b64 = re.sub(rb"\s+", b"", body)
        try:
            payload = base64.b64decode(b64, validate=False)
        except (binascii.Error, ValueError):
            pos = e.end()
            continue
        blocks.append((w, h, n, crc, payload))
        pos = e.end()
    return blocks


# ------------------------------------------------------------------ 解码像素
def decode_fb(payload: bytes, w: int, h: int):
    """ST7305 打包 → 逐行像素 (每行 w 个 0/255 灰度值, 行序 = 从上到下)。"""
    stride = h // 4                    # 75
    cols = w // 2                      # 200
    rows_img = []
    for y in range(h):
        inv_y = h - 1 - y
        by = inv_y >> 2
        b = (inv_y & 3) << 1
        base = by
        row = bytearray(w)
        for x in range(w):
            byte = payload[(x >> 1) * stride + base]
            bit = 7 - (b | (x & 1))
            row[x] = 255 if (byte >> bit) & 1 else 0
        rows_img.append(bytes(row))
    return rows_img


# ------------------------------------------------------------------ 无依赖 PNG
def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png_gray(path: str, w: int, h: int, rows, scale: int = 1):
    """写 8-bit 灰度 PNG; scale>1 用最近邻放大 (每像素方块)。"""
    if scale > 1:
        up = []
        for row in rows:
            wide = bytearray()
            for v in row:
                wide += bytes([v]) * scale
            for _ in range(scale):
                up.append(bytes(wide))
        rows = up
        w, h = w * scale, h * scale
    raw = b"".join(b"\x00" + row for row in rows)
    png = (b"\x89PNG\r\n\x1a\n"
           + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
           + _png_chunk(b"IDAT", zlib.compress(raw, 9))
           + _png_chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


# ------------------------------------------------------------------ ASCII 预览
def ascii_preview(rows, w: int, h: int):
    """终端里的粗预览: 每 2×4 像素合成 1 个字符, 宽字符块用 '█' 这类画。"""
    out = []
    for ty in range(0, h, 4):
        line = []
        for tx in range(0, w, 2):
            dark = 0
            for y in range(ty, min(ty + 4, h)):
                for x in range(tx, min(tx + 2, w)):
                    if rows[y][x] == 0:
                        dark += 1
            line.append(" .:+#@@"[min(dark, 5)] if dark else " ")
        out.append("".join(line))
    return "\n".join(out)


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description="把串口 fbdump 抓屏数据解成 PNG")
    ap.add_argument("log", help="串口日志文件, 或 '-' 表示从 stdin 读")
    ap.add_argument("-o", "--out", default="fb_shot.png",
                    help="输出 PNG 路径; 配 --all 时视为输出目录 (默认 fb_shot.png)")
    ap.add_argument("--scale", type=int, default=2, help="放大倍数 (默认 2)")
    ap.add_argument("--all", action="store_true", help="解出全部帧, 而不是只解最后一张")
    ap.add_argument("--ascii", action="store_true", help="额外打印终端 ASCII 预览")
    ap.add_argument("--raw", help="同时把原始 15000 字节写到该文件")
    args = ap.parse_args()

    if args.log == "-":
        data = sys.stdin.buffer.read()
    else:
        with open(args.log, "rb") as f:
            data = f.read()

    blocks = extract_blocks(data)
    if not blocks:
        print("[错误] 日志里没有找到 FB_BEGIN..FB_END 抓屏块。")
        print("       请确认: ① 设备已跑带 fbdump 的固件 ② 发过 'fbdump' 命令 ③ 日志被完整保存")
        return 1

    bad = 0
    # 默认只解**最后一张** (脚本通常抓了多帧, 想看的是最终画面); --all 才全解
    todo = list(enumerate(blocks)) if args.all else [(len(blocks) - 1, blocks[-1])]
    for i, (w, h, n, crc, payload) in todo:
        tag = "[第 %d/%d 帧]" % (i + 1, len(blocks))
        if len(payload) != n:
            print("%s ❌ 长度不符: 期望 %d, 实际 %d" % (tag, n, len(payload)))
            bad += 1
            continue
        real = zlib.crc32(payload) & 0xFFFFFFFF
        if real != crc:
            print("%s ❌ CRC 不符: 头里 %08x, 实算 %08x (串口丢字节)" % (tag, crc, real))
            bad += 1
            continue

        rows = decode_fb(payload, w, h)

        if args.all:
            os.makedirs(args.out, exist_ok=True)
            path = os.path.join(args.out, "fb_%03d.png" % (i + 1))
        else:
            path = args.out
        write_png_gray(path, w, h, rows, args.scale)
        print("%s ✅ %dx%d  CRC %08x → %s (放大 %dx)"
              % (tag, w, h, crc, path, args.scale))

        if args.raw:
            rp = args.raw if not args.all else args.raw + ".%03d" % (i + 1)
            with open(rp, "wb") as f:
                f.write(payload)
            print("        原始帧缓冲 → %s" % rp)

        if args.ascii:
            print()
            print(ascii_preview(rows, w, h))
            print()

    return 2 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
