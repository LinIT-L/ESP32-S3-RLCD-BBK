#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_bad.py — 从正常 LPK 造出若干"坏包"，文件名前缀 = 期望的 lpk_open 返回码。"""
import os
import struct
import sys

HDR = 64
ENT = 32

src, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
raw = bytearray(open(src, "rb").read())
for fn in os.listdir(outdir):
    os.remove(os.path.join(outdir, fn))


def emit(name, buf):
    open(os.path.join(outdir, name), "wb").write(bytes(buf))


def hdr_get(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def hdr_set(buf, off, val):
    struct.pack_into("<I", buf, off, val)


man_off = hdr_get(raw, 24)
count = hdr_get(raw, 32)

# -3 魔数坏
b = bytearray(raw); b[0:4] = b"ZIP0"; emit("-3_magic.lpk", b)

# -4 版本不支持
b = bytearray(raw); struct.pack_into("<H", b, 4, 2); emit("-4_version.lpk", b)

# -5 头长度字段异常
b = bytearray(raw); struct.pack_into("<H", b, 6, 32); emit("-5_hdrsize.lpk", b)

# -6 文件被截断（头里的 total_size 不变）
emit("-6_trunc.lpk", raw[:-1024])

# -7 第 0 条资源越界（off 改到文件尾之外）
b = bytearray(raw)
o = man_off
off0 = struct.unpack_from("<I", b, o + 4)[0]
struct.pack_into("<I", b, o + 4, len(b) - 8)
emit("-7_entry_oob.lpk", b)

# -8 路径非法（把第 0 条名字首字节改成 '/', 变成绝对路径）
b = bytearray(raw)
name_off = struct.unpack_from("<I", b, man_off + 0)[0]
b[man_off + name_off] = ord("/")
emit("-8_path.lpk", b)

# 增强：-7 另一种（size 溢出到 32 位边界）
b = bytearray(raw)
struct.pack_into("<I", b, man_off + 8, 0xFFFFFFF0)
emit("-7_entry_overflow.lpk", b)

print("生成 %d 个坏包 -> %s" % (len(os.listdir(outdir)), outdir))
