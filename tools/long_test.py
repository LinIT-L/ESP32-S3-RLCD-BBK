#!/usr/bin/env python3
"""LinTOS 整夜长测 v2 (raw fd, 不碰 DTR/RTS)

⚠ 必修: usb_hid / usb_bt / usb_net / winassist 四个页面会 os_hid_start(),
  把 USB 从 CDC 切成 HID 设备 —— **串口本身会消失**, 后续命令全部无效,
   看起来像"整机卡死"。所以巡检序列里必须排除它们, 单独低频率测。

用法: lintos_long2.py <port> <logfile> <total_min>
  - 读线程持续落盘
  - 每 10 min 轻量巡检 (mem/stack + main/settings/sd 往返)
  - 每 45 min 全页巡检 (排除 4 个 USB 页)
  - 每 3h 单独测一次 USB 页组, 测完等 20s 让 CDC 回来
"""
import os, sys, termios, threading, time, signal, re

port = sys.argv[1]
logpath = sys.argv[2]
total_min = float(sys.argv[3]) if len(sys.argv) > 3 else 480

LIGHT_IV = 10 * 60
FULL_IV = 45 * 60
USB_IV = 3 * 3600

PAGES = ["main", "select_game", "settings", "gamepad", "display", "time", "bt", "sd",
         "info", "btinfo", "key_config", "volume", "return_game", "mp3",
         "game_settings", "gb", "book", "wallpaper", "pomodoro",
         "app_manager", "storage", "gbc", "nes", "md", "sms", "arduboy",
         "diagnosis", "wqx", "vpet", "pal", "meshtasic", "whiteboard", "terminal",
         "sponsor", "modtool", "logdiag", "calc", "wifiprobe", "morse", "netdect",
         "fav", "sdr"]
USB_PAGES = ["usb_hid", "usb_bt", "usb_net", "winassist"]

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

logf = open(logpath, "ab", buffering=0)
summ = open(logpath + ".summary", "a", buffering=1)
lock = threading.Lock()
stop = {"v": False}
buf = {"mem": None, "stack": None}
bad = []           # panic / 栈溢出 / 断言 / 看门狗


def rd():
    while not stop["v"]:
        try:
            d = os.read(fd, 4096)
        except BlockingIOError:
            d = b""
        except OSError:
            time.sleep(0.3)
            continue
        if not d:
            time.sleep(0.05)
            continue
        txt = d.decode("utf-8", "replace")
        with lock:
            logf.write(d)
            for line in txt.replace("\r", "\n").split("\n"):
                if line.startswith("MEM "):
                    buf["mem"] = line.strip()
                elif line.startswith("STACK "):
                    buf["stack"] = line.strip()
                elif re.search(r"(panic|abort\(\)|stack overflow in task|assert failed|"
                               r"Guru Meditation|WATCHDOG|***ERROR***)", line, re.I):
                    bad.append("%s %s" % (time.strftime("%m-%d %H:%M:%S"), line.strip()[:160]))
                    with lock:
                        summ.write("!! %s\n" % line.strip()[:160])


def mark(s):
    with lock:
        logf.write(("\r\n[AUTO %s] %s\r\n" % (time.strftime("%m-%d %H:%M:%S"), s)).encode())


def send(cmd, wait=0.9):
    mark("> " + cmd)
    try:
        os.write(fd, (cmd + "\r\n").encode())
    except OSError:
        return
    t = time.time()
    while time.time() - t < wait and not stop["v"]:
        time.sleep(0.02)


def snap(tag):
    buf["mem"] = None
    buf["stack"] = None
    send("mem", 1.6)
    send("stack", 1.4)
    with lock:
        summ.write("%s | %-8s | %s | %s\n" % (time.strftime("%m-%d %H:%M"), tag,
                                              buf["mem"], buf["stack"]))


def light_pass(n):
    mark("LIGHT %d" % n)
    snap("light%d" % n)
    for p in ("settings", "sd", "main"):
        send("page " + p, 1.1)


def full_pass(n):
    mark("FULL %d start" % n)
    t0 = time.time()
    for p in PAGES:
        send("page " + p, 1.1)
    send("page main", 1.5)
    snap("full%d" % n)
    mark("FULL %d end (%.0fs)" % (n, time.time() - t0))


def usb_pass(n):
    mark("USBPASS %d start (串口会短暂消失属正常)" % n)
    for p in USB_PAGES:
        send("page " + p, 1.3)
    send("page main", 1.5)
    time.sleep(22)          # 等 os_hid_stop 把 CDC 还回来
    snap("usb%d" % n)
    mark("USBPASS %d end" % n)


th = threading.Thread(target=rd, daemon=True)
th.start()


def _sig(_s, _f):
    stop["v"] = True


signal.signal(signal.SIGTERM, _sig)
signal.signal(signal.SIGINT, _sig)

mark("longtest v2 start total=%.0fmin" % total_min)
end = time.time() + total_min * 60
n_l = n_f = n_u = 0
next_l = time.time() + 20
next_f = time.time() + 20
next_u = time.time() + USB_IV

while not stop["v"] and time.time() < end:
    now = time.time()
    if now >= next_f:
        n_f += 1
        full_pass(n_f)
        next_f = time.time() + FULL_IV
        next_l = time.time() + LIGHT_IV
    elif now >= next_u:
        n_u += 1
        usb_pass(n_u)
        next_u = time.time() + USB_IV
    elif now >= next_l:
        n_l += 1
        light_pass(n_l)
        next_l = time.time() + LIGHT_IV
    time.sleep(1.0)

mark("longtest exit light=%d full=%d usb=%d bad=%d" % (n_l, n_f, n_u, len(bad)))
stop["v"] = True
time.sleep(0.3)
with lock:
    logf.close()
    summ.close()
os.close(fd)
print("done light=%d full=%d usb=%d bad=%d" % (n_l, n_f, n_u, len(bad)))
