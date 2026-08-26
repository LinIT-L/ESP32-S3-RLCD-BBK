#!/usr/bin/env python3
"""复位 ESP32 (via 下载口 DTR/RTS) 然后从运行口 cu.debug-console 读取启动日志"""
import serial
import time
import sys

RESET_PORT = '/dev/cu.usbmodem101'
CONSOLE_PORT = '/dev/cu.debug-console'
BAUD = 115200
DURATION = 25  # 秒

def reset_via(port):
    try:
        ser = serial.Serial(port, BAUD, timeout=1)
        ser.dtr = False
        ser.rts = True    # EN=LOW 复位
        time.sleep(0.1)
        ser.dtr = True    # BOOT=HIGH
        ser.rts = False   # EN=HIGH 释放
        time.sleep(0.05)
        ser.close()
        return True
    except Exception as e:
        print(f'复位口打开失败 {port}: {e}')
        return False

print('复位 ESP32 ...')
reset_via(RESET_PORT)
time.sleep(2.5)  # 等待 USB-CDC 重新枚举为 cu.debug-console

try:
    ser = serial.Serial(CONSOLE_PORT, BAUD, timeout=1)
    print(f'已打开 {CONSOLE_PORT}, 读取 {DURATION} 秒日志...')
    print('=' * 60)
    start = time.time()
    while time.time() - start < DURATION:
        data = ser.read(4096)
        if data:
            try:
                sys.stdout.write(data.decode('utf-8', errors='replace'))
                sys.stdout.flush()
            except Exception:
                pass
    ser.close()
    print()
    print('=' * 60)
    print('日志采集结束')
except Exception as e:
    print(f'串口错误 ({CONSOLE_PORT}): {e}')
