#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mk_asset_manifest.py — 为资源包生成「文件清单」(资源商店 方案 B: 多文件直接下载)

为什么需要清单
--------------
原有流程是「下一个 41 MB 的 zip，再在设备上解包」——真机实测(仙剑 41.7 MB):
    下载 252 s + 解包 133 s = 385 s，而 TF 卡总共写了 83 MB(41.7 zip + 41.7 文件)。
系统 `cp` 同一个文件 200 s 都拷不完(<208 KB/s)，说明 313 KB/s 就是那张卡
"边读边写"的上限。唯一还能省的就是**别写两遍**。

于是改成: 服务器出清单 → 设备按清单逐个下载真实文件 → 直接落最终位置。
卡上只写一遍，且天然支持断点续传(已下好的文件按 size+CRC 跳过)。

用法
----
    # 从目录(推荐)
    python3 tools/mk_asset_manifest.py --name pal --src ~/Desktop/LinTOS/XJQXZDOS \
            --out dist/asset/pal --title 仙剑奇侠传 --ver 3

    # 从现有 zip(设备端以前下载的就是它，内容一致可直接复用)
    python3 tools/mk_asset_manifest.py --name pal --zip server/pal.zip \
            --out dist/asset/pal --ver 3

输出
----
    <out>/manifest.json          清单(设备拉这个)
    <out>/<相对路径...>           真实文件(与清单一一对应)

部署: 把整个 <out> 目录上传成 `bbk/pkgs/<name>/`，然后给 index.json 对应条目加一行
      "manifest": "pkgs/<name>/manifest.json"
      ⇒ 新固件走多文件下载；老固件/清单缺失仍走原来的 zip，互不影响。

