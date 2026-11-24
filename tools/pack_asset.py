#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pack_asset.py — 打「资源包」(商店 type=asset 的 ZIP)

资源包不是应用: 它只是一堆要落到 TF 卡某个目录的文件 (仙剑的游戏数据就是这样)。
商店下载后由固件 `am_asset_install()` 就地解到 target 目录, 然后 apps_registry
里那条 `.requires=<target>` 的应用才会出现。

★ 必须用 **ZIP_STORED**(不压缩): 固件解包器只搬字节, 不链 inflate 库。
   (体积换可靠性 —— 25MB 的资源包解压零内存峰值, 也不会因为缺库而失败。)

用法:
    # 1) 只打包 + 更新 index 条目 (不上传)
    python3 tools/pack_asset.py make pal \
        --src ~/Desktop/LinTOS/XJQXZDOS \
        --target /sdcard/apps/app/pal \
        --label 仙剑奇侠传 --cat 游戏 \
        --desc "DOS 经典: 下载后解到 TF 卡, 游戏分类里出现"

    # 2) 顺手写进 store/index.json 与 dist/upload/index.json
    python3 tools/pack_asset.py make pal ... --write-index
"""
import argparse
import binascii
import json
import os
import shutil
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_EXT = (".exe", ".drv", ".js3", ".bat", ".com", ".dll")


def build_zip(src_dir, out_zip, arc_root=""):
    """把 src_dir 下所有文件打成一个 stored zip。返回 (文件数, 字节数)。"""
    files = []
    for name in sorted(os.listdir(src_dir)):
        p = os.path.join(src_dir, name)
        if not os.path.isfile(p):
            continue
        if name.lower().endswith(SKIP_EXT):
            continue
        files.append((p, arc_root + name))
    if not files:
        sys.exit("✗ %s 里没有可打包的文件" % src_dir)

    os.makedirs(os.path.dirname(out_zip), exist_ok=True)
    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_STORED) as z:
        for p, arc in files:
            z.write(p, arc)
    return len(files), os.path.getsize(out_zip)


def upsert_index(index_path, entry):
    """在 index.json 的 pkgs[] 里插入/替换同名条目。"""
    if os.path.exists(index_path):
        with open(index_path, encoding="utf-8") as f:
            idx = json.load(f)
    else:
        idx = {"ver": 1, "site": "https://bbk.linit.cn/bbk",
               "mirrors": ["https://bbk.linit.cn/bbk"], "pkgs": []}
    pkgs = idx.setdefault("pkgs", [])
    for i, p in enumerate(pkgs):
        if p.get("name") == entry["name"]:
            pkgs[i] = entry
            break
    else:
        pkgs.append(entry)
    idx["updated"] = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(index_path, "w", encoding="utf-8") as f:
        json.dump(idx, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return len(pkgs)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    mk = sub.add_parser("make", help="打资源包并(可选)写索引")
    mk.add_argument("name", help="包名(英文, 用于文件名) 例如 pal")
    mk.add_argument("--src", required=True, help="源目录")
    mk.add_argument("--target", required=True, help="解包目标目录, 例如 /sdcard/apps/app/pal")
    mk.add_argument("--label", required=True, help="商店显示名")
    mk.add_argument("--cat", default="游戏", help="分类")
    mk.add_argument("--desc", default="", help="商店列表第二行说明")
    mk.add_argument("--ver", type=int, default=1)
    mk.add_argument("--out", default=os.path.join(ROOT, "dist", "upload"),
                    help="输出根目录(含 pkgs/ 与 index.json)")
    mk.add_argument("--write-index", action="store_true",
                    help="同时写 <out>/index.json 与 store/index.json")
    args = ap.parse_args()

    out_zip = os.path.join(args.out, "pkgs", args.name + ".zip")
    n, size = build_zip(args.src, out_zip)
    with open(out_zip, "rb") as f:
        data = f.read()
    crc = "%08x" % (binascii.crc32(data) & 0xFFFFFFFF)
    print("✓ %s: %d 个文件, %.1f MB, crc=%s" % (out_zip, n, size / 1048576, crc))

    entry = {
        "name": args.name,
        "label": args.label,
        "cat": args.cat,
        "desc": args.desc,
        "ver": args.ver,
        "size": size,
        "crc32": crc,
        "path": "pkgs/%s.zip" % args.name,
        "type": "asset",
        "target": args.target,
    }
    if args.write_index:
        for ip in (os.path.join(args.out, "index.json"),
                   os.path.join(ROOT, "store", "index.json")):
            n_pkg = upsert_index(ip, entry)
            print("✓ %s: %d 个包" % (ip, n_pkg))


if __name__ == "__main__":
    main()
