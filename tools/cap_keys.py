import serial, time, sys, re

PORT = "/dev/cu.usbmodem101"
BAUD = 115200
D = 60.0
OUT = "cap_keys.log"
ansi = re.compile(rb"\x1b\[[0-9;]*m")

ser = serial.Serial(PORT, BAUD, timeout=0.2, rtscts=False, dsrdtr=False)
ser.rts = False
ser.dtr = False
ser.reset_input_buffer()

print("=== CAPTURE 60s -> %s ===" % OUT, flush=True)
print("请在 60 秒内: 进 手柄页 -> 映射按键, 然后依次按 上/下/左/右/确认(A)/返回(B)/退出到菜单(HOME)", flush=True)
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
                    print(clean.decode("utf-8", "replace"))
                    sys.stdout.flush()
        except Exception as e:
            print("ERR", e)
            break
ser.close()
print("=== END (saved to %s) ===" % OUT, flush=True)
