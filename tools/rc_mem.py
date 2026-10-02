#!/usr/bin/env python3
# 通过 remcon 串口定位内部 RAM 占用:
#   - 复位设备 -> 等待 boot 完成 -> 发送 mem / blk / help
# 用法: python3 tools/rc_mem.py [port]
import serial, time, sys, glob

def detect():
    c = sorted(glob.glob('/dev/cu.usbmodem*'))
    return c[-1] if c else None

PORT = sys.argv[1] if len(sys.argv) > 1 else detect()
if not PORT:
    print("no port"); sys.exit(1)
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=0.1)

def flush(t):
    t0 = time.time(); out = []
    while time.time() - t0 < t:
        line = ser.readline()
        if line:
            s = line.decode('utf-8', 'replace').rstrip(); out.append(s)
    return out

try:
    ser.setDTR(False)
    ser.setRTS(True); time.sleep(0.25)
    ser.setRTS(False)
except Exception as e:
    print("reset skipped:", e)

time.sleep(0.2); flush(1.0)

ser.write(b'hello\n'); time.sleep(0.4)
t0 = time.time()
while time.time() - t0 < 3:
    line = ser.readline()
    if line:
        s = line.decode('utf-8', 'replace').rstrip()
        print("[boot]", s)
        if 'READY' in s or 'REMOCON' in s: break

def send(cmd, wait=0.8):
    ser.write((cmd + '\n').encode()); time.sleep(wait)
    lines = flush(1.0)
    print("### >", cmd)
    for l in lines: print("   ", l)

send('mem'); send('blk'); send('help')
ser.close()