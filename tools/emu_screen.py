#!/usr/bin/env python3
"""LinTOS 模拟器带屏运行 (esp-emu + 帧解码)

用法:
  python3 tools/emu_screen.py                 # 跑模拟器, 弹出实时窗口 (400x300), Ctrl+C 退出
  python3 tools/emu_screen.py --png out.png   # 无窗口, 每帧覆盖写 out.png, 跑 30s 自动退出

依赖: ~/.local/bin/esp-emu; 固件: build/emu_flash.bin (merge 好的 16MB 镜像)
帧格式: 'LTFB' + u16 w + u16 h + w*h/8 字节, 位序与 st7305_draw_pixel 一致
"""
import base64
import os
import struct
import subprocess
import sys
import time

EMU = os.path.expanduser("~/.local/bin/esp-emu")
FW = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "emu_flash.bin"))
# ★ 2026-10-30: --fw 可指定固件 (模拟器构建产物在 build_emu/emu_flash.bin)
if "--fw" in sys.argv:
    FW = os.path.abspath(sys.argv[sys.argv.index("--fw") + 1])
MAGIC = b"LTFB"


# 每个 fb 字节 = 2x4 像素块 (x0,x0+1) × 4 行, bit7..bit0 顺序见 st7305_draw_pixel。
# 预生成 256 项查表: byte -> 8 个 (行内偏移k, 颜色) —— 行内偏移在放置时按 y 平移。
def _build_patch_table():
    tbl = []
    for byte in range(256):
        px = []  # (k, j, on)  k=0..3 行号, j=0..1 列, on=True 表示白
        for k in range(4):
            for j in range(2):
                bit = 7 - (k * 2 + j)
                px.append((k, j, bool((byte >> bit) & 1)))
        tbl.append(px)
    return tbl

_PATCH = _build_patch_table()


def decode_pixels(fb, w, h):
    """fb -> bytearray 灰度 (w*h, 0=黑 255=白), 查表版"""
    stride = h // 4
    pix = bytearray(b"\xff" * (w * h))
    for i, byte in enumerate(fb):
        col, grp = divmod(i, stride)
        x0 = col * 2
        for k, j, on in _PATCH[byte]:
            if not on:
                pix[(h - 1 - (grp * 4 + k)) * w + x0 + j] = 0
    return pix


def latest_frame(buf):
    """返回 (w, h, fb) 或 None"""
    i = buf.rfind(MAGIC)
    if i < 0 or len(buf) < i + 8:
        return None
    w, h = struct.unpack_from("<HH", buf, i + 4)
    need = w * h // 8
    if not (0 < w <= 800 and 0 < h <= 800) or len(buf) < i + 8 + need:
        return None
    return w, h, buf[i + 8:i + 8 + need]


def main():
    png_mode = "--png" in sys.argv
    out_path = sys.argv[sys.argv.index("--png") + 1] if png_mode else None
    # ★ 2026-10-30: png 模式让模拟器自己带 --timeout 退出 —— 否则 Popen 一直活着,
    #   read() 等不到 EOF, 脚本卡死。
    cmd = [EMU, "--chip", "esp32s3", "--firmware", FW]
    if png_mode:
        cmd += ["--timeout", "32s"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    buf = b""
    frames = 0
    last_decode = 0.0
    deadline = time.time() + 30 if png_mode else None
    try:
        while True:
            if png_mode and time.time() > deadline:
                break
            # ★ 2026-10-30: 必须用 read1 —— read(n) 会一直阻塞到凑满 n 字节或 EOF,
            #   而整段输出只有 ~57KB (< 64KB) 且模拟器不退出 ⇒ 永远读不到任何一帧。
            d = proc.stdout.read1(1 << 16) if hasattr(proc.stdout, "read1") else proc.stdout.read(1 << 16)
            if d:
                buf += d
            now = time.time()
            if now - last_decode < 0.5:
                continue
            last_decode = now
            fr = latest_frame(buf)
            if fr is None:
                continue
            w, h, fb = fr
            pix = decode_pixels(fb, w, h)
            frames += 1
            if png_mode:
                import zlib
                raw = b"".join(b"\x00" + bytes(pix[y * w:(y + 1) * w]) for y in range(h))
                def chunk(tag, data):
                    c = struct.pack(">I", len(data)) + tag + data
                    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
                ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)
                open(out_path, "wb").write(
                    b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                    + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
            else:
                import tkinter as tk
                ppm = b"P5\n%d %d\n255\n" % (w, h) + bytes(pix)
                root = tk._default_root
                if root is None:
                    root = tk.Tk()
                    root.title("LinTOS @ esp-emu")
                    root.configure(bg="white")
                    root.geometry(f"{w}x{h}")
                    lbl = tk.Label(root, bg="white")
                    lbl.pack()
                    root._lbl = lbl
                    root._img = None
                img = tk.PhotoImage(data=base64.b64encode(ppm), format="ppm")
                root._img = img
                root._lbl.config(image=img)
                root.update()
    except KeyboardInterrupt:
        pass
    finally:
        proc.kill()
    print(f"共渲染 {frames} 帧" + (f", 最后画面: {out_path}" if png_mode else ""))


if __name__ == "__main__":
    main()
