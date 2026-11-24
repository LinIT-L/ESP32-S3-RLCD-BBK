#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_db_integrity.py — 数据库损坏检测（配合 wiki_open_probe）

wiki_open() 的契约是「index.bin 大小必须严格等于 articleCount×80，否则拒读」。
本脚本把真实生成的库复制一份，逐个制造损坏，确认：

    能打开的，必须能读；
    不能打开的，必须是干净地拒绝（返回 0），而不是崩。

用法： python3 tests/wiki/test_db_integrity.py <源库目录> <探针可执行文件>
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

g_pass = g_fail = 0


def check(cond, msg):
    global g_pass, g_fail
    if cond:
        g_pass += 1
        print("  PASS  " + msg)
    else:
        g_fail += 1
        print("  FAIL  " + msg)


def probe(binpath, d):
    r = subprocess.run([binpath, d], capture_output=True, text=True, timeout=60)
    out = r.stdout.strip().splitlines()
    opened = None
    for line in out:
        if line.startswith("OPEN "):
            opened = int(line.split()[1])
    return r.returncode, opened, "\n".join(out)


def fresh_copy(src, tmp):
    d = tempfile.mkdtemp(dir=tmp)
    for n in os.listdir(src):
        if n == "build.log":
            continue
        shutil.copy2(os.path.join(src, n), os.path.join(d, n))
    return d


def main():
    if len(sys.argv) < 3:
        print("用法: test_db_integrity.py <源库> <探针>")
        return 2
    src, binpath = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="wikidb_")

    print("========== 数据库损坏检测 ==========")
    print("源库: %s\n探针: %s\n" % (src, binpath))

    # 基线
    print("T0 基线")
    d = fresh_copy(src, tmp)
    rc, n, out = probe(binpath, d)
    check(rc == 0 and n and n > 0, "原样库能打开（%s 篇）" % n)

    # T1 index.bin 被截断 → 大小 != count*80
    print("\nT1 index.bin 截断")
    d = fresh_copy(src, tmp)
    p = os.path.join(d, "index.bin")
    data = open(p, "rb").read()
    open(p, "wb").write(data[:-80])
    rc, n, _ = probe(binpath, d)
    check(rc != 0 and n == 0, "截掉一条后拒读（返回 0）")

    # T2 index.bin 多出字节
    print("\nT2 index.bin 变长")
    d = fresh_copy(src, tmp)
    p = os.path.join(d, "index.bin")
    open(p, "ab").write(b"\x00" * 80)
    rc, n, _ = probe(binpath, d)
    check(rc != 0 and n == 0, "多出 80 字节后拒读（返回 0）")

    # T3 metadata.json 缺失
    print("\nT3 metadata.json 缺失")
    d = fresh_copy(src, tmp)
    os.remove(os.path.join(d, "metadata.json"))
    rc, n, _ = probe(binpath, d)
    check(rc != 0 and n == 0, "没有 metadata 时拒读")

    # T4 version 不对
    print("\nT4 格式版本不符")
    d = fresh_copy(src, tmp)
    mp = os.path.join(d, "metadata.json")
    m = json.load(open(mp, encoding="utf-8"))
    m["version"] = 3
    json.dump(m, open(mp, "w", encoding="utf-8"), ensure_ascii=False)
    rc, n, _ = probe(binpath, d)
    check(rc != 0 and n == 0, "version=3 时拒读（只认 2）")

    # T5 articleCount 与 index 不符
    print("\nT5 articleCount 与 index 不符")
    d = fresh_copy(src, tmp)
    mp = os.path.join(d, "metadata.json")
    m = json.load(open(mp, encoding="utf-8"))
    m["articleCount"] = m["articleCount"] + 5
    json.dump(m, open(mp, "w", encoding="utf-8"), ensure_ascii=False)
    rc, n, _ = probe(binpath, d)
    check(rc != 0 and n == 0, "count 与 index 尺寸不符时拒读")

    # T6 整篇 chunk 文件被删 → 能开但读不了（load 返回负）
    print("\nT6 块文件缺失")
    d = fresh_copy(src, tmp)
    for n_ in list(os.listdir(d)):
        if n_.startswith("articles_"):
            os.remove(os.path.join(d, n_))
            break
    rc, n, out = probe(binpath, d)
    load_line = [l for l in out.splitlines() if l.startswith("LOAD ")]
    # 第一篇文章可能恰好在剩下的 chunk 里；只要不崩即可
    crashed = rc < 0
    check(not crashed, "缺块文件不崩（exit=%d，%s）" % (rc, load_line[0] if load_line else "无 LOAD 行"))

    # T7 索引里的 off 被改成越界值 → load 必须报错而不是读到野数据
    print("\nT7 索引偏移越界")
    d = fresh_copy(src, tmp)
    p = os.path.join(d, "index.bin")
    data = bytearray(open(p, "rb").read())
    # 第一条记录的 off 字段在 68..71
    struct.pack_into("<I", data, 68, 0x7FFFFF00)
    open(p, "wb").write(bytes(data))
    rc, n, out = probe(binpath, d)
    load_line = [l for l in out.splitlines() if l.startswith("LOAD ")]
    lv = int(load_line[0].split()[1]) if load_line else 0
    check(rc == 0 and lv < 0, "偏移越界时 load 返回负错误码（%d），不崩" % lv)

    # T8 帧头被破坏
    print("\nT8 帧头破坏")
    d = fresh_copy(src, tmp)
    # 找到第一条记录的真实 (chunk, off)，把该位置改成垃圾
    idx = open(os.path.join(d, "index.bin"), "rb").read()
    chunk, off = struct.unpack_from("<II", idx, 64)
    ap = os.path.join(d, "articles_%04d.dat" % chunk)
    blob = bytearray(open(ap, "rb").read())
    if off + 4 <= len(blob):
        blob[off:off + 4] = b"ZZZZ"
        open(ap, "wb").write(bytes(blob))
    rc, n, out = probe(binpath, d)
    load_line = [l for l in out.splitlines() if l.startswith("LOAD ")]
    lv = int(load_line[0].split()[1]) if load_line else 0
    check(lv < 0, "magic 被破坏时 load 返回负错误码（%d）" % lv)

    # T9 目录根本不存在
    print("\nT9 目录不存在")
    rc, n, _ = probe(binpath, "/nonexistent/wiki/dir")
    check(rc != 0 and n == 0, "不存在的目录返回 0")

    shutil.rmtree(tmp, ignore_errors=True)
    print("\n================ %d PASS / %d FAIL ================" % (g_pass, g_fail))
    return 1 if g_fail else 0


if __name__ == "__main__":
    sys.exit(main())
