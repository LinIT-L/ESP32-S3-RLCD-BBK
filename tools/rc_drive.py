#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rc_drive.py — 通过串口 remcon 远程驱动设备 (注入命令 / 按键 / 抓屏), 并可选解出 PNG.

对标技能 lintos-lua-app-pipeline 的"远程回归"套路:
    注入命令 → 抓串口日志 → 从日志里解 fbdump → 看界面 + 看有没有 panic

用法:
    # 一条命令 (用 ';' 分隔多条, 设备端支持)
    python3 tools/rc_drive.py "page main ; wait 500 ; fbdump" -o /tmp/shot.png

    # 拉起 Lua 应用 + 按几下 + 抓屏, 同时保留完整日志
    python3 tools/rc_drive.py "app /store/apps/app/biossim.lua ; wait 1500 ; key down ; wait 300 ; fbdump" \
        -o /tmp/shot.png --log /tmp/cap.log

    # 只读日志, 不发任何命令
    python3 tools/rc_drive.py --read 5 --log /tmp/cap.log

⚠ 本脚本 **不动 DTR/RTS** (只读打开), 不会物理复位设备。
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial: 用 ~/.espressif/python_env/idf5.5_py3.13_env/bin/python3 跑")


def find_port(explicit=None):
    if explicit:
        return explicit
    c = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not c:
        sys.exit("✗ 找不到 /dev/cu.usbmodem* —— 设备可能在深睡/关机 (需按 PWR 唤醒)")
    return c[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmds", nargs="?", default="",
                    help="要注入的命令行 (';' 分隔多条)。留空 = 只读")
    ap.add_argument("-p", "--port", default=None)
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--read", type=float, default=3.0, help="注入后读多少秒 (默认 3)")
    ap.add_argument("--log", default=None, help="把原始日志写到该文件")
    ap.add_argument("-o", "--out", default=None, help="把最后一次 fbdump 解成 PNG 存这里")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    port = find_port(args.port)
    ser = serial.Serial(port, args.baud, timeout=0.3, dsrdtr=False, rtscts=False)
    ser.reset_input_buffer()

    if args.cmds:
        ser.write((args.cmds + "\r\n").encode())     # re,con 以 \r 或 \n 或 ';' 分行
        ser.flush()

    t0 = time.time()
    buf = []
    while time.time() - t0 < args.read:
        line = ser.readline()
        if line:
            buf.append(line.decode("utf-8", "replace").rstrip("\r\n"))
    ser.close()

    txt = "\n".join(buf)
    if args.log:
        with open(args.log, "w", encoding="utf-8") as f:
            f.write(txt + "\n")
        print("日志 → %s (%d 行)" % (args.log, len(buf)))

    if not args.quiet:
        # 只回显有信息量的行, 免得刷屏
        keep = [l for l in buf if re.search(r"FB_|panic|Guru|rst:0x|abort|assert|RC_ERR|ERROR|APP\]|慢|LUAAPP", l)]
        for l in keep[:40]:
            print("  " + l)

    if args.out:
        m = re.search(r"FB_BEGIN[^\n]*\n(.*?)FB_END", txt, re.S)
        if not m:
            print("⚠ 日志里没有完整的 fbdump (FB_BEGIN..FB_END) —— 没抓够时间? 检查 --read")
        else:
            here = os.path.dirname(os.path.abspath(__file__))
            p = subprocess.run([sys.executable, os.path.join(here, "fb_png.py"), "-",
                                "-o", args.out, "--scale", str(args.scale)],
                               input=txt, text=True, capture_output=True)
            print(p.stdout.strip() or p.stderr.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
