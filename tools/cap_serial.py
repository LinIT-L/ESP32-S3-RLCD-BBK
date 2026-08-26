#!/usr/bin/env python3
"""抓取 ESP32 串口日志 (USB-Serial-JTAG). 用法: python3 tools/cap_serial.py [秒数]"""
import serial
import sys
import time

import os
PORT = os.environ.get('CAP_SERIAL_PORT', '/dev/cu.usbmodem1101')
BAUD = 115200
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0

ser = serial.Serial(PORT, BAUD, timeout=0.5)
ser.reset_input_buffer()
print(f'=== capture {DURATION:.0f}s from {PORT} ===')
t0 = time.time()
while time.time() - t0 < DURATION:
    try:
        data = ser.read(4096)
    except Exception as e:
        print(f'read error: {e}')
        time.sleep(0.5)
        continue
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
ser.close()
print('\n=== capture done ===')
