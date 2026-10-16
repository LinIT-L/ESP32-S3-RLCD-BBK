#!/usr/bin/env python3
"""聚焦复查: 开 VERBOSE 日志, 完整走一遍播放, 看功放/音量相关日志到底有没有。"""
import serial
import time

PORT = "/dev/cu.usbmodem101"
OUT = "/Users/linit/AI/LinTOS0.8/audio_probe2.log"

ser = serial.Serial(PORT, 115200, timeout=0.2)
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass
time.sleep(0.6)

chunks = []


def drain(sec):
    end = time.time() + sec
    out = bytearray()
    while time.time() < end:
        d = ser.read(8192)
        if d:
            out += d
    return out.decode("utf-8", "replace")


def send(cmd, wait=0.35):
    ser.write((cmd + "\n").encode())
    ser.flush()
    time.sleep(wait)


chunks.append(drain(0.8))
send("loglevel 5", 0.4);     chunks.append(drain(0.8))
send("page volume", 0.5);    chunks.append(drain(1.5))   # 先看音量页
send("key right", 0.3);      chunks.append(drain(0.6))   # 音量加
send("key right", 0.3);      chunks.append(drain(0.6))
send("key right", 0.3);      chunks.append(drain(0.6))
send("key back", 0.3);       chunks.append(drain(0.8))

send("page mp3", 0.5);       chunks.append(drain(1.5))
send("key confirm", 0.5);    chunks.append(drain(7.0))   # 播放
send("key back", 0.4);       chunks.append(drain(1.2))
send("loglevel 3", 0.4);     chunks.append(drain(1.0))
ser.close()

text = "".join(chunks)
with open(OUT, "w", encoding="utf-8") as f:
    f.write(text)
print("bytes=%d lines=%d -> %s" % (len(text), text.count("\n"), OUT))
