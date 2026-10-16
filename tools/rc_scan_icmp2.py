#!/usr/bin/env python3
# 复位→等WiFi连上→进netdect→启动分析→过信息→到扫描步抓ICMP日志
import serial, time, sys, glob
def detect():
    c=sorted(glob.glob('/dev/cu.usbmodem*')); return c[-1] if c else None
PORT=sys.argv[1] if len(sys.argv)>1 else detect()
ser=serial.Serial(PORT,115200,timeout=0.1)
t0=time.time()
def rel(t): return f"{t-t0:6.2f}"
def out(s,tag=""):
    if any(k in s for k in ['WIFI_MANAGER','扫描','发现有','主机扫描','netdect','[gw]','[pub]','[info','ping','ICMP','icmp','状态','socket','raw','ESP_ERR','state:','connected','连接','push ->','正在扫描','失败','run']):
        print(rel(time.time()), tag, s)
def flush(t):
    e=time.time()+t
    while time.time()<e:
        l=ser.readline()
        if l: out(l.decode('utf-8','replace').rstrip())
def reset():
    try: ser.setDTR(False); ser.setRTS(True); time.sleep(0.25); ser.setRTS(False)
    except: pass
def boot_wait_conn(maxs):
    e=time.time()+maxs
    while time.time()<e:
        l=ser.readline()
        if l:
            s=l.decode('utf-8','replace').rstrip()
            if 'connected with' in s or 'WIFI_EVENT_STA_CONNECTED' in s:
                out('CONN-OK'); return True
            if any(k in s for k in ['WIFI_MANAGER','wifi:state','run','GOT_IP']): out(s)
    return False

def wait_connected(maxs):
    e=time.time()+maxs
    while time.time()<e:
        l=ser.readline()
        if l:
            s=l.decode('utf-8','replace').rstrip()
            if 'connected with' in s or 'WIFI_EVENT_STA_CONNECTED' in s:
                return True
            if any(k in s for k in ['WIFI_MANAGER','wifi:state','run','GOT_IP','失败','连接']): out(s)
    return False

reset(); time.sleep(0.3); flush(3.0)
print(rel(time.time()), "== 进netdect(会触发连WiFi)")
ser.write(b'page netdect\n')
if not wait_connected(15):
    print(rel(time.time()), "!! 进netdect后仍未连上, 退出"); ser.close(); sys.exit(2)
print(rel(time.time()), "== 已连上, 确认启动分析")
ser.write(b'key confirm\n'); flush(4.0)
flush(14.0)                                  # 联网+信息(公网TLS最长~12s)
ser.write(b'key confirm\n'); flush(1.0)      # 下一步→扫描
flush(45.0)                                  # 抓扫描
ser.close(); print("[done]")