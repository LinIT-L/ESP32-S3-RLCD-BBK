import serial, time, sys, re

PORT = "/dev/cu.usbmodem101"
BAUD = 115200
D = 90.0
OUT = "cap_now.log"
ansi = re.compile(rb"\x1b\[[0-9;]*m")

ser = serial.Serial(PORT, BAUD, timeout=0.2, rtscts=False, dsrdtr=False)
ser.rts = False
ser.dtr = False
ser.reset_input_buffer()

print("=== CAPTURE 90s -> %s ===" % OUT, flush=True)
print("请在此窗口内: 按手柄 上/下/左/右/A, 并进入 电子词典", flush=True)
t0 = time.time()
with open(OUT, "wb") as f:
    while time.time() - t0 < D:
        try:
            l = ser.readline()
            if l:
                clean = ansi.sub(b"", l).strip()
                if clean:
                    f.write(clean + b"\n")
                    f.flush()
                    s = clean.decode("utf-8", "replace")
                    # 只实时打印关心的行, 减少刷屏
                    if ("HID[" in s or "扫描" in s or "SD" in s or "映射" in s
                            or "词典" in s or "error" in s.lower() or "挂载" in s):
                        print(s)
                        sys.stdout.flush()
        except Exception as e:
            print("ERR", e)
            break
ser.close()
print("=== END (saved to %s) ===" % OUT, flush=True)
