#!/usr/bin/env python3
"""maintenance.py — 设备自治维护驱动 (remcon 远程控制台).

把"握手/读日志/下命令/复位抓开机/崩溃监视"收敛成一个命令, 供 AI 全程自治诊断维修.
同一根 USB 串口(/dev/cu.usbmodem*)既读日志又下 remcon 命令.

用法:
  python3 maintenance.py head                    -> 握手 + 在线确认
  python3 maintenance.py cmd  "<一行;用;分号"     -> 下发 remcon 命令并回读 (page/key/tap/...)
  python3 maintenance.py logs  <秒> [all|crash|wifi]   -> 读 N 秒日志
  python3 maintenance.py reboot <秒>             -> EN(RTS) 复位 + 抓开机日志
  python3 maintenance.py crash <秒>              -> 崩溃监视 (panic/backtrace/stack overflow/assert)
  python3 maintenance.py port                    -> 打印自动探测到的端口
"""
import sys, time, glob, serial

def detect_port():
    cands = sorted(glob.glob('/dev/cu.usbmodem*'))
    if not cands:
        return None
    return cands[-1]

PORT = detect_port() or '/dev/cu.usbmodemXXXX'
BAUD = 115200

CRASH = ['assert failed', 'guru meditation', 'stack overflow', 'backtrace',
         'panic()', 'abort()', 'A stack overflow', 'Guru Meditation', 'rst:0x8']

def open_serial():
    for _ in range(80):
        try:
            s = serial.Serial(PORT, BAUD, timeout=0.2)
            s.reset_input_buffer()
            return s
        except Exception:
            time.sleep(0.5)
    return None

def handshake(s):
    for _ in range(80):
        try:
            s.write(b'hello\n'); s.flush()
        except Exception:
            pass
        try:
            if b'REMOCON_READY' in s.read(256):
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return False

def drain(s, buf):
    end = time.time() + 0.6
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
            end = time.time() + 0.6
        else:
            time.sleep(0.03)
    return buf

def read_for(s, sec):
    buf = b''
    end = time.time() + sec
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
        else:
            time.sleep(0.03)
    return buf

def reset_en():
    """用 DTR/RTS 拉 EN 复位 (io0=DTR, EN=RTS). 需 pc 兼容时序."""
    s = serial.Serial(PORT, BAUD, timeout=0.1)
    s.setDTR(False)
    s.setRTS(True)     # EN low
    time.sleep(0.25)
    s.setRTS(False)    # EN high -> 释放复位
    s.close()

def report(txt, filt):
    lines = txt.splitlines()
    hit = [ln for ln in lines if any(k.lower() in ln.lower() for k in CRASH)]
    if filt == 'crash':
        for ln in hit:
            print('##', ln[:240], flush=True)
    elif filt in ('wifi', 'net'):
        kw = ['wifi', 'connected', 'netdect', 'scan', 'sta ']
        for ln in lines:
            low = ln.lower()
            if any(k in low for k in kw):
                print('[%.0f]' % (time.time()-0), ln[:200], flush=True)
    else:
        for ln in lines[-220:]:
            print(ln[:240], flush=True)
    # 汇总
    print('--- SUMMARY: crash_hits=%d ---' % len(hit), flush=True)
    for ln in hit[:8]:
        print('  ##', ln[:200], flush=True)

def watch_crash(s, sec):
    t0 = time.time()
    while time.time() - t0 < sec:
        d = s.read(4096)
        if not d:
            time.sleep(0.03); continue
        txt = d.decode('utf-8', 'replace')
        for ln in txt.splitlines():
            if 'STACK main_free' in ln or 'TOUCH: 诊断' in ln:
                continue
            print('[%.1fs] %s' % (time.time()-t0, ln[:240]), flush=True)
            if any(k.lower() in ln.lower() for k in CRASH):
                print('### CRASH DETECTED ###', flush=True)

def cmd_driver(s, line):
    s.write((line + '\n').encode()); s.flush()
    time.sleep(min(0.8 + 0.3 * len(line), 3))
    buf = drain(s, b'')
    return buf.decode('utf-8', 'replace')

def main():
    argv = sys.argv[1:]
    mode = argv[0] if argv else 'head'
    if mode == 'port':
        print('PORT=%s' % PORT); return 0
    s = open_serial()
    if not s:
        print('PORT_FAIL=%s' % PORT); return 1
    ok = handshake(s)
    print('PORT=%s READY=%s CMD=%s' % (PORT, ok, mode), flush=True)
    if not ok:
        s.close(); return 1
    if mode == 'head':
        pass
    elif mode == 'cmd':
        line = argv[1] if len(argv) > 1 else 'help'
        print(cmd_driver(s, line), flush=True)
    elif mode == 'logs':
        sec = float(argv[1]) if len(argv) > 1 else 15
        filt = argv[2] if len(argv) > 2 else 'all'
        report(read_for(s, sec).decode('utf-8', 'replace'), filt)
    elif mode == 'reboot':
        sec = float(argv[1]) if len(argv) > 1 else 12
        s.close()
        try:
            reset_en()
        except Exception as e:
            print('RESET_ERR=%s' % e, flush=True)
        s = open_serial()
        report(read_for(s, sec).decode('utf-8', 'replace'), 'all')
    elif mode == 'crash':
        sec = float(argv[1]) if len(argv) > 1 else 60
        watch_crash(s, sec)
    elif mode == 'scenario':
        sec = float(argv[2]) if len(argv) > 2 else 10
        filt = argv[3] if len(argv) > 3 else 'all'
        print(cmd_driver(s, argv[1]), flush=True)
        report(read_for(s, sec).decode('utf-8', 'replace'), filt)
    else:
        print('USAGE: head|port|cmd|logs|reboot|crash|scenario'); s.close(); return 2
    s.close()
    return 0

if __name__ == '__main__':
    sys.exit(main())