安全
----
清单里的 path 必须是**相对路径**，且不含 `..` 段 / 反斜杠 / 盘符 / 控制字符。
本脚本在生成时就按设备端同一套规则校验(见 am_manifest.c 的 am_mf_path_ok)，
脏数据在这里就被打回，不会等到设备上才失败。
"""
import argparse
import json
import os
import shutil
import sys
import zipfile
import zlib

FMT = 1
MAX_PATH = 96          # 与 am_manifest.h 的 AM_MF_PATH_MAX 一致
MAX_FILES = 4096       # 与 AM_MF_MAX_FILES 一致


def norm_rel(rel: str) -> str:
    """把 zip 里的条目名规整成 '/' 分隔的相对路径。"""
    rel = rel.replace("\\", "/")
    while rel.startswith("./"):
        rel = rel[2:]
    return rel


def path_ok(rel: str) -> bool:
    """设备端 am_mf_path_ok() 的等价实现 —— 两边必须一致，否则生成得出、装不上。"""
    if not rel:
        return False
    if len(rel.encode("utf-8")) >= MAX_PATH:
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


def crc32_file(p: str) -> int:
    c = 0
    with open(p, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            c = zlib.crc32(b, c)
    return c & 0xFFFFFFFF


def collect_from_zip(zp: str, out: str):
    """解 zip 到 out（保留目录结构），返回相对路径列表。"""
    got = []
    with zipfile.ZipFile(zp) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            rel = norm_rel(info.filename)
            if ".." in rel.split("/") or rel.startswith("/"):
                print("  ⛔ zip 内含越界条目，拒绝: %r" % info.filename, file=sys.stderr)
                sys.exit(2)
            if not path_ok(rel):
                print("  ⛔ 条目路径不合规(设备端会拒收): %r" % rel, file=sys.stderr)
                sys.exit(2)
            dst = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            with z.open(info) as src, open(dst, "wb") as fo:
                shutil.copyfileobj(src, fo, 1 << 20)
            got.append(rel)
    return got


def collect_from_dir(src: str, out: str):
    """把 src 下的文件复制成 out 下的同一结构，返回相对路径列表。"""
    got = []
    for root, _dirs, files in os.walk(src):
        for fn in files:
            full = os.path.join(root, fn)
            rel = norm_rel(os.path.relpath(full, src))
            if not path_ok(rel):
                print("  ⛔ 路径不合规(设备端会拒收): %r" % rel, file=sys.stderr)
                sys.exit(2)
            dst = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            shutil.copy2(full, dst)
            got.append(rel)
    return got


def main():
    ap = argparse.ArgumentParser(description="生成资源包文件清单 (manifest.json)")
    ap.add_argument("--name", required=True, help="包名(与 zip 主干同名, 如 pal)")
    ap.add_argument("--src", help="源目录")
    ap.add_argument("--zip", help="源 zip(与 --src 二选一)")
    ap.add_argument("--out", required=True, help="输出目录(会被清空重建)")
    ap.add_argument("--title", default="", help="中文名(仅写进清单, 便于排查)")
    ap.add_argument("--ver", type=int, default=0, help="包版本号")
    ap.add_argument("--force", action="store_true", help="输出目录非空时仍然覆盖")
    args = ap.parse_args()

    if bool(args.src) == bool(args.zip):
        ap.error("--src 与 --zip 必须给且只给一个")
    src = args.src or args.zip
    if not os.path.exists(src):
        ap.error("源不存在: %s" % src)

    out = os.path.abspath(args.out)
    # 只在"自己产出的目录"上动手：里面有 manifest.json 或为空才允许重建，
    # 避免手滑把别的目录（比如源码树）清掉。
    if os.path.isdir(out) and os.listdir(out):
        if not args.force and not os.path.exists(os.path.join(out, "manifest.json")):
            print("⛔ 输出目录非空且不像本工具产出，加 --force 才会覆盖: %s" % out, file=sys.stderr)
            sys.exit(3)
        shutil.rmtree(out)
    os.makedirs(out, exist_ok=True)

    print("源: %s" % src)
    if args.zip:
        rels = collect_from_zip(src, out)
    else:
        rels = collect_from_dir(src, out)
    rels.sort()

    if not rels:
        print("⛔ 一个文件都没有", file=sys.stderr)
        sys.exit(2)
    if len(rels) > MAX_FILES:
        print("⛔ 文件数 %d 超过设备上限 %d" % (len(rels), MAX_FILES), file=sys.stderr)
        sys.exit(2)

    files = []
    total = 0
    for rel in rels:
        full = os.path.join(out, rel)
        size = os.path.getsize(full)
        if size <= 0:
            print("⛔ 空文件(设备端会拒收): %r" % rel, file=sys.stderr)
            sys.exit(2)
        c = crc32_file(full)
        if c == 0:
            print("⛔ crc32 恰好为 0(设备端判为字段无效): %r" % rel, file=sys.stderr)
            sys.exit(2)
        files.append({"path": rel, "size": size, "crc32": "%08x" % c})
        total += size

    mf = {
        "fmt": FMT,
        "pkg": args.name,
        "title": args.title,
        "ver": args.ver,
        "count": len(files),
        "total": total,
        "files": files,
    }
    mp = os.path.join(out, "manifest.json")
    with open(mp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(mf, f, ensure_ascii=False, indent=1)
        f.write("\n")

    msz = os.path.getsize(mp)
    # 自检：重新读回，确认 count/total 与文件表自洽（设备端也会这么验）
    back = json.load(open(mp, encoding="utf-8"))
    assert back["count"] == len(back["files"])
    assert back["total"] == sum(x["size"] for x in back["files"])
    assert all(path_ok(x["path"]) for x in back["files"])

    print("✅ 清单: %s" % mp)
    print("   文件数 %d | 总字节 %d (%.1f MB) | 清单 %d B | 包名 %s ver=%d"
          % (len(files), total, total / 1048576.0, msz, args.name, args.ver))
    big = sorted(files, key=lambda x: -x["size"])[:3]
    print("   最大的几个: " + ", ".join("%s %.2f MB" % (x["path"], x["size"] / 1048576.0) for x in big))
    print("\n部署:")
    print("  上传整个目录 → bbk/pkgs/%s/" % args.name)
    print("  index.json 该条目加: \"manifest\": \"pkgs/%s/manifest.json\"" % args.name)


if __name__ == "__main__":
    main()
