import serial, time, sys
PORT="/dev/cu.usbmodem101"
end=time.time()+14
buf=[]
try:
    s=serial.Serial(PORT,115200,timeout=0.3,dsrdtr=False,rtscts=False)
    while time.time()<end:
        d=s.read(4096)
        if d:
            try: buf.append(d.decode("utf-8","replace"))
            except Exception: pass
    s.close()
except Exception as e:
    print("SERIAL_ERR:",e); sys.exit(1)
txt="".join(buf)
open("boot_psram_bss.log","w").write(txt)
print("BYTES=%d"%len(txt))
low=txt.lower()
for k in ["panic","guru meditation","abort","assert","rebooting","rst:0x","stack overflow","cache disabled","loadprohibited","storeprohibited"]:
    print("%-18s %d"%(k, low.count(k)))
