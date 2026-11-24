#!/usr/bin/env python3
"""verify_pack.py — 核对 LPK 与**原始 ZIP** 是否一一对应（G3 的验收）

用法:
    python3 tests/lpk/verify_pack.py <pal.lpk> <pal.zip> [--exclude 名称 ...]

它独立解析 LPK（不 import pack_lpk），逐条比对：
  · 条目数是否相同
  · 名字集合是否相同（多一条少一条都算失败）
  · 每条 size / crc32 是否与 zip 里同名条目一致
  · 偏移是否落在文件内、区间是否互不重叠、是否 512 对齐
"""
import binascii
import os
import struct
import sys
import zipfile

HDR = 64
ENT = 32
DEFAULT_EXCLUDES = {"manifest.json", ".DS_Store", "Thumbs.db"}


def read_lpk(path):
    with open(path, "rb") as f:
        hdr = f.read(HDR)
        if len(hdr) != HDR or hdr[:4] != b"LPK1":
            raise SystemExit("不是 LPK1: %s" % path)
        ver, hsz = struct.unpack_from("<HH", hdr, 4)
        total, lua_off, lua_size, lua_crc, man_off, man_size, count, data_off, pkg_crc, flags = \
            struct.unpack_from("<10I", hdr, 8)
        f.seek(man_off)
        meta = f.read(man_size)
    if len(meta) != man_size:
        raise SystemExit("索引读不满")
    out = []
    for i in range(count):
        r = meta[i * ENT:(i + 1) * ENT]
        # 记录布局(见 lpk1.h): name_off u32 | off u32 | size u32 | crc u32 | nlen u16 | type u8
        name_off, off, size, crc, nlen = struct.unpack_from("<IIIIH", r, 0)
        name = meta[name_off:name_off + nlen].decode("utf-8")
        out.append((name, off, size, crc))
    return dict(ver=ver, total=total, count=count, data_off=data_off, pkg_crc=pkg_crc,
                flags=flags, entries=out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 2
    lpk, zp = args
    ex = set(DEFAULT_EXCLUDES)
    if "--exclude" in sys.argv:
        i = sys.argv.index("--exclude")
        ex = set(sys.argv[i + 1:]) | ex

    L = read_lpk(lpk)
    zf = zipfile.ZipFile(zp)
    zentries = {}
    skipped = []
    for info in zf.infolist():
        if info.is_dir():
            continue
        nm = info.filename.replace("\\", "/").lstrip("/")
        if os.path.basename(nm) in ex or nm in ex:
            skipped.append(nm)
            continue
        zentries[nm] = (info.file_size, info.CRC & 0xFFFFFFFF)

    print("LPK : %d 条, %d B, pkg_crc=%08x" % (L["count"], L["total"], L["pkg_crc"]))
    print("ZIP : %d 条（排除 %d 个非资源项%s）"
          % (len(zentries), len(skipped), (": " + ", ".join(skipped)) if skipped else ""))

    bad = 0
    lnames = [e[0] for e in L["entries"]]
    if len(lnames) != len(set(lnames)):
        print("  FAIL  LPK 内有重名条目"); bad += 1
    if len(lnames) != len(set(n.lower() for n in lnames)):
        print("  FAIL  LPK 内有仅大小写不同的重名条目"); bad += 1

    missing = sorted(set(zentries) - set(lnames))
    extra = sorted(set(lnames) - set(zentries))
    if missing:
        print("  FAIL  少了 %d 个: %s" % (len(missing), missing[:6])); bad += 1
    if extra:
        print("  FAIL  多了 %d 个: %s" % (len(extra), extra[:6])); bad += 1

    for name, off, size, crc in L["entries"]:
        if name not in zentries:
            continue
        zsz, zcrc = zentries[name]
        if size != zsz or crc != zcrc:
            print("  FAIL  %-18s size %d/%d crc %08x/%08x" % (name, size, zsz, crc, zcrc))
            bad += 1
        if off < L["data_off"] or off + size > L["total"]:
            print("  FAIL  %-18s 偏移越界 %d..%d" % (name, off, off + size)); bad += 1
        if off % 512:
            print("  FAIL  %-18s 偏移未 512 对齐 (%d)" % (name, off)); bad += 1

    ents = sorted(L["entries"], key=lambda x: x[1])
    for i in range(1, len(ents)):
        if ents[i][1] < ents[i - 1][1] + ents[i - 1][2]:
            print("  FAIL  条目区间重叠: %s / %s" % (ents[i - 1][0], ents[i][0])); bad += 1

    if not bad:
        print("  PASS  %d 条一一对应，size/crc 全等，偏移合法且 512 对齐" % len(lnames))
    print("\n==== %d FAIL ====" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
