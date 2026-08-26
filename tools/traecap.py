import sys, time
import serial
PORT="/dev/cu.usbmodem1101"
BAUD=115200
LOG="build/traeserial.log"
s=serial.Serial(PORT, BAUD, timeout=0.2)
print("capturing %s@%d -> %s" % (PORT, BAUD, LOG), flush=True)
buf=b""
with open(LOG,"ab") as f:
    while True:
        try:
            n=s.in_waiting
            if n:
                b=s.read(n)
                f.write(b); f.flush()
                buf+=b
                while b"\n" in buf:
                    line,buf = buf.split(b"\n",1)
                    line=line.rstrip(b"\r")
                    try: print(line.decode("utf-8","replace"), flush=True)
                    except Exception: pass
        except Exception as e:
            try: s.close()
            except Exception: pass
            time.sleep(0.2)
            try: s=serial.Serial(PORT, BAUD, timeout=0.2)
            except Exception: pass
            continue
        time.sleep(0.05)