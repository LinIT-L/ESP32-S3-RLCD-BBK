#!/usr/bin/env python3
"""复现脚本: 持续读串口, 按时间线注入命令, 不漏任何日志"""
import os, sys, termios, time, threading

port = sys.argv[1]
out = sys.argv[2]
cmds = sys.argv[3:]          # 形式 "秒:命令"

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
iflag, oflag, cflag, lflag, ispeed, ospeed, cc = a
cflag &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE | termios.HUPCL)
cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
iflag &= ~(termios.IXON | termios.IXOFF | termios.IXANY | termios.INLCR |
           termios.IGNCR | termios.ICRNL)
lflag &= ~(termios.ICANON | termios.ECHO | termios.ECHONL | termios.ISIG | termios.IEXTEN)
oflag &= ~termios.OPOST
cc[termios.VMIN] = 1
cc[termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])

f = open(out, "wb", buffering=0)
stop = {"v": False}
t0 = time.time()


def rd():
    while not stop["v"]:
        try:
            d = os.read(fd, 4096)
        except BlockingIOError:
            d = b""
        except OSError:
            f.write(b"\n[[PORT LOST]]\n")
            time.sleep(0.2)
            continue
        if d:
            f.write(d)
        else:
            time.sleep(0.03)


threading.Thread(target=rd, daemon=True).start()
plan = []
for c in cmds:
    t, cmd = c.split(":", 1)
    plan.append((float(t), cmd))
plan.sort()
i = 0
dur = (plan[-1][0] + 15) if plan else 30
while time.time() - t0 < dur:
    if i < len(plan) and time.time() - t0 >= plan[i][0]:
        f.write(("\n[[SEND %.1fs]] %s\n" % (time.time() - t0, plan[i][1])).encode())
        try:
            os.write(fd, (plan[i][1] + "\r\n").encode())
        except OSError:
            f.write(b"[[WRITE FAIL]]\n")
        i += 1
    time.sleep(0.05)
stop["v"] = True
time.sleep(0.2)
f.close()
os.close(fd)
print("done, %.0fs" % (time.time() - t0))
