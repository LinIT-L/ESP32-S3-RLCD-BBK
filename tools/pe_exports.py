#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pe_exports.py — 纯标准库读 Windows PE 的**导出表/导入表**（无需装 pefile）。

为什么要有它
------------
QQ堂客户端的渲染层在 `gameUI.dll` / `GFX.dll` / `ClientBase.dll` 里，但这几个 DLL
被加壳/保护（`strings` 取不到任何符号）。**导出表通常是加壳后仍然可读的**——
所以走 PE 结构解析比 `strings` 靠谱。找 `.img` 的解包入口就靠它。

用法
----
    python3 tools/pe_exports.py <file.dll|.exe>            # 导出表
    python3 tools/pe_exports.py <file> --imports           # 导入表 (看它调了谁)
    python3 tools/pe_exports.py <file> --sections          # 节表 (看是否加壳)
"""
import argparse
import struct
import sys


def rva2off(secs, rva):
    for name, va, vsz, raw, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return raw + (rva - va)
    return None


def parse(path):
    d = open(path, "rb").read()
    if d[:2] != b"MZ":
        raise SystemExit("不是 PE: %s" % path)
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    if d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise SystemExit("没有 PE 签名")
    coff = e_lfanew + 4
    nsec, = struct.unpack_from("<H", d, coff + 2)
    optsz, = struct.unpack_from("<H", d, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", d, opt)
    is64 = (magic == 0x20B)
    # 数据目录: PE32 从 opt+96 起, PE32+ 从 opt+112 起
    dd = opt + (112 if is64 else 96)
    exp_rva, exp_sz = struct.unpack_from("<II", d, dd)
    imp_rva, imp_sz = struct.unpack_from("<II", d, dd + 8)
    sects = []
    so = opt + optsz
    for i in range(nsec):
        b = so + i * 40
        nm = d[b:b + 8].rstrip(b"\0").decode("latin1")
        vsz, va, rsz, raw = struct.unpack_from("<IIII", d, b + 8)
        sects.append((nm, va, vsz, raw, rsz))
    return d, sects, is64, exp_rva, exp_sz, imp_rva, imp_sz


def exports(d, sects, exp_rva, exp_sz):
    if not exp_rva:
        return []
    off = rva2off(sects, exp_rva)
    if off is None:
        return []
    nfun, nname = struct.unpack_from("<II", d, off + 20)[0], struct.unpack_from("<I", d, off + 24)[0]
    aof, aon, aoo = struct.unpack_from("<III", d, off + 28)
    if nname == 0:
        return []
    no = rva2off(sects, aon)
    oo = rva2off(sects, aoo)
    if no is None or oo is None:
        return []
    out = []
    for i in range(nname):
        nrva, = struct.unpack_from("<I", d, no + i * 4)
        ordn, = struct.unpack_from("<H", d, oo + i * 2)
        so = rva2off(sects, nrva)
        if so is None:
            continue
        e = d.index(b"\0", so)
        out.append((ordn, d[so:e].decode("latin1")))
    return sorted(out)


def imports(d, sects, imp_rva):
    if not imp_rva:
        return []
    off = rva2off(sects, imp_rva)
    out = []
    while off:
        ent = d[off:off + 20]
        if len(ent) < 20 or ent == b"\0" * 20:
            break
        name_rva, = struct.unpack_from("<I", d, off + 12)
        no = rva2off(sects, name_rva)
        if no:
            e = d.index(b"\0", no)
            out.append(d[no:e].decode("latin1"))
        off += 20
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--imports", action="store_true")
    ap.add_argument("--sections", action="store_true")
    ap.add_argument("--filter", default="")
    a = ap.parse_args()
    d, sects, is64, exp_rva, exp_sz, imp_rva, imp_sz = parse(a.path)
    print("%s  %s  %d 字节  节数=%d  数据目录: 导出 rva=%#x 导入 rva=%#x"
          % (a.path, "PE32+" if is64 else "PE32", len(d), len(sects), exp_rva, imp_rva))
    if a.sections or not (a.imports):
        print("--- 节表 ---")
        for nm, va, vsz, raw, rsz in sects:
            print("  %-9s vaddr=%#010x vsize=%#010x raw=%#010x rawsize=%#010x"
                  % (nm, va, vsz, raw, rsz))
    if a.imports:
        print("--- 导入的 DLL ---")
        for m in imports(d, sects, imp_rva):
            print("  " + m)
        return
    ex = exports(d, sects, exp_rva, exp_sz)
    print("--- 导出 (%d 个) ---" % len(ex))
    for ordn, name in ex:
        if a.filter and a.filter.lower() not in name.lower():
            continue
        print("  [%4d] %s" % (ordn, name))


if __name__ == "__main__":
    main()
