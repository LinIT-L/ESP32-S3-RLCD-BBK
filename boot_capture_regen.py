import serial, time, sys

PORT = '/dev/cu.usbmodem101'
BAUD = 115200

# 1) 等待 USB CDC 重新枚举
ser = None
for _ in range(80):
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.3, dsrdtr=False, rtscts=False)
        break
    except Exception as e:
        time.sleep(0.4)
if ser is None:
    print("PORT_NOT_FOUND"); sys.exit(2)

# 2) reset pulse: EN=LOW 然后 HIGH
ser.setDTR(False); ser.setRTS(True); time.sleep(0.2)
ser.setDTR(True);  ser.setRTS(False)
time.sleep(1.5)
ser.close()

# 3) 重新打开并读 boot 日志 ~8s
ser = serial.Serial(PORT, BAUD, timeout=0.5, dsrdtr=False, rtscts=False)
t0 = time.time(); lines = []
while time.time() - t0 < 8:
    raw = ser.readline()
    if raw:
        try:
            lines.append(raw.decode(errors='replace').rstrip())
        except Exception:
            pass
ser.close()

with open('/Users/linit/AI/LinTOS0.6/boot_regen.log', 'w') as f:
    f.write('\n'.join(lines))

print(f"CAPTURED {len(lines)} lines")
for ln in lines[:90]:
    print(ln)
