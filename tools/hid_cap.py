import serial, time, sys
PORT="/dev/cu.usbmodem1101"
BAUD=115200
LOG="/tmp/hid_full.log"
s=None
f=open(LOG,"ab")
def open_port():
    for _ in range(200):
        try:
            return serial.Serial(PORT, BAUD, timeout=0.1)
        except Exception:
            time.sleep(0.2)
    return None
print("capturing ->", LOG, flush=True)
n=0
while True:
    if s is None:
        s=open_port()
        if s is None:
            time.sleep(0.3); continue
    try:
        n=s.in_waiting
        if n:
            b=s.read(n); f.write(b); f.flush()
            sys.stdout.write(b.decode("utf-8","replace")); sys.stdout.flush()
    except Exception:
        try: s.close()
        except Exception: pass
        s=None
    time.sleep(0.05)