#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_manifest_deploy.py — 校验「清单 + 文件树」能否按设备的方式取到

设备拼 URL 的规则(见 am_asset_multi.c):
    manifest URL = <mirror>/<manifest_rel>             例 https://host/bbk/pkgs/pal/manifest.json
    file URL     = <mirror>/<dirname(manifest_rel)>/<path>  例 https://host/bbk/pkgs/pal/ABC.MKF

本脚本起一个本地静态服务器，把 <root> 当站点根，然后完全按上面两条规则去取，
逐个比对 size 与 crc32 —— 上线前就能确认"目录布局对不对"。

顺带测「连接复用」到底省多少: 同一批请求分别用
  ① 每次新建 TCP 连接   ② 复用一条连接(http.client 的持久连接)
在 localhost 上测出来的是**下限**（没有 TLS 握手），真机上的差距会更大。

用法:
    python3 tests/asset/check_manifest_deploy.py /tmp/serve bbk/pkgs/pal/manifest.json [--sample 5]
"""
import argparse
import functools
import http.client
import http.server
import json
import os
import socketserver
import sys
import threading
import time
import zlib


def crc32_file(p):
    c = 0
    with open(p, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            c = zlib.crc32(b, c)
    return c & 0xFFFFFFFF


def serve(root):
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=root)
    httpd = socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler)
    httpd.daemon_threads = True
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd, httpd.server_address[1]


def get(port, path, conn=None, keep=False):
    if conn is None:
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    conn.request("GET", path, headers={"Connection": "keep-alive" if keep else "close"})
    r = conn.getresponse()
    body = r.read()
    status = r.status
    if not keep:
        conn.close()
        return status, body, None
    return status, body, conn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="站点根目录")
    ap.add_argument("manifest_rel", help="清单相对路径, 例 bbk/pkgs/pal/manifest.json")
    ap.add_argument("--sample", type=int, default=5, help="抽样校验的文件数")
    ap.add_argument("--reuse-rounds", type=int, default=30, help="复用时延对比轮数")
    a = ap.parse_args()

    httpd, port = serve(a.root)
    ok = fail = 0

    def chk(cond, msg):
        nonlocal ok, fail
        if cond:
            ok += 1
            print("  ✓ " + msg)
        else:
            fail += 1
            print("  ✗ " + msg)

    print("=== 站点根: %s  端口 %d ===" % (a.root, port))

    murl = "/" + a.manifest_rel.lstrip("/")
    st, body, _ = get(port, murl)
    chk(st == 200, "清单可取: %s → HTTP %d (%d B)" % (murl, st, len(body)))
    if st != 200:
        httpd.shutdown()
        return 1

    mf = json.loads(body.decode("utf-8"))
    chk(mf.get("fmt") == 1, "fmt=1")
    chk(mf["count"] == len(mf["files"]), "count %d == 文件表 %d" % (mf["count"], len(mf["files"])))
    chk(mf["total"] == sum(x["size"] for x in mf["files"]), "total 自洽 (%d B)" % mf["total"])

    base = "/" + os.path.dirname(a.manifest_rel).strip("/")
    if base != "/":
        base += "/"
    print("  文件 URL 前缀: %s" % base)

    # 抽样: 头部/中部/尾部 + 最大的那个
    files = mf["files"]
    idx = sorted(set([0, len(files) // 2, len(files) - 1,
                      max(range(len(files)), key=lambda i: files[i]["size"])]
                     + list(range(min(a.sample, len(files))))))
    for i in idx:
        f = files[i]
        st, data, _ = get(port, base + f["path"])
        good = (st == 200 and len(data) == f["size"] and (zlib.crc32(data) & 0xFFFFFFFF) == int(f["crc32"], 16))
        chk(good, "%-28s %9d B http=%d" % (f["path"], f["size"], st))

    # 连接复用对比（localhost，无 TLS ⇒ 只是下限）
    print("\n=== 连接复用对比 (%d 次请求) ===" % a.reuse_rounds)
    probe = base + files[0]["path"]

    t0 = time.time()
    for _ in range(a.reuse_rounds):
        get(port, probe)
    t_new = time.time() - t0

    conn = None
    t0 = time.time()
    for _ in range(a.reuse_rounds):
        _, _, conn = get(port, probe, conn=conn, keep=True)
    t_keep = time.time() - t0
    if conn:
        conn.close()

    print("  每次新建连接: %.1f ms/次" % (t_new * 1000 / a.reuse_rounds))
    print("  复用同一连接: %.1f ms/次" % (t_keep * 1000 / a.reuse_rounds))
    if t_keep > 0:
        print("  localhost 上省 %.1f%%（真机还要加 TLS 握手，差距更大）"
              % ((1 - t_keep / t_new) * 100))
        print("  推算 109 个文件: %.1f s → %.1f s (仅连接开销)"
              % (t_new * 1000 / a.reuse_rounds * 109 / 1000,
                 t_keep * 1000 / a.reuse_rounds * 109 / 1000))

    httpd.shutdown()
    print("\n==== %d PASS / %d FAIL ====" % (ok, fail))
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
