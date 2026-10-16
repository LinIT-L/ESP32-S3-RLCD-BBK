#!/usr/bin/env python3
# 驱动 netdect 走到扫描步, 抓选 ICMP/扫描日志, 判断裸 ICMP 收包是否失效
import serial, time, sys, glob
def detect():
    c=sorted(glob.glob('/dev/cu.usbmodem*')); return c[-1] if c else None
PORT=sys.argv[1] if len(sys.argv)>1 else detect()
ser=serial.Serial(PORT,115200,timeout=0.1)
t0=time.time()
def rel(t):
    return f"{t-t0:6.2f}"
def out(s,tag=""):
    if any(k in s for k in ['WIFI_MANAGER','扫描','发现有','主机扫描','netdect','[gw]','[pub]','[info','ping','ICMP','icmp','socket','raw','ESP_ERR','state:','connected','push ->','正在扫描','失败']):
        print(rel(time.time()), tag, s)
def flush(t):
    e=time.time()+t
    while time.time()<e:
        l=ser.readline()
        if l: out(l.decode('utf-8','replace').rstrip())
def send(c):
    ser.write((c+'\n').encode()); print(rel(time.time()), ">> send", c)

# 进网络分析
send('page netdect'); flush(4.0)
# 网络页(已联网): 确认 → 启动分析
send('key confirm'); flush(4.0)
# 等信息(TLS, 最长~12s)完成并暂停
flush(14.0)
# 下一步 到扫描
send('key confirm'); flush(2.0)
# 抓扫描 ~40s(254台 串行 ping)
flush(40.0)
ser.close(); print("[done]")