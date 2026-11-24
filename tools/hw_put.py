#!/usr/bin/env python3
"""hw_put.py — 走 remcon 的 put 协议把本地文件上传到设备 TF 卡 (不触发复位).

协议 (见 main/remcon.c):
  主机: put <path> <总字节>\\n   → 设备回 RC_PUT_READY
        每 <=4096 字节一块          → 设备每块回 RC_PUT_ACK <已收>
        收满                       → 设备回 RC_PUT_DONE <字节> <crc32>

用法:
  python3 tools/hw_put.py <本地文件> <设备路径> [端口]
  python3 tools/hw_put.py "store/pkgs/矢量地图.lua" "/sdcard/apps/app/矢量地图.lua"
"""
import sys
import os
import time
import zlib
import glob
import re

try:
    import serial
except ImportError:
    print("缺少 pyserial: python3 -m pip install pyserial", flush=True)
    sys.exit(2)

ANSI = re.compile(rb"\x1b\[[0-9;]*m")
CHUNK = 4096


def pick_port(preferred: str) -> str:
    if preferred:
        return preferred
    cands = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not cands:
        print("未找到 /dev/cu.usbmodem* —— 设备没插或被占用", flush=True)
        sys.exit(2)
    return cands[0]


def read_until(ser, pat: bytes, timeout: float) -> bytes:
    t0 = time.time()
    buf = b""
    while time.time() - t0 < timeout:
        line = ser.readline()
        if not line:
            continue
        line = ANSI.sub(b"", line)
        buf += line
        if pat in line:
            return buf
    return buf


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, flush=True)
        return 2
    src = sys.argv[1]
    dst = sys.argv[2]
    port = pick_port(sys.argv[3] if len(sys.argv) > 3 else "")

    data = open(src, "rb").read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    print("PORT=%s  本地=%s (%d B)  设备=%s  crc32=%08X" % (port, src, len(data), dst, crc), flush=True)

    ser = serial.Serial(port, 115200, timeout=0.2, rtscts=False, dsrdtr=False)
    ser.dtr = True
    ser.rts = True
    time.sleep(0.15)
    ser.reset_input_buffer()

    ser.write(("put %s %d\n" % (dst, len(data))).encode())
    ser.flush()
    resp = read_until(ser, b"RC_PUT_READY", 5.0)
    if b"RC_PUT_READY" not in resp:
        print("设备未就绪, 回包: %r" % resp[-200:], flush=True)
        ser.close()
        return 1

    sent = 0
    for off in range(0, len(data), CHUNK):
        blk = data[off:off + CHUNK]
        ser.write(blk)
        ser.flush()
        sent += len(blk)
        ack = read_until(ser, b"RC_PUT_ACK", 10.0)
        if b"RC_PUT_ACK" not in ack:
            print("块 %d 无 ACK, 中止: %r" % (off, ack[-300:]), flush=True)
            ser.close()
            return 1
        # 设备端要 read 满 4096 才回 ACK; USB CDC 突发写太快会丢字节 ⇒ 留一点间隔
        time.sleep(0.008)
        if sent % (CHUNK * 16) == 0 or sent == len(data):
            print("  已发 %d / %d" % (sent, len(data)), flush=True)

    done = read_until(ser, b"RC_PUT_DONE", 10.0)
    txt = done.decode("utf-8", "replace").strip().splitlines()
    print("收尾: %s" % (txt[-1] if txt else "(无)"), flush=True)
    ok = ("RC_PUT_DONE" in done.decode("utf-8", "replace"))
    try:
        ser.dtr = True
        ser.rts = True
    except Exception:
        pass
    ser.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
