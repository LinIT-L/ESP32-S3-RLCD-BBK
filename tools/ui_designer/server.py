#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LinTOS UI 设计器 — 本地服务 (400x300 单色屏, 面向这台掌机/设备)
=================================================================
用法:
    python3 server.py [port]          # 默认 8123
    然后浏览器打开 http://localhost:8123   (脚本也会自动打开)

功能:
  * 提供 index.html / style.css / app.js 等静态资源
  * POST /save/{name} 接收 JSON 设计稿, 写入 project_dir/designs/{name}.json
    (返回写好的绝对路径), 方便 AI 之后直接读取该 JSON 生成设备 C 代码
  * GET  /designs           列出 designs/ 下已有方案 (供复载)

纯标准库, 无第三方依赖; 跨平台 (win/mac/linux) 均可运行.
"""
import sys, os, json, mimetypes, webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE   = os.path.dirname(os.path.abspath(__file__))
DESIGNS = os.path.join(HERE, "designs")
os.makedirs(DESIGNS, exist_ok=True)

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8123
ROOT = HERE

MIME = {
    ".html": "text/html", ".css": "text/css", ".js": "application/javascript",
    ".json": "application/json", ".svg": "image/svg+xml", ".png": "image/png",
    ".woff2": "font/woff2", ".ttf": "font/ttf", ".ico": "image/x-icon",
}

def safe_name(name: str) -> str:
    keep = []
    for ch in name:
        keep.append(ch if (ch.isalnum() or ch in "-_.") else "_")
    s = "".join(keep).strip("._")
    return (s or "design")

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="text/plain; charset=utf-8", extra=None):
        data = body if isinstance(body, bytes) else str(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def do_OPTIONS(self):
        self._send(204, "", extra={"Access-Control-Allow-Methods": "GET,POST,OPTIONS",
                                   "Access-Control-Allow-Headers": "Content-Type"})

    def _static(self, path):
        fp = os.path.join(ROOT, path)
        if not os.path.isfile(fp):
            return self._send(404, "not found")
        ext = os.path.splitext(fp)[1].lower()
        with open(fp, "rb") as f:
            return self._send(200, f.read(), MIME.get(ext, "application/octet-stream"))

    def do_GET(self):
        raw = self.path.split("?")[0]
        p = raw.rstrip("/")
        # 根路径或 /index.html → 首页
        if p in ("", "/", "/index.html"):
            return self._static("index.html")
        # /designs   → 列出方案; /designs/{name} → 读取方案
        if p.startswith("/designs"):
            name = p[len("/designs/"):].lstrip("/")
            if not name:
                lst = [f for f in sorted(os.listdir(DESIGNS)) if f.endswith(".json")]
                return self._send(200, json.dumps(lst, ensure_ascii=False), "application/json; charset=utf-8")
            if not name.endswith(".json"):
                name += ".json"
            fp = os.path.join(DESIGNS, safe_name(name))
            if os.path.isfile(fp):
                with open(fp, "r", encoding="utf-8") as f:
                    return self._send(200, f.read(), "application/json; charset=utf-8")
            return self._send(404, "design not found")
        if p.startswith("/"):
            return self._static(p.lstrip("/"))

    def do_POST(self):
        p = self.path.split("?")[0]
        if p.startswith("/save/"):
            name = safe_name(p[len("/save/"):])
            body = self._read_body().decode("utf-8", "replace")
            try:
                obj = json.loads(body)
            except Exception as e:
                return self._send(400, f"bad json: {e}")
            obj["_saved"] = True
            fp = os.path.join(DESIGNS, name + ".json")
            with open(fp, "w", encoding="utf-8") as f:
                json.dump(obj, f, ensure_ascii=False, indent=2)
            return self._send(200, json.dumps({"ok": True, "file": fp}), "application/json; charset=utf-8")
        return self._send(404, "no such api")

def main():
    srv = HTTPServer(("127.0.0.1", PORT), Handler)
    url = f"http://127.0.0.1:{PORT}/"
    print("=" * 52)
    print("  LinTOS UI 设计器  (400x300 单色屏)")
    print(f"  运行中:  {url}")
    print(f"  设计方案保存目录: {DESIGNS}")
    print("  按 Ctrl+C 退出")
    print("=" * 52)
    try:
        webbrowser.open(url)
    except Exception:
        pass
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")

if __name__ == "__main__":
    main()