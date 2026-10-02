#!/usr/bin/env python3
import serial, time, sys, glob
def detect():
    c=sorted(glob.glob('/dev/cu.usbmodem*')); return c[-1] if c else None
PORT=sys.argv[1] if len(sys.argv)>1 else detect()
ser=serial.Serial(PORT,115200,timeout=0.1)
t0=time.time()
def flush(t, keep):
    e=time.time()+t
    while time.time()<e:
        l=ser.readline()
        if l:
            s=l.decode('utf-8','replace').rstrip()
            if keep and (len(s)<200 or True): print(f"{time.time()-t0:6.1f} {s}", flush=True)
def reset():
    try: ser.setDTR(False); ser.setRTS(True); time.sleep(0.25); ser.setRTS(False)
    except: pass
def boot_until_conn(maxs):
    e=time.time()+maxs
    while time.time()<e:
        l=ser.readline()
        if l:
            s=l.decode('utf-8','replace').rstrip()
            if 'connected with' in s: 
                print(f"{time.time()-t0:6.1f} {s}", flush=True); return True
            if any(k in s for k in ['Guru','panic','assert','stack overflow','rst:0x','abort','LoadProhibited','StoreProhibited']):
                print(f"{time.time()-t0:6.1f} !!{s}", flush=True)
    return False

reset(); time.sleep(0.3)
# 进netdect触发连接
ser.write(b'page netdect\n')
if not boot_until_conn(18):
    print("[not connected, still try analysis via list]")
ser.write(b'key confirm\n'); flush(3, True)      # 若列表有选中 发起连接/启动
flush(13, False)                                 # 联网+信息(静默, 只打印保留关键行)
# 打印关键行 during info
print("=== after info, go scan ===")
ser.write(b'key confirm\n'); flush(1, True)      # →扫描
# 扫到设备即复panics; 全程打印
flush(40, True)
ser.close(); print("[done]")