#!/usr/bin/env python3
"""复位ESP32并读取启动+运行日志"""
import serial
import time
import sys

PORT = '/dev/cu.usbmodem101'
BAUD = 115200
DURATION = 30  # 秒

try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
    # 通过 DTR/RTS 硬复位 ESP32
    ser.dtr = False
    ser.rts = True   # EN=LOW (复位)
    time.sleep(0.1)
    ser.dtr = True    # BOOT=HIGH (正常启动)
    ser.rts = False   # EN=HIGH (释放复位)
    time.sleep(0.05)
    
    print(f'已复位 ESP32, 读取 {DURATION} 秒日志...')
    print('请等待启动完成后, 尝试连接蓝牙手柄!')
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
