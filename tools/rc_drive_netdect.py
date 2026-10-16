#!/usr/bin/env python3
# 用 remcon 远程驱动 netdect: 连发多次 confirm 一路推进到 IP 扫描,
# 全程连续抓串口不中断, 复现/定位"扫描IP重启"。
import serial, time

PORT = "/dev/cu.usbmodem1101"
BAUD = 115200
OUT = "<HOME>/AI项目/LinTOS0.6/build/netdect_drive.log"

s = serial.Serial(PORT, BAUD, timeout=0.3)
f = open(OUT, "wb")

def rd(n):
    t = time.time(); out = b''
    while time.time() - t < n:
        d = s.read(4096)
        if d:
            out += d; f.write(d); f.flush()
    return out.decode(errors='replace')

def send(cmd):
    s.write((cmd + "\n").encode()); time.sleep(0.15)

def wait_for(patterns, timeout):
    t = time.time(); acc = ""
    while time.time() - t < timeout:
        d = s.read(4096)
        if d:
            acc += d.decode(errors='replace'); f.write(d); f.flush()
        for p in patterns:
            if p in acc:
                return p
    return None

print("[1] page netdect"); send("page netdect"); rd(2.5)
print("[2] wait wifi connect")
r = wait_for(["wifi:connected with", "已连接"], 12); print("    wifi ->", r); rd(1.5)
print("[3] push through engine with repeated confirm")
for i in range(8):
    send("key confirm")
    print(f"    confirm #{i+1}")
    r = wait_for(["[found]", "stack overflow", "Rebooting", "Guru", "Backtrace"], 6)
    if r:
        print(f"    early match -> {r}")
        break
    # 若已进入扫描或暂停, 保持抓取
    rd(2)
print("[4] keep capturing 40s")
r = wait_for(["[found]", "stack overflow", "Rebooting", "Guru", "Backtrace"], 40)
print("    final ->", r)
rd(8)
print("[5] done")
f.close(); s.close(); print("saved:", OUT)
