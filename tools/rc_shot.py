#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rc_shot.py — 用 remcon 远程拉起一个 Lua 应用并抓屏 (真机取证)。

为什么要有它
------------
`remcon` 的 `page` 命令列不出 Lua 页 (OS_PAGE_LUA 不在页表里), 所以跑 Lua 应用
只能靠应用管家手点 —— 没法自动化。但它有隐藏好用的 `app <lua路径>`:
设置 boot 路径 + 进 Lua 页, 等价于在应用管家点这一项。本脚本就是把它和
`fbdump` 串成"拉起 → 抓屏 → 落盘 PNG"一条命令。

⚠ 三个踩过的坑 (来自板级复核工具箱的复盘):
  1. fbdump 前必须 `loglevel 0`, 否则心跳日志插进 base64 → CRC 不符。
  2. 抓屏本身只读 (不碰 DTR/RTS, 不复位设备); 但 `app` 会**真的改变设备状态**。
  3. fb 位 1=白 0=黑; fb_png.py 会按这个极性还原。

用法
----
    python3 tools/rc_shot.py --app /store/wuxia.lua --keys "wait 900" --out /tmp/shot
"""
import argparse
import base64
import os
import re
import subprocess
import sys
import time
import zlib

import serial   # pyserial


def find_port():
    import glob
    ps = glob.glob("/dev/cu.usbmodem*")
    if not ps:
        raise SystemExit("没找到串口设备")
    return ps[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--app", default=None, help="要拉起的 Lua 路径, 如 /store/wuxia.lua")
    ap.add_argument("--keys", default="", help="额外动作, 分号分隔, 如 'wait 600; key ok'")
    ap.add_argument("--out", default="/tmp/rc_shot")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--boot", action="store_true", help="先复位设备 (会打断用户操作!)")
    a = ap.parse_args()

    port = a.port or find_port()
    if a.boot:
        s = serial.Serial(port, a.baud, timeout=0.3, dsrdtr=False, rtscts=False)
        s.setDTR(False); s.setRTS(True); time.sleep(0.2)
        s.setDTR(True);  s.setRTS(False); s.close()
        for _ in range(60):
            time.sleep(0.5)
            try:
                t = serial.Serial(port, a.baud, timeout=0.3, dsrdtr=False, rtscts=False)
                t.close(); break
            except Exception:
                pass
        time.sleep(2.0)

    # 只读打开: 绝不碰 DTR/RTS (那会物理复位设备)
    ser = serial.Serial(port, a.baud, timeout=0.3, dsrdtr=False, rtscts=False)
    time.sleep(0.3)
    ser.reset_input_buffer()
    log = []

    def send(cmd):
        ser.write((cmd + "\n").encode())
        ser.flush()
        time.sleep(0.25)

    def drain(sec):
        end = time.time() + sec
        while time.time() < end:
            n = ser.in_waiting
            if n:
                log.append(ser.read(n).decode("utf-8", "replace"))
            else:
                time.sleep(0.05)

    send("loglevel 0")           # ★ 先关日志, 否则 base64 会被心跳插断 → CRC 不符
    drain(0.4)
    if a.app:
        send("app " + a.app)
        drain(1.6)

    # keys 支持 `shot <名>`: 在动作序列中间插一帧抓屏 —— 排查"某步之后画面坏了"
    # 时必须能逐帧留证, 否则只能靠猜 (实测: "按返回后半屏黑"就是靠这个定位的)。
    import subprocess as _sp
    shots = []
    for k in [x.strip() for x in a.keys.split(";") if x.strip()]:
        if k.startswith("shot"):
            name = (k[4:].strip() or ("s%d" % (len(shots) + 1)))
            send("loglevel 0"); drain(0.2)
            log.append("\n===SHOT %s===\n" % name)
            for _try in range(3):
                send("fbdump"); drain(3.5)
                text = "".join(log)
                m = re.search(r"FB_BEGIN\s+(\d+)\s+(\d+)\s+(\d+)\s+([0-9a-fA-F]+)\s*\n(.*?)\r?\nFB_END",
                              text, re.S)
                if m and len(set(re.findall(r"[A-Za-z0-9+/=]+", m.group(5)))) > 0:
                    break
            send("loglevel 3"); drain(0.2)
            logp = "%s.%s.log" % (a.out, name)
            open(logp, "w", encoding="utf-8").write("".join(log))
            png = "%s.%s.png" % (a.out, name)
            r = _sp.run([sys.executable,
                         os.path.join(os.path.dirname(os.path.abspath(__file__)), "fb_png.py"),
                         logp, "-o", png, "--scale", "2"], capture_output=True, text=True)
            print("[%s] %s" % (name, (r.stdout or r.stderr).strip()))
            shots.append(png)
            continue
        send(k)
        drain(1.2)
    if shots:
        text = "".join(log)
        os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
        open(a.out + ".log", "w", encoding="utf-8").write(text)
        ser.close()
        for p in shots:
            print("✓ " + p)
        return

    # fbdump 偶发丢字节 (CRC 不符): 自动重抓最多 3 次。
    # 不要把 CRC 不符当"固件坏了" —— 先用项目自带的 fb_png.py 判据复验。
    ok = False
    for attempt in range(1, 4):
        log.append("\n===FBDUMP %d===\n" % attempt)
        send("fbdump")
        drain(3.5)
        text = "".join(log)
        m = re.search(r"FB_BEGIN\s+(\d+)\s+(\d+)\s+(\d+)\s+([0-9a-fA-F]+)", text)
        if m:
            body = text[m.end():text.find("FB_END", m.end())]
            b64 = "".join(re.findall(r"[A-Za-z0-9+/=]+", body))
            try:
                raw = base64.b64decode(b64 + "=" * (-len(b64) % 4))
            except Exception:
                raw = b""
            if len(raw) == int(m.group(3)) and (zlib.crc32(raw) & 0xFFFFFFFF) == int(m.group(4), 16):
                print("第 %d 次抓屏 CRC 通过" % attempt)
                ok = True
                break
            print("第 %d 次抓屏 CRC 不符 (收到 %d 字节), 重抓…" % (attempt, len(raw)))
        else:
            print("第 %d 次没抓到 FB_BEGIN, 重抓…" % attempt)
    if not ok:
        print("⚠ 三次抓屏都没通过校验 —— 看日志判断是丢字节还是设备卡住")
    send("loglevel 3")           # 恢复日志
    drain(0.3)
    ser.close()

    text = "".join(log)
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    logp = a.out + ".log"
    open(logp, "w", encoding="utf-8").write(text)

    m = re.search(r"FB_BEGIN\s+(\d+)\s+(\d+)\s+(\d+)\s+([0-9a-fA-F]+)", text)
    if not m:
        print("✗ 没抓到 FB_BEGIN —— 设备可能没在跑, 或 fbdump 被日志打断")
        print("  详见 " + logp)
        sys.exit(1)
    W, H, nbytes, crc = (int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4))
    body = text[m.end():text.find("FB_END", m.end())]
    b64 = "".join(re.findall(r"[A-Za-z0-9+/=]+", body))
    raw = base64.b64decode(b64 + "=" * (-len(b64) % 4))
    print("FB %dx%d 期望 %d 字节, 实收 %d 字节, crc=%s" % (W, H, nbytes, len(raw), crc))

    # 交给 fb_png.py 解码 (同一套布局约定, 不要自己另写一份)
    r = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "fb_png.py"),
                        logp, "-o", a.out + ".png", "--scale", "2"],
                       capture_output=True, text=True)
    print(r.stdout.strip() or r.stderr.strip())
    png = a.out + ".png"
    if os.path.exists(png):
        print("✓ " + png)
    # 顺带把关键日志打出来, 便于看有没有 Lua 报错
    for ln in text.splitlines():
        if any(k in ln for k in ("LUAAPP", "img.load", "ERR", "error", "找不到")):
            print("  | " + ln.strip())


if __name__ == "__main__":
    main()
