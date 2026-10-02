import serial, time, sys

PORT = "/dev/cu.usbmodem1101"
BAUD = 115200
OUT = "/Users/linit/AI/LinTOS0.6/diag_sleep_capture.txt"
DURATION = 1200  # 20 min

KEYWORDS = ("reset_reason", "Backtrace", "Guru", "abort", "Reboot", "PANIC",
            "panic", "WDT", "Brownout", "brownout", "assert", "Assert",
            "esp_restart", "sleep", "Sleep", "wake", "Wake", "关蓝牙", "蓝牙",
            "bt_core", "controller", "light_sleep", "deep_sleep", "sleep_request",
            "BT", "WiFi", "wifi", "断开", "息屏", "屏保", "软关机")

try:
    s = serial.Serial(PORT, BAUD, timeout=0.3, dsrdtr=False, rtscts=False)
    # 只读: DTR=True/RTS=False 避免物理复位设备(项目约定)
    s.dtr = True
    s.rts = False
except Exception as e:
    print("OPEN ERR:", e); sys.exit(1)

print("=== capture start ===", flush=True)
t0 = time.time()
with open(OUT, "w", buffering=1) as f:
    f.write("=== sleep diag capture %s ===\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
    while time.time() - t0 < DURATION:
        try:
            raw = s.readline()
        except Exception as e:
            line = "<<read err: %s>>" % e
            print(line, flush=True); f.write(line + "\n"); break
        if not raw:
            continue
        try:
            txt = raw.decode(errors="replace").rstrip()
        except Exception:
            continue
        f.write(txt + "\n")
        if any(k in txt for k in KEYWORDS):
            print("[%6.0fs] %s" % (time.time() - t0, txt), flush=True)
    s.close()
print("=== capture done, wrote", OUT, "===", flush=True)
