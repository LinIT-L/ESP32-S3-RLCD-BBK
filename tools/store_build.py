#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""store_build.py — 生成「应用商店」的公网分发包与索引。

产物（全部落在 <项目>/store/）：
    store/index.json          包清单：mirrors[] + 每个包的 相对 path/size/crc32/版本
    store/pkgs/<name>.lua     待装包（已带 LAPP 头，直接复制自 apps/lua/apps/）

设计要点（对应 docs/应用商店-多源托管方案.md）：
  ★ index.json 里**只存相对路径**，并内嵌 mirrors[] 列表。
    设备拼 `mirrors[i] + "/" + path` ⇒ **加源/换源只改这个 json，固件一行都不用动**。

用法：
    python3 tools/store_build.py                       # 默认 mirrors
    python3 tools/store_build.py --mirror https://bbk.linit.cn/bbk --mirror https://xxx
"""
import argparse
import binascii
import io
import json
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPS_ROOT = os.path.join(ROOT, "apps", "lua", "apps")   # 打包后的 LAPP 应用
OUT_ROOT = os.path.join(ROOT, "store")

# 默认镜像列表（顺序 = 设备尝试顺序）。主源是 linit 自有站，其余为备源。
DEFAULT_MIRRORS = [
    "https://bbk.linit.cn/bbk",                                  # 主源：自有站（阿里云国内节点，已备案）
    "https://bbk.linit.cn/bbk-mirror",                           # 同域第二目录（可指到别的后端）
    "https://gitee.com/LinIT-L/ESP32-S3-RLCD-BBK/raw/master/store",  # 备源：Gitee raw
]

HDR = re.compile(rb"--@app\s+([a-z_]+)=([^\r\n]*)")


# 展示名覆盖: 应用 LAPP 头里的标签太简略时, 商店里显示更清楚的名称
LABEL_FIX = {
    "bikecomp": "自行车码表",
}

# 分类覆盖: 所有 Lua 源都在 apps/lua/apps/ 下, 自动推算一律得到 "app" ⇒ 必须逐个指定
# game=游戏(装载到 engine 目录) / ops=运维 / app=软件
CAT_FIX = {
    "minidayz": "game", "qqtang": "game", "wuxia": "game",
    "earthsurvival": "game", "biossim": "ops",
    "planeradar": "ops", "map": "ops",
    "bikecomp": "app",
}

def lapp_meta(path):
    """从 LAPP 头里取 name/desc/cat（tools/pack_app.py 打进去的）。"""
    head = open(path, "rb").read(2048)
    d = {}
    for k, v in HDR.findall(head):
        d[k.decode()] = v.decode("utf-8", "replace").strip()
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mirror", action="append", default=None,
                    help="镜像基地址（可多次；不传则用内置默认）")
    ap.add_argument("--out", default=OUT_ROOT)
    ap.add_argument("--site", default="https://bbk.linit.cn/bbk",
                    help="写进 json 的主源自述地址")
    args = ap.parse_args()

    mirrors = args.mirror or DEFAULT_MIRRORS

    if not os.path.isdir(APPS_ROOT):
        sys.exit("✗ 找不到 %s —— 先跑 tools/sync_apps.py 生成打包好的应用" % APPS_ROOT)

    pkgs_dir = os.path.join(args.out, "pkgs")
    if os.path.isdir(pkgs_dir):
        shutil.rmtree(pkgs_dir)
    os.makedirs(pkgs_dir, exist_ok=True)

    pkgs = []
    for cat in sorted(os.listdir(APPS_ROOT)):          # app / ops
        cdir = os.path.join(APPS_ROOT, cat)
        if not os.path.isdir(cdir):
            continue
        for fn in sorted(os.listdir(cdir)):
            if not fn.endswith(".lua"):
                continue
            src = os.path.join(cdir, fn)
            name = fn[:-4]
            data = open(src, "rb").read()
            if len(data) < 32:
                print("  ⚠ 跳过空文件: %s" % src)
                continue
            shutil.copy2(src, os.path.join(pkgs_dir, fn))
            meta = lapp_meta(src)
            pkgs.append({
                "name": name,
                "label": LABEL_FIX.get(name) or meta.get("label") or meta.get("name") or name,
                "cat": CAT_FIX.get(name, cat),
                "ver": int(meta.get("ver") or 1),
                "size": len(data),
                "crc32": "%08x" % (binascii.crc32(data) & 0xFFFFFFFF),   # ★ 必须是【十六进制字符串】: 设备端 am_store_json.c 用 j_str+strtoul(hex) 解析, 数字会导致整条被丢弃
                "path": "pkgs/%s" % fn,
            })
            print("  + %-14s %-10s %6d B  crc=%s" % (name, pkgs[-1]["label"],
                                                     pkgs[-1]["size"], pkgs[-1]["crc32"]))

    index = {
        "ver": 1,
        "site": args.site,
        "updated": __import__("time").strftime("%Y-%m-%d %H:%M:%S"),
        "mirrors": mirrors,
        "pkgs": pkgs,
    }
    out_json = os.path.join(args.out, "index.json")
    with io.open(out_json, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=2)
        f.write("\n")

    total = sum(p["size"] for p in pkgs)
    print()
    print("=" * 60)
    print(" 产出 %s" % args.out)
    print("   index.json   %d 个包, 合计 %.1f KB" % (len(pkgs), total / 1024.0))
    print("   pkgs/        %d 个 .lua" % len(pkgs))
    print("   镜像列表(设备按序尝试):")
    for i, m in enumerate(mirrors, 1):
        print("     %d. %s" % (i, m))
    print("=" * 60)
    print(" 设备侧拼 URL: mirrors[i] + \"/\" + pkg.path")
    print(" 例: %s/pkgs/%s" % (mirrors[0], pkgs[0]["name"] + ".lua" if pkgs else "x.lua"))


if __name__ == "__main__":
    main()
