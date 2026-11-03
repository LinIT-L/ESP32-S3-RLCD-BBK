#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""store_audit.py — 应用商店全量资源盘点 / 三方对账（只读）。

对每一个可下载项目同时比较 **四份来源**：
  ① 仓库源码     apps/lua/apps/{app,ops}/<file>.lua  （LAPP 源，带 --@app build=）
  ② 仓库发布副本  store/pkgs/<file>                   （store_deploy 上传的东西）
  ③ 线上索引     <site>/index.json                    （设备实际读的元数据）
  ④ 线上实际文件  <site>/<path>                       （设备实际下到的字节）

判定只用 **内容哈希（crc32）**，不看文件名/日期：
  · 索引 vs 线上文件不一致 → 设备校验必失败（rc=4），最严重
  · 仓库副本 vs 线上文件不一致 → 发布过但没重新部署，或反之
  · 源码 vs 仓库副本不一致 → 源码改了但没重新打包（商店发的是旧版）

用法:
  python3 tools/store_audit.py                    # 全量（含 pal.zip 31MB）
  python3 tools/store_audit.py --skip-large       # 跳过 >8MB 的包（快）
  python3 tools/store_audit.py --json out.json    # 附带机器可读结果
退出码: 0=全部一致  1=有差异  2=取不到索引
"""
import argparse
import binascii
import json
import os
import re
import sys
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPS_ROOT = os.path.join(ROOT, "apps", "lua", "apps")
STORE_ROOT = os.path.join(ROOT, "store")
HDR = re.compile(rb"--@app\s+([a-z_]+)=([^\r\n]*)")
LARGE = 8 * 1024 * 1024


def crc32(b):
    return "%08x" % (binascii.crc32(b) & 0xFFFFFFFF)


def lapp_meta(path):
    d = {}
    with open(path, "rb") as f:
        head = f.read(2048)
    for k, v in HDR.findall(head):
        d[k.decode()] = v.decode("utf-8", "replace").strip()
    return d


def scan_sources():
    """{显示名: (path, size, crc, build, cat)} —— 只收带 --@lapp 头的文件"""
    out = {}
    for cat in ("app", "ops", "engine"):
        cdir = os.path.join(APPS_ROOT, cat)
        if not os.path.isdir(cdir):
            continue
        for fn in sorted(os.listdir(cdir)):
            if not fn.endswith(".lua"):
                continue
            p = os.path.join(cdir, fn)
            meta = lapp_meta(p)
            if not meta.get("name"):
                continue                      # _ui.lua 这类公共库不是应用
            data = open(p, "rb").read()
            out[meta["name"]] = {
                "file": fn, "path": p, "size": len(data), "crc32": crc32(data),
                "build": meta.get("build", ""), "cat": meta.get("cat", ""),
            }
    return out


def fetch(url, timeout=90):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--site", default="https://bbk.linit.cn/bbk")
    ap.add_argument("--skip-large", action="store_true")
    ap.add_argument("--json", default="")
    args = ap.parse_args()
    site = args.site.rstrip("/")

    try:
        idx = json.loads(fetch(site + "/index.json", 25).decode("utf-8"))
    except Exception as e:
        print("✗ 取不到索引 %s/index.json: %s" % (site, e))
        return 2

    src = scan_sources()
    local_pkgs = os.path.join(STORE_ROOT, "pkgs")
    rows, bad = [], 0

    print("=" * 118)
    print("线上索引 ver=%s updated=%s  共 %d 项   源码目录 %d 个应用"
          % (idx.get("ver"), idx.get("updated"), len(idx.get("pkgs", [])), len(src)))
    print("=" * 118)
    print("%-14s %-8s %10s %9s  %-10s %-10s %-10s %s"
          % ("项目", "类别", "字节", "build", "索引crc", "线上crc", "仓库crc", "结论"))
    print("-" * 118)

    for p in idx.get("pkgs", []):
        name = p["name"]
        is_asset = (p.get("type") == "asset")
        fn = os.path.basename(p["path"])
        # ⚠ 路径里有中文/空格时 urllib 不会自动编码 → 直接拼会 400/404。
        #   （和固件端 am_url_encode 是同一个坑，这里必须一致地百分号编码）
        url = site + "/" + urllib.parse.quote(p["path"])

        # ④ 线上实际字节
        live = None
        if args.skip_large and p.get("size", 0) > LARGE:
            live = ("SKIP", 0)
        else:
            try:
                b = fetch(url)
                live = (crc32(b), len(b))
            except Exception as e:
                live = ("ERR", str(e)[:40])

        # ② 仓库发布副本（asset 包一般不在仓库里）
        lc = "-"
        lp = os.path.join(local_pkgs, fn)
        if os.path.exists(lp):
            lc = crc32(open(lp, "rb").read())

        # ① 源码（只有 Lua 应用有）
        sc = "-"
        s = src.get(name)
        if s:
            sc = s["crc32"]

        idx_crc, idx_size = p.get("crc32", ""), p.get("size", 0)
        problems, notes = [], []
        if live[0] not in ("SKIP", ) and (live[0] != idx_crc or live[1] != idx_size):
            problems.append("索引≠线上")
        if live[0] == "ERR":
            problems.append("取不到")
        if lc != "-" and lc != idx_crc:
            problems.append("仓库≠索引")
        # 源码差异单独提示: 有的包发布前会被工具注入数据（如矢量地图 100KB→175KB），
        # 源码与成品本来就不相等，不能一律当"过期"。只有"仓库副本也≠索引"才是真问题。
        if sc != "-" and sc != idx_crc and not problems:
            notes.append("源码≠发布")
        verdict = "OK" if not problems else " / ".join(problems)
        if notes:
            verdict += "  (%s)" % ",".join(notes)
        if problems:
            bad += 1

        rows.append({
            "name": name, "type": p.get("type", "lua"), "size": idx_size,
            "build": p.get("build", ""), "path": p["path"],
            "index_crc": idx_crc, "live_crc": live[0], "repo_crc": lc, "src_crc": sc,
            "verdict": verdict,
        })
        print("%-14s %-8s %10d %9s  %-10s %-10s %-10s %s"
              % (name, p.get("type", "lua"), idx_size, p.get("build", "") or "-",
                 idx_crc, live[0], lc, verdict))

    # 仅本地有、线上没有的（下架/未发布）
    online_names = {p["name"] for p in idx.get("pkgs", [])}
    only_local = [n for n in src if n not in online_names]
    if only_local:
        print("-" * 118)
        print("仅仓库有、线上索引没有（未发布或已下架）:")
        for n in only_local:
            print("   %-14s %s  %d B crc=%s build=%s"
                  % (n, src[n]["file"], src[n]["size"], src[n]["crc32"], src[n]["build"]))

    print("=" * 118)
    print("结论: %d/%d 项一致%s" % (len(rows) - bad, len(rows),
                                 "" if not bad else "  ⚠ %d 项需处理" % bad))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"index": idx, "rows": rows, "only_local": only_local},
                      f, ensure_ascii=False, indent=2)
        print("明细已写入 %s" % args.json)
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
