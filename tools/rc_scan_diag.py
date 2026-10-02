#!/usr/bin/env python3
# 进入网络分析, 抓取 wifi_manager / netdect 关键日志(带时间戳), 判断扫描卡点
import serial, time, sys, glob

def detect():
    c = sorted(glob.glob('/dev/cu.usbmodem*')); return c[-1] if c else None
PORT = sys.argv[1] if len(sys.argv) > 1 else detect()
ser = serial.Serial(PORT, 115200, timeout=0.1)

t0 = time.time()
def log(line):
    dt = time.time() - t0
    s = line.decode('utf-8','replace').rstrip()
    # 只留相关行
    if any(k in s for k in ['WIFI_MANAGER','SCAN','扫描','Wi-Fi','WiFi','wifi','netdect','NETDECT','nd_','esp_wifi','ESP_WIFI','WIFI:','scan','HTTP_CLIENT','SNTP','NTP','promisc','BOOT','connect','CONNECT','STATE']):
        print(f"[{dt:6.2f}] {s}")

# 复位到干净态
try:
    ser.setDTR(False); ser.setRTS(True); time.sleep(0.25); ser.setRTS(False)
except: pass
time.sleep(0.3)
def flush(t):
    e=time.time()+t
    while time.time()<e:
        l=ser.readline()
        if l: log(l)
flush(2.0)   # boot

# 进入网络分析
ser.write(b'page netdect\n')
# 抓 10 秒
flush(10.0)
ser.close()
print("[done]")