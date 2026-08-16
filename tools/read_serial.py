#!/usr/bin/env python3
"""读取 ESP32 串口日志"""
import serial
import time
import sys

PORT = '/dev/cu.usbmodem1101'
BAUD = 115200
DURATION = 20  # 秒

try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
    print(f'串口已打开 {PORT}, 读取 {DURATION} 秒日志...')
    print('请在此期间尝试连接蓝牙手柄!')
    print('=' * 60)
    start = time.time()
    while time.time() - start < DURATION:
        data = ser.read(4096)
        if data:
            try:
                text = data.decode('utf-8', errors='replace')
                sys.stdout.write(text)
                sys.stdout.flush()
            except:
                pass
    ser.close()
    print()
    print('=' * 60)
    print('日志采集结束')
except Exception as e:
    print(f'串口错误: {e}')
