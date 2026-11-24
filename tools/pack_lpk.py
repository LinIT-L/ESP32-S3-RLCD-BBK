#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pack_lpk.py — 打包 / 检查 / 解包 LinTOS 自包含资源包 (.lpk)

格式与 components/services/lpk/lpk1.h 严格对应（那边是设备端唯一读取实现）。
一句话: 一个文件装下 Lua 程序 + 全部资源, 运行时按偏移直接读, **不解压、不散落**。

用法
----
    # 打包（Lua 应用 + 资源目录）
    python tools/pack_lpk.py --lua main.lua --assets ./assets -o pal.lpk

    # 打包（纯资源包, 例如仙剑这类 C 引擎游戏: 没有 Lua 入口）
    python tools/pack_lpk.py --assets ./pal_data -o pal.lpk --name pal

    # 检查
    python tools/pack_lpk.py --inspect pal.lpk
    python tools/pack_lpk.py --inspect pal.lpk --verify      # 逐文件校验 CRC（慢）

    # 解包到主机目录（仅开发调试；设备端永远不解包）
    python tools/pack_lpk.py --extract pal.lpk --out ./unpacked

设计要点
--------
· 不压缩：设备端零解压依赖、零解压内存峰值；TF 卡只写一遍。
· 索引前置：打开时一次读几十 KB 就能建表，之后每个资源就是一段 [off, off+size)。
· 资源数据按 512 字节对齐：单次读不会跨扇区起头（109 个文件最多浪费 ~55 KB，0.1%）。
· 打包完成后**重新打开校验一遍**（魔数/条数/逐条 CRC），不是写完就完事。
"""
import argparse
import fnmatch
import hashlib
import re
import json
import os
import struct
import sys
import zipfile
import zlib

MAGIC = b"LPK1"
VERSION = 1
HDR_SIZE = 64
ENTRY_SIZE = 32
ALIGN = 512
MAX_NAME = 192
MAX_ENTRIES = 8192

CRC_T = []


def crc32_update(crc, data):
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def crc32_file(path):
    c = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            c = crc32_update(c, b)
    return c


def path_ok(rel):
    """与 lpk1.c 的 lpk_path_ok 完全一致 —— 两侧规则必须同一套。"""
    if not rel or len(rel.encode("utf-8")) >= MAX_NAME:
        return False
    if rel.startswith("/") or rel.endswith("/"):
        return False
    for ch in rel:
        o = ord(ch)
        if o < 0x20 or o == 0x7F:
            return False
        if ch in ("\\", ":"):
            return False
    for seg in rel.split("/"):
        if seg in ("", ".", ".."):
            return False
    return True


class ZipSrc:
    """把一个 zip 条目当作资源源。打包时直接从 zip 流式读，不落到磁盘。"""
    __slots__ = ("zf", "name", "size", "crc")

    def __init__(self, zf, name):
        self.zf = zf
        self.name = name
        info = zf.getinfo(name)
        self.size = info.file_size
        self.crc = info.CRC & 0xFFFFFFFF


def src_size(src):
    return src.size if isinstance(src, ZipSrc) else os.path.getsize(src)


def src_crc(src):
    return src.crc if isinstance(src, ZipSrc) else crc32_file(src)


def src_open(src):
    return src.zf.open(src.name) if isinstance(src, ZipSrc) else open(src, "rb")


# 打包时默认跳过的"非游戏资源"：上一轮就是漏了这个，把测试用的 manifest.json
# 一起打进了 pal.lpk（110 条 vs zip 的 109 条）。真资源不会叫这些名字。
DEFAULT_EXCLUDES = ["manifest.json", ".DS_Store", "Thumbs.db",
                    "*.tmp", "*.part", "*.bak"]


def _excluded(rel, excludes):
    for pat in excludes:
        if fnmatch.fnmatch(rel, pat) or fnmatch.fnmatch(os.path.basename(rel), pat):
            return True
    return False


def _finish(out, root_label, excludes):
    """排序 + 重复路径检查 + 统计被排除的条目。"""
    out.sort(key=lambda x: x[0])
    seen = {}
    for rel, _src in out:
        low = rel.lower()
        if low in seen:
            raise SystemExit("⛔ 重复资源路径（仅大小写不同也不行，设备端无法区分）: %r / %r"
                             % (seen[low], rel))
        seen[low] = rel
    return out


def collect_assets(root, excludes=None):
    """返回 [(相对路径, 源)]，已排序、已做路径合法性检查、已过滤排除项。"""
    excludes = DEFAULT_EXCLUDES if excludes is None else excludes
    out = []
    skipped = []
    renamed = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            raw = os.path.relpath(full, root)
            rel = raw.replace("\\", "/")
            if raw != rel:
                renamed.append((raw, rel))
            if _excluded(rel, excludes):
                skipped.append(rel)
                continue
            if not path_ok(rel):
                raise SystemExit("⛔ 资源路径不合法（设备端会拒收）: %r" % rel)
            if os.path.getsize(full) == 0:
                raise SystemExit("⛔ 空文件（设备端会拒收）: %r" % rel)
            out.append((rel, full))
    out = _finish(out, root, excludes)
    if renamed:
        # ⚠ 反斜杠被改成 '/' 是必要的（Windows 上 relpath 就是这种），但**不能静默**：
        #   在 macOS/Linux 上 '\\' 是合法文件名字符，改完等于悄悄改了资源名。
        #   真要如此就得让人看见（重名冲突会在 _finish 里直接拒绝）。
        print("   ⚠ %d 个文件名里的反斜杠被改成正斜杠（例如 %r → %r）"
              % (len(renamed), renamed[0][0], renamed[0][1]))
    if skipped:
        print("   排除 %d 项: %s" % (len(skipped), ", ".join(skipped[:8]) +
                                    (" …" if len(skipped) > 8 else "")))
    return out


def collect_zip(zip_path, excludes=None):
    """直接从 zip 条目打包 —— 「以原始 ZIP 内容为准」的唯一可靠做法。

    好处: 不会被解压目录里残留的测试文件污染（manifest.json 就是这么混进来的），
    也不会因为解压时文件名大小写/编码差异而漂移。
    """
    excludes = DEFAULT_EXCLUDES if excludes is None else excludes
    out = []
    skipped = []
    renamed = []
    zf = zipfile.ZipFile(zip_path)          # 不在这里关：build() 还要用它流式读
    for info in zf.infolist():
        if info.is_dir():
            continue
        rel = info.filename.replace("\\", "/")
        if rel != info.filename:
            renamed.append((info.filename, rel))
        if rel.startswith("/"):
            rel = rel.lstrip("/")
        if _excluded(rel, excludes):
            skipped.append(rel)
            continue
        if not path_ok(rel):
            zf.close()
            raise SystemExit("⛔ zip 内路径不合法（设备端会拒收）: %r" % rel)
        if info.file_size == 0:
            zf.close()
            raise SystemExit("⛔ zip 内空文件（设备端会拒收）: %r" % rel)
        out.append((rel, ZipSrc(zf, info.filename)))
    out = _finish(out, zip_path, excludes)
    if renamed:
        print("   ⚠ %d 个 zip 条目里的反斜杠被改成正斜杠（例如 %r → %r）"
              % (len(renamed), renamed[0][0], renamed[0][1]))
    if skipped:
        print("   排除 %d 项: %s" % (len(skipped), ", ".join(skipped[:8]) +
                                    (" …" if len(skipped) > 8 else "")))
    return out, zf



def build(lua_path, assets, out_path, name=""):
    """assets: [(相对路径, 源)]，源可以是磁盘路径或 ZipSrc。"""
    if len(assets) > MAX_ENTRIES:
        raise SystemExit("⛔ 资源条数 %d 超过上限 %d" % (len(assets), MAX_ENTRIES))
    lua_bytes = b""
    lua_crc = 0
    if lua_path:
        with open(lua_path, "rb") as f:
            lua_bytes = f.read()
        if not lua_bytes:
            raise SystemExit("⛔ Lua 入口是空文件: %s" % lua_path)
        lua_crc = crc32_update(0, lua_bytes)
    if not lua_bytes and not assets:
        raise SystemExit("⛔ 既没有 Lua 也没有资源，拒绝生成空包")

    # ---- 布局计算 ----
    lua_off = HDR_SIZE
    man_off = lua_off + len(lua_bytes)
    man_off = (man_off + 3) & ~3                      # 索引按 4 字节对齐

    # 名字池：紧跟在记录数组后面。name_off 是**相对 man_off（索引块起点）**的偏移，
    # 与设备端 lpk1.c 的 `nm = meta + name_off` 保持一致（不是相对池起点）。
    names = []
    pool = bytearray()
    base = len(assets) * ENTRY_SIZE
    for rel, _full in assets:
        nb = rel.encode("utf-8")
        names.append((base + len(pool), len(nb)))
        pool += nb
    man_size = len(assets) * ENTRY_SIZE + len(pool)
    data_off = man_off + man_size
    data_off = (data_off + ALIGN - 1) & ~(ALIGN - 1)   # 资源数据 512 对齐

    # ---- 逐条算偏移 ----
    entries = []
    cur = data_off
    for i, (rel, src) in enumerate(assets):
        size = src_size(src)
        crc = src_crc(src)
        entries.append({"name": rel, "off": cur, "size": size, "crc32": crc,
                        "name_off": names[i][0], "name_len": names[i][1], "src": src})
        cur += size
        cur = (cur + ALIGN - 1) & ~(ALIGN - 1)
    total_size = cur

    # ---- 写文件 ----
    tmp = out_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(b"\x00" * HDR_SIZE)                   # 头最后回填（pkg_crc 要整包算）
        f.write(lua_bytes)
        f.write(b"\x00" * (man_off - lua_off - len(lua_bytes)))
        recs = bytearray()
        for i, e in enumerate(entries):
            rec = struct.pack("<IIIIHBBIII", e["name_off"], e["off"], e["size"],
                              e["crc32"], e["name_len"], 0, 0, 0, 0, 0)
            assert len(rec) == ENTRY_SIZE, len(rec)
            recs += rec
        f.write(recs)
        f.write(pool)
        f.write(b"\x00" * (data_off - man_off - man_size))
        for e in entries:
            pos = f.tell()
            assert pos == e["off"], "偏移计算错误 %d != %d" % (pos, e["off"])
            with src_open(e["src"]) as sf:
                while True:
                    b = sf.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
            pad = ((f.tell() + ALIGN - 1) & ~(ALIGN - 1)) - f.tell()
            if pad:
                f.write(b"\x00" * pad)

    real = os.path.getsize(tmp)
    assert real == total_size, "写出的文件 %d != 预算 %d" % (real, total_size)

    # ---- 先写完整头（pkg_crc 置 0），再算整包 CRC，最后回填 4 字节 ----
    #  约定: pkg_crc = 整个文件在 **[40,44) 视为 0** 时的 CRC32。
    #  设备端 lpk_verify_pkg() 按同一约定算，两边必须一致。
    hdr = struct.pack("<4sHH10I16s",
                      MAGIC, VERSION, HDR_SIZE,
                      total_size,
                      lua_off, len(lua_bytes), lua_crc,
                      man_off, man_size,
                      len(entries),
                      data_off,
                      0,                                   # pkg_crc 占位
                      1 if entries else 0,
                      b"\x00" * 16)
    assert len(hdr) == HDR_SIZE, len(hdr)
    with open(tmp, "r+b") as f:
        f.seek(0)
        f.write(hdr)

    pkg_crc = crc32_file(tmp)          # 此刻 [40,44) 就是 0
    with open(tmp, "r+b") as f:
        f.seek(40)
        f.write(struct.pack("<I", pkg_crc))

    os.replace(tmp, out_path)

    # ---- 自检：重新打开逐条校验 ----
    ok, msg = inspect(out_path, verify=True)
    if not ok:
        raise SystemExit("⛔ 生成后自检失败: " + msg)

    print("✅ %s" % out_path)
    print("   资源 %d 个 | 包体 %d B (%.2f MB) | Lua 入口 %d B | 索引 %d B"
          % (len(entries), total_size, total_size / 1048576.0, len(lua_bytes), man_size))
    print("   布局: 头 64 | Lua @%d | 索引 @%d | 数据 @%d | pkg_crc=%08x"
          % (lua_off, man_off, data_off, pkg_crc))
    if entries:
        big = sorted(entries, key=lambda x: -x["size"])[:3]
        print("   最大: " + ", ".join("%s %.2f MB" % (x["name"], x["size"] / 1048576.0) for x in big))
    if name:
        print("   包名: %s" % name)
    return 0


def read_header(f):
    f.seek(0)
    hdr = f.read(HDR_SIZE)
    if len(hdr) < HDR_SIZE:
        raise ValueError("文件太短，连头都不完整")
    magic, ver, hsz, total, lua_off, lua_size, lua_crc, man_off, man_size, \
        count, data_off, pkg_crc, flags, _resv = struct.unpack("<4sHH10I16s", hdr)
    if magic != MAGIC:
        raise ValueError("魔数不对: %r" % magic)
    if ver != VERSION:
        raise ValueError("格式版本不支持: %d" % ver)
    if hsz != HDR_SIZE:
        raise ValueError("头长度异常: %d" % hsz)
    return dict(total=total, lua_off=lua_off, lua_size=lua_size, lua_crc=lua_crc,
                man_off=man_off, man_size=man_size, count=count, data_off=data_off,
                pkg_crc=pkg_crc, flags=flags)


def inspect(path, verify=False):
    fsz = os.path.getsize(path)
    with open(path, "rb") as f:
        h = read_header(f)
        if h["total"] != fsz:
            return False, "包体大小 %d != 头声明 %d（被截断？）" % (fsz, h["total"])
        rows = []
        if h["count"]:
            f.seek(h["man_off"])
            meta = f.read(h["man_size"])
            if len(meta) != h["man_size"]:
                return False, "索引读取不完整"
            for i in range(h["count"]):
                r = meta[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
                name_off, off, size, crc, nlen, typ = struct.unpack("<IIIIHB", r[:19])
                nm = meta[name_off:name_off + nlen].decode("utf-8", "replace")
                if not path_ok(nm):
                    return False, "第 %d 条路径不合法: %r" % (i, nm)
                if off + size > h["total"]:
                    return False, "第 %d 条越界: %s" % (i, nm)
                good = None
                if verify:
                    f.seek(off)
                    c = 0
                    left = size
                    while left > 0:
                        b = f.read(min(left, 1 << 20))
                        if not b:
                            return False, "读不到数据: %s" % nm
                        c = crc32_update(c, b)
                        left -= len(b)
                    good = (c == crc)
                    if not good:
                        return False, "CRC 不符: %s (%08x != %08x)" % (nm, c, crc)
                rows.append((i, nm, off, size, crc, good))
    return True, (h, rows)


def cmd_inspect(path, verify):
    ok, res = inspect(path, verify)
    if not ok:
        print("❌ %s" % res)
        return 2
    h, rows = res
    print("✅ %s" % path)
    print("   格式 LPK%d | 包体 %d B (%.2f MB) | 资源 %d 个 | 索引 @%d (%d B)"
          % (VERSION, h["total"], h["total"] / 1048576.0, h["count"], h["man_off"], h["man_size"]))
    if h["lua_size"]:
        print("   Lua 入口 @%d %d B crc=%08x" % (h["lua_off"], h["lua_size"], h["lua_crc"]))
    else:
        print("   Lua 入口: 无（纯资源包）")
    print("   pkg_crc=%08x flags=%d" % (h["pkg_crc"], h["flags"]))
    if rows:
        print("   %-4s %-34s %10s %10s  %s" % ("#", "路径", "大小", "偏移", "CRC"))
        for i, nm, off, size, crc, good in rows[:200]:
            flag = "" if good is None else ("✓" if good else "✗")
            print("   %-4d %-34s %10d %10d  %08x %s" % (i, nm[:34], size, off, crc, flag))
        if len(rows) > 200:
            print("   … 其余 %d 条省略" % (len(rows) - 200))
        print("   合计 %d B" % sum(r[3] for r in rows))
    return 0


def cmd_extract(path, out):
    ok, res = inspect(path, verify=True)
    if not ok:
        print("❌ %s" % res)
        return 2
    h, rows = res
    os.makedirs(out, exist_ok=True)
    with open(path, "rb") as f:
        if h["lua_size"]:
            f.seek(h["lua_off"])
            with open(os.path.join(out, "__entry__.lua"), "wb") as g:
                g.write(f.read(h["lua_size"]))
        for _i, nm, off, size, _crc, _g in rows:
            dst = os.path.join(out, nm)
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            f.seek(off)
            left = size
            with open(dst, "wb") as g:
                while left > 0:
                    b = f.read(min(left, 1 << 20))
                    if not b:
                        raise SystemExit("读失败: %s" % nm)
                    g.write(b)
                    left -= len(b)
    print("✅ 已解包到 %s（%d 个资源）—— 仅开发调试用，设备端不解包" % (out, len(rows)))
    return 0


# ============================ 应用包（manifest.json）============================
# 规则与设备端 components/services/lpk/lpk_manifest.c **逐条对齐** —— 两边判据一旦
# 分叉，就会出现"工具打得出来、设备装不进去"这种最难受的故障。
MF_NAME   = "manifest.json"
MF_ICON   = "icon.bin"
ICON_BYTES = 512          # 64×64 1bpp
APPID_RE  = re.compile(r"^[a-z][a-z0-9._-]*$")

RUNTIMES = ("lua", "native", "engine")


def mf_id_ok(s):
    if not s or len(s) < 3 or len(s) >= 64:
        return False
    if not APPID_RE.match(s):
        return False
    if ".." in s or s.endswith("."):
        return False
    parts = s.split(".")
    return len(parts) >= 2 and all(parts)


def mf_path_ok(rel):
    if not rel or rel.startswith("/") or rel.endswith("/"):
        return False
    if "\\" in rel or ".." in rel or "//" in rel:
        return False
    return all(ord(c) >= 0x20 for c in rel)


def mf_validate(app_dir, mf, fail):
    """校验 manifest；fail(msg) 负责中止。返回 (ok, info)"""
    if mf.get("schema_version") != 1:
        fail("schema_version 必须为 1")
    if not mf_id_ok(mf.get("app_id", "")):
        fail("app_id 非法（需形如 com.lintos.recorder）: %r" % mf.get("app_id"))
    if not mf.get("label"):
        fail("label 为空")
    if not mf.get("version"):
        fail("version 为空")

    ptype = mf.get("package_type")
    if ptype not in ("app", "resource"):
        fail("package_type 必须是 app 或 resource")

    if ptype == "app":
        rt = mf.get("runtime")
        if rt not in RUNTIMES:
            fail("runtime 必须是 %s 之一（实际 %r）" % ("/".join(RUNTIMES), rt))
        entry = mf.get("entry") or ""
        if not mf_path_ok(entry):
            fail("entry 路径非法: %r" % entry)
        if not os.path.isfile(os.path.join(app_dir, entry)):
            fail("entry 指向的文件不在应用目录里: %s" % entry)
    else:
        if not mf.get("resource_type"):
            fail("resource 包必须有 resource_type")
        if mf.get("entry"):
            fail("resource 包不应有 entry")

    icon = mf.get("icon") or ""
    if icon:
        if not mf_path_ok(icon):
            fail("icon 路径非法: %r" % icon)
        ip = os.path.join(app_dir, icon)
        if not os.path.isfile(ip):
            fail("icon 指向的文件不在应用目录里: %s" % icon)
        if os.path.getsize(ip) != ICON_BYTES:
            fail("icon 必须正好 %d 字节（64×64 1bpp），实际 %d"
                 % (ICON_BYTES, os.path.getsize(ip)))
    return True


def cmd_pack_app(app_dir, out_path, do_list=False, index_path=None):
    """把一个应用目录（manifest.json + icon.bin + 程序/资源）打成单个 .lpk"""
    if not os.path.isdir(app_dir):
        raise SystemExit("⛔ 不是目录: %s" % app_dir)

    def fail(msg):
        raise SystemExit("⛔ manifest 校验失败: %s" % msg)

    mp = os.path.join(app_dir, MF_NAME)
    if not os.path.isfile(mp):
        raise SystemExit("⛔ 缺少 %s（应用包必须带 manifest）" % MF_NAME)
    try:
        mf = json.load(open(mp, encoding="utf-8"))
    except Exception as e:
        raise SystemExit("⛔ manifest.json 不是合法 JSON: %s" % e)
    if not isinstance(mf, dict):
        raise SystemExit("⛔ manifest.json 顶层必须是对象")

    mf_validate(app_dir, mf, fail)

    # 重复 App ID 检查（防止同一个应用换壳上架两次）
    if index_path and os.path.isfile(index_path):
        try:
            idx = json.load(open(index_path, encoding="utf-8"))
            for pk in idx.get("packages") or idx.get("pkgs") or []:
                if pk.get("app_id") and pk.get("app_id") == mf["app_id"]:
                    print("   ⚠ 索引里已存在同一 app_id：%s（%s）—— 这是升级还是冲突请确认"
                          % (mf["app_id"], pk.get("name", "?")))
        except Exception as e:
            print("   ⚠ 索引读取失败，跳过重复检查: %s" % e)

    # ⚠ 默认排除表里有 "manifest.json"（那是给仙剑这类纯资源包用的，怕误把清单打进包）。
    #   应用包里 manifest 就是身份本身 ⇒ 必须留下，其余默认排除继续生效。
    app_excl = [x for x in DEFAULT_EXCLUDES if x != MF_NAME]
    assets = collect_assets(app_dir, excludes=app_excl)
    pairs = dict(assets)
    if MF_NAME not in pairs:
        raise SystemExit("⛔ %s 没被打进包（检查排除表）" % MF_NAME)

    build(None, assets, out_path, name=mf.get("label", ""))

    raw = open(out_path, "rb").read()
    size = len(raw)
    crc = format(zlib.crc32(raw) & 0xFFFFFFFF, "08x")
    sha = hashlib.sha256(raw).hexdigest()

    print("✅ 应用包打包完成: %s" % out_path)
    print("   app_id      : %s" % mf["app_id"])
    print("   label       : %s  (%s)" % (mf.get("label"), mf.get("name", "")))
    print("   package_type: %s%s" % (mf.get("package_type"),
                                     "" if mf.get("package_type") == "app"
                                     else " / " + str(mf.get("resource_type"))))
    if mf.get("package_type") == "app":
        print("   runtime     : %s   entry=%s" % (mf.get("runtime"), mf.get("entry")))
    print("   version     : %s   build=%s" % (mf.get("version"), mf.get("build", "")))
    print("   文件数      : %d" % len(assets))
    print("   大小        : %d 字节" % size)
    print("   CRC32       : %s" % crc)
    print("   SHA-256     : %s" % sha)

    rec = {
        "app_id": mf["app_id"],
        "name": mf.get("label"),
        "category": mf.get("category", "software"),
        "package_type": mf.get("package_type"),
        "version": mf.get("version"),
        "build": mf.get("build", ""),
        "size": size,
        "crc32": crc,
        "sha256": sha,
        "path": os.path.basename(out_path),
    }
    print("   索引记录    : " + json.dumps(rec, ensure_ascii=False))

    if do_list:
        for rel, _ in assets:
            print("     -", rel)


def main():
    ap = argparse.ArgumentParser(description="打包/检查/解包 LinTOS .lpk 自包含资源包")
    ap.add_argument("--lua", help="Lua 入口文件（纯资源包可不给）")
    ap.add_argument("--assets", help="资源目录（递归）")
    ap.add_argument("--zip", help="直接从 zip 打包（以原始 zip 内容为准，推荐给仙剑这类发行包）")
    ap.add_argument("--exclude", action="append", default=[],
                    help="额外排除的 glob（可重复）；默认已排除 %s"
                         % ", ".join(DEFAULT_EXCLUDES))
    ap.add_argument("--no-default-exclude", action="store_true",
                    help="不套用默认排除表（调试用）")
    ap.add_argument("--list", action="store_true", help="打包后列出全部资源")
    ap.add_argument("-o", "--output", help="输出 .lpk 路径")
    ap.add_argument("--name", default="", help="包名（仅打印用）")
    ap.add_argument("--inspect", help="检查已有 .lpk")
    ap.add_argument("--verify", action="store_true", help="检查时逐文件校验 CRC")
    ap.add_argument("--app", help="应用目录（必须含 manifest.json；自动校验后打成 LPK）")
    ap.add_argument("--index", default="store/index.json",
                    help="重复 app_id 检查用的索引（默认 store/index.json）")
    ap.add_argument("--extract", help="解包已有 .lpk（仅调试）")
    ap.add_argument("--out", help="解包目标目录")
    a = ap.parse_args()

    if a.app:
        if not a.output:
            ap.error("--app 需要 -o/--output")
        return cmd_pack_app(a.app, a.output, a.list, a.index)

    if a.inspect:
        return cmd_inspect(a.inspect, a.verify)
    if a.extract:
        if not a.out:
            ap.error("--extract 需要 --out")
        return cmd_extract(a.extract, a.out)
    if not a.output:
        ap.error("打包需要 -o/--output")
    if not a.lua and not a.assets and not a.zip:
        ap.error("至少给 --lua / --assets / --zip 之一")
    if a.assets and a.zip:
        ap.error("--assets 与 --zip 只能二选一")
    for p, label in ((a.lua, "--lua"), (a.assets, "--assets"), (a.zip, "--zip")):
        if p and not os.path.exists(p):
            ap.error("%s 不存在: %s" % (label, p))

    excludes = ([] if a.no_default_exclude else list(DEFAULT_EXCLUDES)) + a.exclude

    zf = None
    if a.zip:
        assets, zf = collect_zip(a.zip, excludes)
        print("   来源: zip %s（%d 个资源）" % (a.zip, len(assets)))
    else:
        assets = collect_assets(a.assets, excludes) if a.assets else []

    if a.list:
        print("   ---- 资源清单 ----")
        for rel, src in assets:
            print("   %9d  %s" % (src_size(src), rel))
        print("   ---- 共 %d 项, %d B ----"
              % (len(assets), sum(src_size(s) for _r, s in assets)))

    try:
        return build(a.lua, assets, a.output, a.name)
    finally:
        if zf is not None:
            zf.close()


if __name__ == "__main__":
    sys.exit(main())
