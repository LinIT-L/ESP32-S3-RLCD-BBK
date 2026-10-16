#!/usr/bin/env python3
"""被动串口读取: 干净复位 ESP32 (仅 RTS 脉冲, 不拉 boot 脚) 并捕获启动日志."""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
DURATION = int(sys.argv[2]) if len(sys.argv) > 2 else 18

ser = serial.Serial(PORT, 115200, timeout=0.2)
# 干净复位: 拉低 RTS 再释放 (不碰 DTR, 避免进入下载模式)
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.1)

deadline = time.time() + DURATION
buf = bytearray()
last_ts = time.time()
while time.time() < deadline:
    try:
        data = ser.read(512)
    except Exception as e:
        print(f"[read error] {e}")
        break
    if data:
        buf += data
        last_ts = time.time()
        try:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
        except Exception:
            pass
        sys.stdout.flush()
    # 如果很久没有新数据 (设备已稳定运行且不再打印), 提前结束
    if time.time() - last_ts > 6 and len(buf) > 200:
        print("\n[静默 6s, 设备已稳定运行]")
        break

ser.close()
text = buf.decode("utf-8", errors="replace")
print("\n===== 诊断统计 =====")
for kw in ["assert failed", "Backtrace", "Rebooting", "Guru Meditation", "abort()", "rst:0xc", "后台引擎初始化完成", "进入菜单系统", "CORE DUMP"]:
    if kw in text:
        print(f"  [出现] {kw}")
print("[完成]")
