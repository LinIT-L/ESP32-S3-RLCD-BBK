#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pack_single.py — 把「Lua 应用 + 素材」打成**一个文件** (2026-11-23)

产物形态
--------
    --@lapp v1
    --@pkg offset=000093269 size=01868354     ← 固定 10 位, 便于回填
    --@app name=...
    <Lua 脚本...>                              ← 前 offset 字节
    <ZIP 数据, 全部 STORED>                    ← offset .. offset+size

为什么这么放
------------
* 固件加载器 (page_lua.c: lapp_script_len) 只把前 offset 字节交给 luaL_loadbuffer,
  后面的二进制不参与解析 —— 所以包内可以放任意字节。
* ZIP 一律 **STORED(不压缩)**: 固件 am_asset 的注释与 pack_asset.py 都写明, deflate 条目
  在真机走 tinfl 会栈溢出崩溃; 这里虽然不解压, 也统一按 stored 存, 让外部工具/校验一致。
* 运行时由 app.pkg.self()/read() 按条目名定位 offset+size 直接读, **不解压、不建目录**。

用法
----
    python3 tools/pack_single.py <app.lua> <art_dir> <out.lua> [--prefix wuxia]
    python3 tools/pack_single.py --check <out.lua>

校验内容: 头部 offset/size 与真实文件长度一致、ZIP 可解析、条目名/数量、每条的偏移落在数据区内。
"""
import argparse
import binascii
import io
import json
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = b"--@lapp"


def read_lapp_head(txt: bytes):
    """返回头部文本 (前 2KB), 用于定位 --@pkg。"""
    return txt[:2048]


def strip_old_pkg(txt: bytes) -> bytes:
    """如果输入已经是单文件包, 先切掉尾部的 ZIP 并去掉 --@pkg 行, 便于重复执行。"""
    head = read_lapp_head(txt)
    i = head.find(b"--@pkg")
    if i >= 0:
        # 解析旧 offset, 只保留脚本部分
        line_end = txt.find(b"\n", i)
        line = txt[i:line_end]
        try:
            off = int(line.split(b"offset=")[1].split(b" ")[0])
            txt = txt[:off]
        except Exception:
            pass
        # 删掉 --@pkg 行 (含换行)
        j = txt.find(b"--@pkg")
        k = txt.find(b"\n", j)
        if j >= 0 and k > 0:
            txt = txt[:j] + txt[k + 1:]
    return txt


def make_zip(art_dir: str, prefix: str) -> bytes:
    files = sorted(f for f in os.listdir(art_dir) if os.path.isfile(os.path.join(art_dir, f)))
    if not files:
        sys.exit("✗ 素材目录为空: %s" % art_dir)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        for f in files:
            z.write(os.path.join(art_dir, f), "%s/%s" % (prefix, f))
    return buf.getvalue()


def build(app_lua: str, art_dir: str, out: str, prefix: str):
    raw = open(app_lua, "rb").read()
    if not raw.startswith(HDR):
        sys.exit("✗ %s 不是 LAPP (缺 --@lapp 头) —— 先跑 tools/pack_app.py" % app_lua)
    body = strip_old_pkg(raw)

    # 占位行插在 --@lapp 之后, 固定宽度; 回填时长度不变
    first_nl = body.index(b"\n") + 1
    ph = b"--@pkg offset=0000000000 size=0000000000\n"
    script = body[:first_nl] + ph + body[first_nl:]

    zdata = make_zip(art_dir, prefix)
    off = len(script)
    line = b"--@pkg offset=%010d size=%010d\n" % (off, len(zdata))
    assert len(line) == len(ph), "占位行宽度必须一致"
    script = script.replace(ph, line, 1)

    out_bin = script + zdata
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(out_bin)

    print("✓ %s" % os.path.relpath(out, ROOT))
    print("  脚本 %d B + ZIP %d B = %d B" % (off, len(zdata), len(out_bin)))
    print("  ZIP 条目 %d 个 (前缀 %s/)" % (len(zipfile.ZipFile(io.BytesIO(zdata)).namelist()), prefix))
    return out_bin


def check(path: str) -> int:
    data = open(path, "rb").read()
    head = read_lapp_head(data)
    i = head.find(b"--@pkg")
    if i < 0:
        print("✗ 没有 --@pkg 头")
        return 1
    line = data[i:data.find(b"\n", i)]
    off = int(line.split(b"offset=")[1].split(b" ")[0])
    size = int(line.split(b"size=")[1].split(b" ")[0])
    ok = True
    if off + size != len(data):
        print("✗ offset+size=%d 与文件长度 %d 不一致" % (off + size, len(data)))
        ok = False
    if not data[off:off + 4] == b"PK\x03\x04":
        print("✗ 数据区开头不是 ZIP local header")
        ok = False
    try:
        z = zipfile.ZipFile(io.BytesIO(data[off:off + size]))
        names = z.namelist()
        bad = [x for x in z.infolist() if x.compress_type != zipfile.ZIP_STORED]
        if bad:
            print("✗ %d 个条目不是 STORED" % len(bad))
            ok = False
        # 每条的数据必须落在数据区内
        for inf in z.infolist():
            if inf.header_offset + 30 + len(inf.filename.encode()) + inf.compress_size > size:
                print("✗ 条目越界: %s" % inf.filename)
                ok = False
                break
        print("  脚本 %d B, ZIP %d B, 条目 %d 个" % (off, size, len(names)))
        print("  首条: %s" % (names[0] if names else "-"))
    except Exception as e:
        print("✗ ZIP 解析失败: %s" % e)
        ok = False
    # 脚本部分必须是可读文本 (不出现 NUL)
    if b"\x00" in data[:off]:
        print("✗ 脚本区含 NUL 字节")
        ok = False
    print("校验: %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", metavar="FILE")
    ap.add_argument("app", nargs="?")
    ap.add_argument("art", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--prefix", default="wuxia")
    a = ap.parse_args()
    if a.check:
        return check(a.check)
    if not (a.app and a.art and a.out):
        ap.error("需要 app.lua art_dir out.lua (或 --check FILE)")
    build(a.app, a.art, a.out, a.prefix)
    return check(a.out)


if __name__ == "__main__":
    sys.exit(main())
