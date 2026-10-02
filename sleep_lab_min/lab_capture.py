import serial, time, sys

PORT = "/dev/cu.usbmodem1101"
BAUD = 115200
DURATION = 100
IDLE_REOPEN = 3.0          # 超过 N 秒没数据 -> 强制重开端口(应对 USB 重新枚举)
OUT = "/Users/linit/AI/LinTOS0.6/sleep_lab_min/lab_capture.txt"

f = open(OUT, "w", buffering=1)
f.write("=== sleep_lab capture %s ===\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
print("=== capture start (force-reopen on idle) ===", flush=True)

t0 = time.time()
s = None
last_data = time.time()

def open_port():
    try:
        p = serial.Serial(PORT, BAUD, timeout=0.3, dsrdtr=False, rtscts=False)
        print("[%5.0fs] port opened" % (time.time() - t0), flush=True)
        return p
    except Exception as e:
        print("[%5.0fs] open fail: %s" % (time.time() - t0, e), flush=True)
        return None

while time.time() - t0 < DURATION:
    if s is None:
        s = open_port()
        if s is None:
            time.sleep(0.5)
            continue
        last_data = time.time()

    # 空闲太久 -> 认为 USB 断过/已重枚举, 强制重开
    if time.time() - last_data > IDLE_REOPEN:
        try:
            s.close()
        except Exception:
            pass
        s = None
        time.sleep(0.3)
        continue

    try:
        raw = s.readline()
    except Exception as e:
        print("[%5.0fs] read err: %s -> reopen" % (time.time() - t0, e), flush=True)
        try:
            s.close()
        except Exception:
            pass
        s = None
        time.sleep(0.3)
        continue

    if not raw:
        continue

    last_data = time.time()
    try:
        txt = raw.decode(errors="replace").rstrip()
    except Exception:
        continue
    f.write(txt + "\n")
    print("[%5.0fs] %s" % (time.time() - t0, txt), flush=True)

try:
    if s:
        s.close()
except Exception:
    pass
f.close()
print("=== capture done ->", OUT, "===", flush=True)
