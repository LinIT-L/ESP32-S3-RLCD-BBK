#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_server.py — 资源包多文件下载的受控测试服务器

为什么不用 python -m http.server
-------------------------------
真机要验证的四件事，标准库默认行为都覆盖不到：
  · 连接复用到底有没有生效 —— 必须**在服务器侧**数「新建 TCP 连接数 vs 请求数」；
  · 服务器主动关连接（Connection: close）时客户端会不会自动重连；
  · 传一半断流（Content-Length 说满、实际只给一半）客户端能不能识别；
  · 内容损坏同长度（CRC 不符）会不会被拒、会不会把坏文件留在目标位置。

用法
----
    python3 tests/asset/test_server.py --root <目录> [--bind IP] [--port N] \
            [--fault 索引:类型] [--close-every N]

    --fault K:drop      第 K 个文件请求只发一半就断开
    --fault K:corrupt   第 K 个文件请求内容同长度但被改坏（CRC 必不符）
    --fault K:close     第 K 个文件请求回 Connection: close（模拟服务端收连接）
    --close-every N     每 N 个请求回一次 Connection: close

统计（结束时打到 stdout，前端脚本据此判定）：
    STAT conns=N requests=M bytes=X
    STAT keepalive_req=K     ← 复用连接上收到多少次请求（>0 就证明客户端复用了）
"""
import argparse
import os
import socket
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

STATS = {"conns": 0, "requests": 0, "bytes": 0, "file_req": 0, "reused_req": 0}
LOCK = threading.Lock()
ARGS = None
CONN_SEQ = {"n": 0}
LOG = []


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"      # 开持久连接，否则每请求必断，测不出复用
    server_version = "LinTOS-TestSrv/1.0"

    def log_message(self, fmt, *a):
        pass                            # 自己打，别用默认格式

    def _note(self, path, nbytes, close, fault=""):
        with LOCK:
            STATS["requests"] += 1
            STATS["bytes"] += nbytes
            LOG.append("%6.2f %-34s %9d %s%s" % (time.time() % 10000, path, nbytes,
                                                 "CLOSE " if close else "", fault))
            # 同一连接上的第二个及以后请求 = 复用
            if getattr(self, "_req_on_conn", 0) > 0:
                STATS["reused_req"] += 1
            self._req_on_conn = getattr(self, "_req_on_conn", 0) + 1
            # 每请求落一次盘：不用等进程退出也能读到统计(后台跑时全靠它)
            try:
                with open(ARGS.statfile, "w") as f:
                    f.write("STAT conns=%d requests=%d bytes=%d file_req=%d reused_req=%d\n"
                            % (STATS["conns"], STATS["requests"], STATS["bytes"],
                               STATS["file_req"], STATS["reused_req"]))
            except Exception:
                pass

    def do_GET(self):
        path = self.path.split("?")[0]
        rel = path.lstrip("/")
        # 运行时可改故障注入(免重启)：ctl 文件内容 "fault=K:kind" / "clear"
        fault_cfg = ARGS.fault
        if ARGS.ctl:
            try:
                t = open(ARGS.ctl).read().strip()
                if t == "clear" or t == "fault=none":
                    fault_cfg = None
                elif t.startswith("fault=") and len(t) > 6:
                    pat, kind = t[6:].rsplit(":", 1)
                    fault_cfg = (pat, kind)
            except Exception:
                pass
        ARGS.fault = fault_cfg
        full = os.path.join(ARGS.root, rel)
        if not os.path.abspath(full).startswith(os.path.abspath(ARGS.root)):
            self.send_error(403)
            return
        if not os.path.isfile(full):
            self.send_error(404)
            self._note(path, 0, True, "404")
            return

        is_manifest = rel.endswith("manifest.json")
        with LOCK:
            fidx = STATS["file_req"]
            if not is_manifest:
                STATS["file_req"] += 1
                fidx = STATS["file_req"]

        fault = ""
        if fault_cfg and not is_manifest:
            pat, kind = fault_cfg
            # 用**文件名匹配**而不是"第 K 个请求" —— 计数器跨多次测试会漂，不靠谱
            if pat in rel:
                fault = kind
        if ARGS.close_every and STATS["requests"] and STATS["requests"] % ARGS.close_every == 0:
            if not fault:
                fault = "close"

        data = open(full, "rb").read()
        if fault == "corrupt":
            b = bytearray(data)
            step = max(1, len(b) // 64)
            for i in range(0, len(b), step):
                b[i] ^= 0xFF          # 长度不变，内容变 ⇒ 只有 CRC 能发现
            data = bytes(b)

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        if fault in ("close",):
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()

        if fault == "drop":
            half = data[: len(data) // 2]
            try:
                self.wfile.write(half)
                self.wfile.flush()
            except Exception:
                pass
            self._note(path, len(half), True, "DROP")
            try:
                self.connection.close()   # 骗客户端"还有一半"，直接断
            except Exception:
                pass
            self.close_connection = True
            return

        try:
            self.wfile.write(data)
            self.wfile.flush()
        except Exception:
            pass
        self._note(path, len(data), fault == "close", fault.upper())


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def verify_request(self, request, client_address):
        with LOCK:
            STATS["conns"] += 1
        return True


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--fault", default="", help="K:drop|corrupt|close")
    ap.add_argument("--close-every", type=int, default=0)
    ap.add_argument("--statfile", default="/tmp/mf_srv_stats.txt")
    ap.add_argument("--ctl", default="", help="运行时故障配置文件")
    a = ap.parse_args()

    if a.fault:
        pat, kind = a.fault.rsplit(":", 1)
        a.fault = (pat, kind)
    else:
        a.fault = None
    ARGS = a

    srv = Server((a.bind, a.port), Handler)
    print("LISTEN %s:%d root=%s fault=%s close_every=%d"
          % (a.bind, a.port, a.root, a.fault, a.close_every), flush=True)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        srv.shutdown()
        print("\n==== 每请求明细 ====")
        for l in LOG:
            print("  " + l)
        print("STAT conns=%d requests=%d bytes=%d file_req=%d reused_req=%d"
              % (STATS["conns"], STATS["requests"], STATS["bytes"],
                 STATS["file_req"], STATS["reused_req"]), flush=True)


if __name__ == "__main__":
    main()
