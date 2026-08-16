import serial, time, sys

PORT = "/dev/cu.usbmodem101"
BAUD = 115200
DURATION = 20.0

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.2, rtscts=False, dsrdtr=False)
    # 避免打开瞬间拉低 RTS/DTR 导致设备复位
    ser.rts = False
    ser.dtr = False
    ser.reset_input_buffer()
except Exception as e:
    print("OPEN FAIL:", e)
    sys.exit(1)

print("=== CAPTURE %s (20s) ===" % PORT, flush=True)
t0 = time.time()
while time.time() - t0 < DURATION:
    try:
        line = ser.readline()
        if line:
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()
    except Exception as e:
        print("READ ERR:", e)
        break
ser.close()
print("=== CAPTURE END ===", flush=True)
