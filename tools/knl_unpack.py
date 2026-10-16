#!/usr/bin/env python3
"""knl_unpack.py — 步步高 9288 `KNL` 固件容器解析器 (2026-10-11 逆向)

格式 (从 `9288.exe` 的错误串 + 实文件对齐推出的):
    0x00  4B  魔数 "KNL " (含结尾空格)
    0x04  16B 版本/日期串, 形如 "0100200704041714" → 2007-04-04 17:14
    0x14  u32 payload 长度 (= 文件大小 - 64)
    0x18  u32 CRC32
    0x1C  u32 入口 / 向量偏移
    0x20  32B 保留(全 0)
    0x40  ... payload: 以「32 位地址表(向量表)」开头, 地址落在 0x02xxxxxx 段

用法:
    python3 tools/knl_unpack.py <9288_kernel.bin> [--dump-table N] [--out-seg DIR]
"""
import struct
import sys
import os
import zlib

HDR = 64


def parse(path):
    d = open(path, "rb").read()
    rep = {"file": os.path.basename(path), "size": len(d)}
    if d[:4] != b"KNL ":
        raise SystemExit("不是 KNL 文件 (前 4 字节 = %r)" % d[:4])
    rep["ver"] = d[4:20].decode("ascii", "replace")
    # 日期串是 4 组四位数字: 版本/年/月日/时分
    v = rep["ver"]
    if len(v) == 16 and v.isdigit():
        rep["ver_decoded"] = "v%s  %s-%s-%s  %s:%s" % (
            v[0:4], v[4:8], v[8:10], v[10:12], v[12:14], v[14:16])
    rep["payload_len"] = struct.unpack_from("<I", d, 0x14)[0]
    rep["crc32_hdr"] = struct.unpack_from("<I", d, 0x18)[0]
    rep["entry"] = struct.unpack_from("<I", d, 0x1C)[0]
    rep["len_ok"] = (rep["payload_len"] == len(d) - HDR)
    body = d[HDR:]
    rep["crc32_real"] = zlib.crc32(body) & 0xFFFFFFFF
    rep["table"] = [struct.unpack_from("<I", body, i)[0] for i in range(0, min(256, len(body)), 4)]
    rep["body"] = body
    return rep


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    r = parse(path)
    print("=" * 68)
    print("文件: %s   %d 字节" % (r["file"], r["size"]))
    print("  版本串 : %s   %s" % (r["ver"], r.get("ver_decoded", "")))
    print("  payload: %d 字节   (文件-64 = %d)  %s" % (
        r["payload_len"], r["size"] - HDR, "✅ 对齐" if r["len_ok"] else "❌ 不对齐"))
    print("  入口   : 0x%08X" % r["entry"])
    print("  CRC32  : 头声明 0x%08X / 实测(payload) 0x%08X  %s" % (
        r["crc32_hdr"], r["crc32_real"],
        "✅ 匹配" if r["crc32_hdr"] == r["crc32_real"] else "⚠ 不匹配(payload 外还有内容或用别的算法)"))
    print()
    print("  payload 前 %d 个 32 位字 (看是否像向量表/地址表):" % min(24, len(r["table"])))
    for i in range(0, min(24, len(r["table"])), 4):
        row = r["table"][i:i + 4]
        print("    +%04X: %s" % (i * 4, "  ".join("%08X" % x for x in row)))
    # 地址段统计
    segs = {}
    for i, v in enumerate(r["table"]):
        seg = v >> 24
        segs[seg] = segs.get(seg, 0) + 1
    print()
    print("  前 256 字的高位字节分布 (判断地址段):",
          {("0x%02X000000" % k): v for k, v in sorted(segs.items())})


if __name__ == "__main__":
    main()
