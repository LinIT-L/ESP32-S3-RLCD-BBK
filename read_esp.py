#!/usr/bin/env python3
# 后台读取 ESP32 串口日志
import glob, serial, sys, time

# 自动探测当前端口 (设备重枚举后端口号会漂移, 如 usbmodem101 -> usbmodem1101)
def detect_port():
    cands = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not cands:
        print("[[SERIAL_ERR]] 未找到串口 /dev/cu.usbmodem*", file=sys.stderr)
        sys.exit(1)
    if len(cands) > 1:
        print("[[SERIAL_WARN]] 发现多个端口, 取最新: %s" % cands[-1], file=sys.stderr)
    return cands[-1]

PORT = sys.argv[2] if len(sys.argv) > 2 else detect_port()
BAUD = 115200
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0

ser = serial.Serial(PORT, BAUD, timeout=0.1)
t0 = time.time()
try:
    while time.time() - t0 < DURATION:
        line = ser.readline()
        if line:
            try:
                print(line.decode('utf-8', 'replace').rstrip())
            except Exception:
                pass
        else:
            time.sleep(0.05)
except KeyboardInterrupt:
    pass
finally:
    ser.close()
print("[[SERIAL_END]]")