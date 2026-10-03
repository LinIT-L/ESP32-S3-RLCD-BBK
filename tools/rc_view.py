#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rc_view.py — 把设备屏幕**实时**投到电脑上看。

原理: 循环发 remcon 的 `fbdump` 把 1bpp 帧缓冲抓回来 (实测一趟 0.27s ≈ **3.6 帧/秒**),
      在内存里解码成 PNG, 由一个小 HTTP 服务吐给浏览器自动刷新 —— 于是你在电脑上
      就能"看着设备屏幕"操作。

用法:
    python3 tools/rc_view.py                 # 网页版 → 浏览器打开 http://127.0.0.1:8765/
    python3 tools/rc_view.py --window        # 桌面窗口版 (tkinter)
    python3 tools/rc_view.py --scale 1       # 1:1 (400x300); 默认 2 → 800x600
    python3 tools/rc_view.py -p /dev/cu.usbmodem1101

⚠ 只读打开串口, **不动 DTR/RTS** (不会物理复位设备)。
⚠ 运行期间**不要再开别的串口 reader**(会抢数据, 两边都拿不全)。
"""
import argparse
import base64
import glob
import io
import os
import re
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial: 用 ~/.workbuddy/binaries/python/envs/default/bin/python 跑")

from PIL import Image

BEGIN_RE = re.compile(rb"FB_BEGIN\s+(\d+)\s+(\d+)\s+(\d+)\s+([0-9a-fA-F]{8})")
END_RE = re.compile(rb"FB_END")


def find_port(explicit=None):
    if explicit:
        return explicit
    cands = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not cands:
        sys.exit("✗ 找不到 /dev/cu.usbmodem* —— 设备可能在深睡/关机 (按 PWR 唤醒)")
    return cands[0]


def open_serial(port, baud=115200):
    """只读打开 + **在 open 之前**就把 DTR/RTS 置位, 避免开串口时把板子拉复位。"""
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.15
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    s.open()
    return s


def decode_fb(payload: bytes, w: int, h: int) -> Image.Image:
    """ST7305 1bpp 打包 → PIL 灰度图。

    布局: idx = (x>>1)*(h//4) + ((h-1-y)>>2)
          bit = 7 - ((((h-1-y) & 3) << 1) | (x & 1))
          该位为 1 ⇒ 白 (255), 0 ⇒ 黑 (0)
    """
    stride = h // 4
    data = bytearray(w * h)
    n = len(payload)
    for y in range(h):
        inv_y = h - 1 - y
        base = inv_y >> 2
        bb = (inv_y & 3) << 1
        off = y * w
        for x in range(w):
            i = (x >> 1) * stride + base
            if i < n and (payload[i] >> (7 - (bb | (x & 1)))) & 1:
                data[off + x] = 255
    return Image.frombytes("L", (w, h), bytes(data))


class Capture(threading.Thread):
    """后台一直抓帧; 线程安全地暴露最新 PNG 字节。"""

    def __init__(self, port, scale, interval=0.05):
        super().__init__(daemon=True)
        self.port, self.scale, self.interval = port, scale, interval
        self.lock = threading.Lock()
        self.png = None
        self.wh = (0, 0)
        self.frames = 0
        self.fails = 0
        self.fps = 0.0
        self.stop = False
        self.ser = None

    def run(self):
        try:
            self.ser = open_serial(self.port)
        except Exception as e:
            print("[串口打开失败] %s" % e, file=sys.stderr)
            return
        t_last = time.time()
        f_last = 0
        while not self.stop:
            try:
                self.ser.reset_input_buffer()
                self.ser.write(b"fbdump\r\n")
                buf = bytearray()
                t0 = time.time()
                while time.time() - t0 < 3.0:
                    chunk = self.ser.read(8192)
                    if chunk:
                        buf += chunk
                    if b"FB_END" in buf:
                        break
                m = BEGIN_RE.search(bytes(buf))
                if not m:
                    self.fails += 1
                    time.sleep(self.interval)
                    continue
                w, h = int(m.group(1)), int(m.group(2))
                body = bytes(buf[m.end():])
                e = END_RE.search(body)
                if not e:
                    self.fails += 1
                    time.sleep(self.interval)
                    continue
                payload = base64.b64decode(re.sub(rb"\s+", b"", body[:e.start()]), validate=False)
                if len(payload) < (w * h) // 8:
                    self.fails += 1
                    time.sleep(self.interval)
                    continue
                im = decode_fb(payload, w, h)
                if self.scale != 1:
                    im = im.resize((w * self.scale, h * self.scale), Image.NEAREST)
                bio = io.BytesIO()
                im.save(bio, format="PNG", optimize=False)
                with self.lock:
                    self.png = bio.getvalue()
                    self.wh = (w, h)
                    self.frames += 1
                now = time.time()
                if now - t_last >= 1.0:
                    self.fps = (self.frames - f_last) / (now - t_last)
                    t_last, f_last = now, self.frames
            except Exception as e:
                self.fails += 1
                time.sleep(0.3)
            time.sleep(self.interval)

    def snapshot(self):
        with self.lock:
            return self.png, self.wh, self.frames, self.fails, self.fps


PAGE = b"""<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8"><title>LinTOS \xe5\xae\x9e\xe6\x97\xb6\xe7\x9c\x8b\xe5\xb1\x8f</title>
<style>
 body{margin:0;background:#1b1d21;color:#ddd;font:13px/1.6 -apple-system,"PingFang SC",sans-serif;
      display:flex;flex-direction:column;align-items:center;gap:10px;padding:14px}
 .bar{display:flex;gap:16px;align-items:center;font-variant-numeric:tabular-nums}
 .scr{background:#fff;border:2px solid #444;border-radius:4px;image-rendering:pixelated}
 .k{color:#8ab4f8}
 b{color:#fff;font-weight:500}
</style></head><body>
<div class="bar">
  <b>LinTOS \xe5\xae\x9e\xe6\x97\xb6\xe7\x9c\x8b\xe5\xb1\x8f</b>
  <span><span class="k">\xe5\xb8\xa7\xe7\x8e\x87</span> <b id="fps">-</b></span>
  <span><span class="k">\xe5\xb8\xa7\xe6\x80\xbb</span> <b id="n">-</b></span>
  <span><span class="k">\xe5\xb0\xba\xe5\xaf\xb8</span> <b id="wh">-</b></span>
  <span><span class="k">\xe5\xa4\xb1\xe8\xb4\xa5</span> <b id="f">-</b></span>
</div>
<img class="scr" id="s" width="800" height="600" alt="device screen">
<script>
const s=document.getElementById('s');
function tick(){ s.src='/frame.png?t='+Date.now(); }
setInterval(tick,300); tick();
setInterval(async()=>{ try{
  const r=await fetch('/status'); const j=await r.json();
  document.getElementById('fps').textContent=j.fps.toFixed(1)+' fps';
  document.getElementById('n').textContent=j.frames;
  document.getElementById('wh').textContent=j.w+'x'+j.h;
  document.getElementById('f').textContent=j.fails;
}catch(e){} },1000);
</script></body></html>
"""


def serve(cap, host, port, scale):
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/":
                body = PAGE
                ct = "text/html; charset=utf-8"
            elif path == "/frame.png":
                body, _, _, _, _ = cap.snapshot()
                if body is None:
                    self.send_response(503)
                    self.end_headers()
                    return
                ct = "image/png"
            elif path == "/status":
                _, wh, n, f, fps = cap.snapshot()
                body = ('{"fps":%.2f,"frames":%d,"fails":%d,"w":%d,"h":%d}'
                        % (fps, n, f, wh[0], wh[1])).encode()
                ct = "application/json"
            else:
                self.send_response(404)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", ct)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

    srv = ThreadingHTTPServer((host, port), H)
    w = 400 * scale
    h = 300 * scale
    print("=" * 56)
    print(" LinTOS 实时看屏已启动")
    print("   串口   : %s  (只读, 不动 DTR/RTS)" % cap.port)
    print("   画面   : %dx%d (放大 %dx)" % (w, h, scale))
    print("   打开   : http://%s:%d/" % (host, port))
    print("   停止   : Ctrl-C")
    print("=" * 56)
    srv.serve_forever()


def window(cap, scale):
    import tkinter as tk
    from PIL import ImageTk

    root = tk.Tk()
    root.title("LinTOS 实时看屏")
    lbl = tk.Label(root, bg="#111")
    lbl.pack()
    info = tk.Label(root, font=("Menlo", 11), fg="#888", bg="#1b1d21")
    info.pack(fill="x")
    state = {"img": None}

    def tick():
        png, wh, n, f, fps = cap.snapshot()
        if png:
            im = Image.open(io.BytesIO(png)).convert("L")
            state["img"] = ImageTk.PhotoImage(im)
            lbl.configure(image=state["img"])
        info.configure(text="%.1f fps   frames=%d  fails=%d  %dx%d"
                            % (fps, n, f, wh[0], wh[1]))
        root.after(120, tick)

    tick()
    root.mainloop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None, help="串口 (默认自动探测)")
    ap.add_argument("--scale", type=int, default=2, help="放大倍数 (默认 2 → 800x600)")
    ap.add_argument("--http-port", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--window", action="store_true", help="用桌面窗口显示(默认用浏览器)")
    ap.add_argument("-o", "--out", default=None, help="顺便每帧存盘到该 png (调试用)")
    args = ap.parse_args()

    port = find_port(args.port)
    cap = Capture(port, args.scale)
    cap.start()

    # 等第一帧, 好让"打不开/没图"早点暴露
    t0 = time.time()
    while time.time() - t0 < 6:
        png, _, _, _, _ = cap.snapshot()
        if png:
            break
        time.sleep(0.1)
    png, wh, n, f, _ = cap.snapshot()
    if not png:
        print("⚠ 6 秒内没抓到帧 (失败 %d 次)。检查: 设备是否开机 / 串口是否被别的程序占用。" % f,
              file=sys.stderr)

    if args.out:
        def saver():
            last = 0
            while True:
                p, _, nn, _, _ = cap.snapshot()
                if p and nn != last:
                    last = nn
                    try:
                        open(args.out, "wb").write(p)
                    except Exception:
                        pass
                time.sleep(0.2)
        threading.Thread(target=saver, daemon=True).start()

    try:
        if args.window:
            window(cap, args.scale)
        else:
            serve(cap, args.host, args.http_port, args.scale)
    except KeyboardInterrupt:
        pass
    finally:
        cap.stop = True
        print("\n已停止。")


if __name__ == "__main__":
    main()
