import serial, time
s = serial.Serial('/dev/cu.usbmodem101', 115200, timeout=0.3)
t0 = time.time()
while time.time() - t0 < 90:
    d = s.read(4096)
    if not d: continue
    txt = d.decode('utf-8', 'replace')
    for line in txt.splitlines():
        if "STACK main_free" in line or "TOUCH: 诊断" in line:
            continue
        print("[%.0fs] %s" % (time.time()-t0, line[:240]), flush=True)
        if any(k in line for k in ["Backtrace", "Guru Meditation", "abort", "panic", "set(app"]:
            print("### CRASH DETECTED ###", flush=True)
s.close()