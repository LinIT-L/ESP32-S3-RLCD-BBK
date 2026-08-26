import serial, time, sys
ser = serial.Serial('/dev/cu.usbmodem101', 115200, timeout=0.1)
# io0 = DTR (GPIO0), EN = RTS
# 复位: 拉 EN
ser.setDTR(False)
ser.setRTS(True)   # EN low
time.sleep(0.25)
ser.setRTS(False)  # EN high -> 复位释放
time.sleep(0.1)
# 抓 12 秒
t0 = time.time()
while time.time() - t0 < 12:
    line = ser.readline()
    if line:
        s = line.decode('utf-8', 'replace').rstrip()
        print(s)
ser.close()
