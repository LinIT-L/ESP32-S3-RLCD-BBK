import serial, time, sys

PORT = "/dev/cu.debug-console"
BAUD = 115200
DURATION = 30.0

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
except Exception as e:
    print("OPEN FAIL:", e)
    sys.exit(1)

print("=== CAPTURE START (30s) ===", flush=True)
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
