#!/usr/bin/env python3
# 更长时间抓取: 关注扫描启动/完成/以及连接后 netdect 状态
import serial, time, sys, glob
def detect():
    c=sorted(glob.glob('/dev/cu.usbmodem*')); return c[-1] if c else None
PORT=sys.argv[1] if len(sys.argv)>1 else detect()
ser=serial.Serial(PORT,115200,timeout=0.1)
t0=time.time()
def log(line):
    s=line.decode('utf-8','replace').rstrip()
    if any(k in s for k in ['WIFI_MANAGER','扫描','扫描已','扫描完成','扫描启动','scan','netdect','push ->','connect','connected','已连接','正在扫描','CONFIRM','nd_start','STATE','auth','assoc','ESP_ERR','失败','deauth','discon']):
        print(f"[{time.time()-t0:6.2f}] {s}")
ser.write(b'page netdect\n')
e=time.time()+25
while time.time()<e:
    l=ser.readline()
    if l: log(l)
ser.close()
print("[done]")