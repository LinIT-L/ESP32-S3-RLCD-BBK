#!/usr/bin/env python3
# 后台读取 ESP32 串口日志
import serial, sys, time

PORT = "/dev/cu.usbmodem101"
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