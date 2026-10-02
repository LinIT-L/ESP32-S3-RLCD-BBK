#!/usr/bin/env python3
import serial, time, sys
PORT = "/dev/cu.usbmodem1101"
BAUD = 115200
OUT = "/Users/linit/AI项目/LinTOS0.6/build/cheat_crash.log"
DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 180
s = serial.Serial(PORT, BAUD, timeout=0.1)
f = open(OUT, "wb")
end = time.time() + DUR
while time.time() < end:
    d = s.read(2048)
    if d:
        f.write(d); f.flush()
s.close(); f.close()
print("done")