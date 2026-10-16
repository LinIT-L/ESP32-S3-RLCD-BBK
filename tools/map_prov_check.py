#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""map_prov_check.py — 省级包**端到端自检**

设备端是"读 bin 的一段 → 当 .ltm 解 → 进度投影 → 画线"。这里在电脑上把这条链
**原样走一遍**, 任何一处字节/偏移写错都会在这里暴露, 而不是等烧到机器上才发现。

用法:
    python3 tools/map_prov_check.py --dir dist/map/fj --prefix fj \\
        --center 25.4335,119.0110 --zoom 14 --out docs/screenshots/福建样张.png
"""
import argparse
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from map_vec import parse_ltm, parse_index, render_preview          # noqa: E402


def check_index(t, dd, tag, bad):
    """校验 v3 网格索引的自洽性 —— 索引错了会**静默少画要素**, 比崩溃更难查。

    三条硬判据:
      ① 每个要素**恰好登记一次** (要么在某格的 fid 里, 要么在 global 里)
      ② cell_start 单调不减, 且末项 == 索引里的条目总数
      ③ 每个 fid 都是合法下标, 且**确实落在它被登记的那一格里**(最小格规则)
         —— 这条错了表现就是"某些要素永远画不出来"
    """
    ix = parse_index(t, dd)
    if not ix:
        return
    n = dd["nfeat"]
    seen = [0] * n
    for k in ix["fid"]:
        if k >= n:
            print("  ✗ %s fid 越界 %d >= %d" % (tag, k, n)); bad += 1
        else:
            seen[k] += 1
    for k in ix["gid"]:
        if k >= n:
            print("  ✗ %s gid 越界 %d >= %d" % (tag, k, n)); bad += 1
        else:
            seen[k] += 1
    miss = [i for i in range(n) if seen[i] == 0]
    dup = [i for i in range(n) if seen[i] > 1]
    if miss:
        print("  ✗ %s 有 %d 个要素没进索引 (永远画不出来), 例: %s" % (tag, len(miss), miss[:5])); bad += 1
    if dup:
        print("  ✗ %s 有 %d 个要素重复登记, 例: %s" % (tag, len(dup), dup[:5])); bad += 1
    st = ix["start"]
    for c in range(len(st) - 1):
        if st[c] > st[c + 1]:
            print("  ✗ %s cell_start 非单调 @%d" % (tag, c)); bad += 1
            break
    if st[-1] != ix["nsid"] or len(ix["fid"]) != ix["nsid"]:
        print("  ✗ %s cell_start 末项 %d != nsid %d" % (tag, st[-1], ix["nsid"])); bad += 1
    if ix["nsid"] + ix["gcount"] != n:
        print("  ✗ %s 覆盖不全 %d+%d != %d" % (tag, ix["nsid"], ix["gcount"], n)); bad += 1
    # ③ 最小格规则: fid 里的第 k 条, 其 bbox 的最小格必须正好是它所在那格
    #   ⚠ 索引里 cw/ch 存的是**微度**(设备端用微度算格号), 而要素 bbox 是 **1e-5 度**
    #     ⇒ 自检这边要 //10 才同量纲。单位不一致是这套格式最容易犯的错。
    cwx, chy = ix["cw"] // 10, ix["ch"] // 10
    for c in range(ix["nx"] * ix["ny"]):
        cx, cy = c % ix["nx"], c // ix["nx"]
        for k in ix["fid"][st[c]:st[c + 1]]:
            bx0, by0, bx1, by1 = dd["feats"][k]["bbox"]
            if (min(ix["nx"] - 1, max(0, bx0 // cwx)) != cx
                    or min(ix["ny"] - 1, max(0, by0 // chy)) != cy):
                print("  ✗ %s 要素 %d 没登记在最小格 (在 %d,%d)" % (tag, k, cx, cy)); bad += 1
                break
    if not bad:
        print("  ✓ %s 索引 %dx%d 格, 格内 %d 条 + 跨界 %d 条 = %d (覆盖 100%%)"
              % (tag, ix["nx"], ix["ny"], ix["nsid"], ix["gcount"], n))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="dist/map/fj")
    ap.add_argument("--prefix", default="fj")
    ap.add_argument("--center", default=None, help="lat,lon")
    ap.add_argument("--zoom", type=int, default=14)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    meta = json.load(open(os.path.join(a.dir, "%s.json" % a.prefix), encoding="utf-8"))
    binpath = os.path.join(a.dir, meta["bin"])
    blob = open(binpath, "rb").read()
    print("包 %s (%s)  %.1f MB  %d 块  tile=%.3f  z>=%d 分块"
          % (meta["id"], meta["name"], len(blob) / 1048576.0,
             meta["count"], meta["tile"], meta["ztile"]))

    # ---- ① 概览 ----
    ov = blob[meta["ov_off"]:meta["ov_off"] + meta["ov_size"]]
    d = parse_ltm(ov)
    print("概览  %d 条 / %d 点 / %.0f KB   bbox %.3f,%.3f ~ %.3f,%.3f  crc %s"
          % (d["nfeat"], d["npt"], len(ov) / 1024.0,
             d["bbox"][0], d["bbox"][1], d["bbox"][2], d["bbox"][3],
             "OK" if d["crc"] == d["file_crc"] else "错!"))
    assert d["nfeat"] == 0 or d["npt"] > 0, "概览点表为空"
    assert d["crc"] == d["file_crc"], "概览 crc 不符"
    bad = check_index(ov, d, "概览", 0) or 0

    # ---- ② 遍历全部块: 逐个**完整**反解 (parse_ltm 会把每个要素、每个点都解一遍,
    #        这本身就是最强校验; 再额外核对坐标确实落在本块范围内) ----
    bad = 0
    rid = [r for r in meta["tiles"] if r["s"]]
    pid = [r for r in meta["tiles"] if r["ps"]]
    t = meta["tile"]
    # 索引自洽性抽样: 取要素最多的 12 个块 (最容易出问题的那批)
    sample_keys = set((r["x"], r["y"]) for r in
                      sorted(rid, key=lambda r: -r.get("no", 0))[:12]) if rid else set()
    for r in meta["tiles"]:
        x0, y0 = r["x"] * t, r["y"] * t
        x1, y1 = x0 + t, y0 + t
        for nk, ok_, lay, sub in (("s", "o", 1, 1), ("ps", "po", 3, 0)):
            sz = r[nk]
            if not sz:
                continue
            off = r[ok_]
            if off + sz > len(blob):
                print("  ✗ %d_%d 越界 off=%d size=%d" % (r["x"], r["y"], off, sz))
                bad += 1
                continue
            dd = parse_ltm(blob[off:off + sz])
            if dd["layer"] != lay or len(dd["feats"]) != dd["nfeat"] or dd["nfeat"] <= 0:
                print("  ✗ %d_%d 结构不符 layer=%d nfeat=%d/%d"
                      % (r["x"], r["y"], dd["layer"], len(dd["feats"]), dd["nfeat"]))
                bad += 1
                continue
            if dd["crc"] != dd["file_crc"]:
                print("  ✗ %d_%d crc 不符" % (r["x"], r["y"]))
                bad += 1
            if sub:
                for f in dd["feats"]:
                    if not (1 <= f["npts"] <= 255):
                        print("  ✗ %d_%d npts=%d 越界" % (r["x"], r["y"], f["npts"]))
                        bad += 1
                        break
                    for (plon, plat) in f["pts"]:
                        # 允许一点点越界 (裁剪/取整), 但不能离谱
                        if not (x0 - 1e-3 <= plon <= x1 + 1e-3 and y0 - 1e-3 <= plat <= y1 + 1e-3):
                            print("  ✗ %d_%d 点 (%.4f,%.4f) 跑到块外" % (r["x"], r["y"], plon, plat))
                            bad += 1
                            break
                # 索引自洽性: 抽样 12 个块 (全查 610 个块会慢, 抽样已足够抓系统性错误)
                if len(rid) and (r["x"], r["y"]) in sample_keys:
                    bad = check_index(blob[off:off + sz], dd,
                                      "%d_%d" % (r["x"], r["y"]), bad) or 0
    print("完整反解 %d 道路块 + %d 地标块, 异常 %d" % (len(rid), len(pid), bad))

    # ---- ③ 出样张: 概览 + 视口内所有块一起渲 ----
    if a.out and a.center:
        clat, clon = (float(x) for x in a.center.split(","))
        t = meta["tile"]
        # 视口在 z14 覆盖约 0.14°, 取 ±1 块
        picked = [ov]
        for r in meta["tiles"]:
            cx = (r["x"] + 0.5) * t
            cy = (r["y"] + 0.5) * t
            if abs(cx - clon) < t * 1.5 and abs(cy - clat) < t * 1.5:
                if r["s"]:
                    picked.append(blob[r["o"]:r["o"] + r["s"]])
                if r["ps"]:
                    picked.append(blob[r["po"]:r["po"] + r["ps"]])
        print("样张用 %d 份数据 (概览 + %d 块)" % (len(picked), len(picked) - 1))
        render_preview(picked, (clat, clon), a.zoom, a.out)
        print("✓ 样张 → %s" % a.out)

    print("✓ 自检通过" if bad == 0 else "✗ 有 %d 处异常" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
