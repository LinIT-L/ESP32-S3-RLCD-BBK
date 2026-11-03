#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""wuxia_bundle.py — 把《英雄坛说》打成一个**应用+素材**的资源包 (2026-10-31)

为什么改成"一个包"
------------------
原来主程序(.lua)与素材(wuxia.zip)是商店里的**两条**，用户要点两次、还会出现
"装了主程序但没装素材 → 打开白屏"。现在合成一条：

    store/pkgs/英雄坛说.zip  ←  英雄坛说.lua (LAPP, 已带图标/头部)
                             wuxia/*.bmp|*.lua (63 张场景 + 图集 + data/world/map)

索引里的条目是 `type=asset` + `target=/sdcard/apps/app`，装的时候
`am_asset_install()` 直接解到应用目录：
    /sdcard/apps/app/英雄坛说.lua     ← 应用管家立刻能看到
    /sdcard/apps/app/wuxia/...        ← 游戏 ROOTS[1] 找的就是这里
所以"点一次安装 = 下载 + 自动解压 = 能玩"，不需要第二次点击。

安全性 (为什么敢解到共享目录 /sdcard/apps/app)
---------------------------------------------
am_asset 会写 `.pkg.wuxia.lst` 清单，卸载时**只删清单里的文件**；目录非空就不删目录
(am_asset.c:249 起)。所以同目录下别的应用不会被误删。

用法
----
    python3 tools/wuxia_bundle.py            # 打包 + 更新 store/index.json
    python3 tools/wuxia_bundle.py --check    # 只校验 zip 与索引是否一致
"""
import argparse
import binascii
import io
import json
import os
import re
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART_DIR = os.path.join(ROOT, "apps", "lua", "wuxia")
APP_LUA = os.path.join(ROOT, "apps", "lua", "apps", "app", "英雄坛说.lua")
OUT_ZIP = os.path.join(ROOT, "store", "pkgs", "英雄坛说.zip")
INDEX = os.path.join(ROOT, "store", "index.json")
ARC_APP = "英雄坛说.lua"          # 解压后落在 /sdcard/apps/app/英雄坛说.lua
ARC_ART = "wuxia"                 # 素材目录前缀 → /sdcard/apps/app/wuxia/
TARGET = "/sdcard/apps/app"
NAME = "英雄坛说"
CAT = "游戏"


def lapp_meta(path):
    head = open(path, "rb").read(4096).decode("utf-8", "replace")
    d = {}
    for k, v in re.findall(r"--@app\s+([a-z_]+)=([^\r\n]*)", head):
        d[k] = v.strip()
    return d


def build_zip():
    meta = lapp_meta(APP_LUA)
    if not meta.get("name"):
        sys.exit("✗ %s 没有 LAPP 头 —— 先跑 tools/pack_app.py" % APP_LUA)
    if not os.path.isdir(ART_DIR):
        sys.exit("✗ 素材目录不存在: %s" % ART_DIR)
    files = sorted(os.listdir(ART_DIR))
    if not any(f.endswith(".bmp") for f in files):
        sys.exit("✗ 素材目录里没有 .bmp —— 先跑 tools/wuxia_pack.py")

    os.makedirs(os.path.dirname(OUT_ZIP), exist_ok=True)
    n = 0
    with zipfile.ZipFile(OUT_ZIP, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(APP_LUA, ARC_APP)
        n += 1
        for f in files:
            p = os.path.join(ART_DIR, f)
            if os.path.isfile(p):
                z.write(p, "%s/%s" % (ARC_ART, f))
                n += 1
    return meta, n


def update_index(meta, n_files):
    data = open(OUT_ZIP, "rb").read()
    size = len(data)
    crc = "%08x" % (binascii.crc32(data) & 0xFFFFFFFF)
    entry = {
        "name": NAME,
        "label": NAME,
        "cat": CAT,
        "desc": meta.get("desc", ""),
        "ver": int(meta.get("ver") or 1),
        "size": size,
        "crc32": crc,
        "path": "pkgs/英雄坛说.zip",
        "type": "asset",
        "target": TARGET,
        "author": "LinTOS",
        "license": "素材来自 gmud.online, 仅个人学习使用",
        "date": time.strftime("%Y-%m-%d"),
    }
    if meta.get("build"):
        entry["build"] = meta["build"]

    idx = json.load(open(INDEX, encoding="utf-8"))
    orig = idx["pkgs"]

    # 主程序条目与旧"资源包"条目都从索引里去掉 —— 现在只有一个包。
    # 位置按"原下标"重排，保证列表顺序稳定（不会因为删除而整体前移）。
    def is_mine(p):
        return p.get("name") in (NAME, NAME + "资源包") or p.get("path") == "pkgs/英雄坛说.zip"

    pos = next((i for i, p in enumerate(orig) if is_mine(p)), None)
    pairs = [(i, p) for i, p in enumerate(orig) if not is_mine(p)]
    if pos is None:
        pairs.append((len(orig), entry))
    else:
        at = 0
        for k, (i, _p) in enumerate(pairs):
            if i < pos:
                at = k + 1
        pairs.insert(at, (pos, entry))

    idx["pkgs"] = [p for _i, p in pairs]
    idx["ver"] = int(idx["ver"]) + 1
    idx["updated"] = time.strftime("%Y-%m-%d %H:%M:%S")
    with io.open(INDEX, "w", encoding="utf-8") as f:
        json.dump(idx, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return entry, n_files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只校验, 不重打包")
    args = ap.parse_args()

    if args.check:
        z = zipfile.ZipFile(OUT_ZIP)
        names = z.namelist()
        idx = json.load(open(INDEX, encoding="utf-8"))
        ent = [p for p in idx["pkgs"] if p.get("path") == "pkgs/英雄坛说.zip"]
        data = open(OUT_ZIP, "rb").read()
        ok = (len(ent) == 1 and ent[0]["size"] == len(data)
              and ent[0]["crc32"] == "%08x" % (binascii.crc32(data) & 0xFFFFFFFF)
              and ARC_APP in names and any(n.startswith(ARC_ART + "/") for n in names))
        print("zip 条目 %d, 含 %s: %s, 索引条目 %d 条, 校验: %s"
              % (len(names), ARC_APP, ARC_APP in names, len(ent), "OK" if ok else "FAIL"))
        return 0 if ok else 1

    meta, n = build_zip()
    ent, nf = update_index(meta, n)
    print("✓ %s" % os.path.relpath(OUT_ZIP, ROOT))
    print("  条目 %d 个 (%s + %s/ 下 %d 个)" % (nf, ARC_APP, ARC_ART, nf - 1))
    print("  %d B  crc=%s  ver=%s  build=%s" %
          (ent["size"], ent["crc32"], ent["ver"], ent.get("build", "-")))
    print("  索引条目: type=asset  target=%s" % ent["target"])
    print("  安装后: %s/%s  +  %s/%s/" % (TARGET, ARC_APP, TARGET, ARC_ART))


if __name__ == "__main__":
    main()
