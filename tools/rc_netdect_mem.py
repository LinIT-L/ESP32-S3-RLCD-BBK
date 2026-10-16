#!/usr/bin/env python3
# 复现 netdect 内存占用: 进入网络分析 -> 测 mem/stack
# 用法: python3 tools/rc_netdect_mem.py [port]
import serial, time, sys, glob

def detect():
    c = sorted(glob.glob('/dev/cu.usbmodem*'))
    return c[-1] if c else None

PORT = sys.argv[1] if len(sys.argv) > 1 else detect()
if not PORT:
    print("no port"); sys.exit(1)
ser = serial.Serial(PORT, 115200, timeout=0.1)

def flush(t):
    t0 = time.time(); out = []
    while time.time() - t0 < t:
        line = ser.readline()
        if line:
            out.append(line.decode('utf-8','replace').rstrip())
    return out

def send(cmd, wait=0.6):
    ser.write((cmd+'\n').encode()); time.sleep(wait)
    lines = flush(1.0)
    print("### >", cmd)
    for l in lines:
        if ('MEM' in l or 'HEAP' in l or 'STACK' in l or 'BTF' in l or 'ERR' in l
            or 'error' in l or 'restart' in l or 'panic' in l or 'assert' in l
            or 'os_push' in l or 'netdect' in l or 'ND_' in l or '内存' in l
            or 'free=' in l or '分配' in l or 'Malloc' in l or 'malloc' in l):
            print("   ", l)

# 先测桌面基线
send('mem', 0.5)
# 进入网络分析
send('page netdect', 1.2)
# 等扫描/分析任务起来
send('mem', 0.8)
send('blk', 0.6)
# 尝试往下一步(信息收集), 触发 http/tls
send('key confirm', 0.8)
send('wait 1500', 1.8)
send('mem', 0.8)
send('stack', 0.5)
ser.close()
print("[done]")