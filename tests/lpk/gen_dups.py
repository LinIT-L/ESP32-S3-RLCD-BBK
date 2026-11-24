#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_dups.py — 造「重名 / 大小写冲突」的包（读取端必须整包拒绝）

用法: gen_dups.py <输出目录>

为什么单独造而不是改一个正常包：
  重名要靠**两条索引指向同一个名字**才能精确命中那条分支；
  大小写冲突要求两条名字**只差大小写**。手工改字节很容易越界写坏名字池，
  所以这里从零拼一个最小的合法包（头 + 2 条索引 + 名字池 + 一段数据），
  除了"名字"这一点，其余字段全部自洽 —— 保证失败原因是重名，而不是别的。

文件名前缀 = 期望的 lpk_open 返回码（run_all.sh / lpk_test.c 按前缀解析）：
  -7  完全同名   → LPK_E_ENTRY
  -11 仅大小写不同 → LPK_E_AMBIG
"""
import os
import struct
import sys

HDR, ENT = 64, 32


def build(names, out, payload=b"X" * 64):
    """names: 两条名字（bytes），共享同一段数据。"""
    pool = b""
    recs = []
    off = 0
    for nm in names:
        recs.append((off, len(nm)))
        pool += nm + b"\x00"
        off += len(nm) + 1
    man_off = HDR
    man_size = ENT * 2 + len(pool)
    data_off = ((HDR + man_size) + 511) // 512 * 512
    total = data_off + len(payload)

    buf = bytearray(b"\x00" * total)
    # ---- HEADER ----
    buf[0:4] = b"LPK1"
    struct.pack_into("<H", buf, 4, 1)            # version
    struct.pack_into("<H", buf, 6, HDR)          # hdr_size
    struct.pack_into("<I", buf, 8, total)        # total_size
    struct.pack_into("<I", buf, 12, 0)           # lua_off
    struct.pack_into("<I", buf, 16, 0)           # lua_size = 0（纯资源包）
    struct.pack_into("<I", buf, 20, 0)           # lua_crc
    struct.pack_into("<I", buf, 24, man_off)
    struct.pack_into("<I", buf, 28, man_size)
    struct.pack_into("<I", buf, 32, 2)           # count
    struct.pack_into("<I", buf, 36, data_off)
    struct.pack_into("<I", buf, 40, 0)           # pkg_crc = 0 ⇒ 不校验整包
    struct.pack_into("<I", buf, 44, 1)           # flags: 含资源
    # ---- 两条索引（名字池偏移相对 man_off）----
    for i, (noff, nlen) in enumerate(recs):
        base = man_off + i * ENT
        struct.pack_into("<I", buf, base + 0, ENT * 2 + noff)   # name_off
        struct.pack_into("<I", buf, base + 4, data_off)         # 资源绝对偏移
        struct.pack_into("<I", buf, base + 8, len(payload))     # size
        struct.pack_into("<I", buf, base + 12, 0)               # crc32（0 = 不校验）
        struct.pack_into("<H", buf, base + 16, nlen)            # name_len
        buf[base + 18] = 0                                      # type = raw
    buf[man_off + ENT * 2: man_off + ENT * 2 + len(pool)] = pool
    buf[data_off: data_off + len(payload)] = payload
    with open(out, "wb") as f:
        f.write(bytes(buf))


def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    for fn in os.listdir(d):
        if fn.endswith("dup_exact.lpk") or fn.endswith("dup_case.lpk"):
            os.remove(os.path.join(d, fn))
    # -7 完全同名
    build([b"DUP.TXT", b"DUP.TXT"], os.path.join(d, "-7_dup_exact.lpk"))
    # -11 仅大小写不同
    build([b"DUP.TXT", b"dup.txt"], os.path.join(d, "-11_dup_case.lpk"))
    print("生成 2 个重名包 -> %s" % d)


if __name__ == "__main__":
    main()
