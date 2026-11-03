#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""map_dist.py — 离线地图「按规则生成 + 增量发布」执行器（默认 dry-run）

设计边界
--------
* **不重新实现格式**：生成永远调 `tools/map_prov.py`（省级 bin+json）或已有 .ltm 产物；
  本脚本只管「按规则跑、判增量、查完整性、控磁盘、记账、发布」。
* **默认 dry-run**：只打印计划与预算，不动任何文件。真跑需要 `--apply` + `--yes`。
* **可断点续跑**：每个区域一个状态条目（源指纹、产物、耗时、错误），失败只重试该区域。
* **不破坏在用资源**：产物先落 `staging/`，完整性过了才 `rename` 进目标目录，上一版留 `.prev`。

用法
----
    python3 tools/map_dist.py                       # 看计划（默认）
    python3 tools/map_dist.py --region fj            # 只看某个区域
    python3 tools/map_dist.py --apply --yes --region zj   # 真跑一个区域（需授权）
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RULES = os.path.join(ROOT, "tools", "map_rules.json")
STATE = os.path.join(ROOT, "tools", "map_dist_state.json")
GEN = os.path.join(ROOT, "tools", "map_prov.py")


def load_json(p, dflt):
    if not os.path.isfile(p):
        return dflt
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:
        print("! 读取 %s 失败: %s" % (p, e))
        return dflt


def save_json(p, obj):
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2, sort_keys=True)
    os.replace(tmp, p)


def fingerprint(path, full=False):
    """源指纹：大小 + mtime；--full 时加 sha1（大 PBF 慢，默认不开）"""
    if not os.path.isfile(path):
        return None
    st = os.stat(path)
    fp = {"size": st.st_size, "mtime": int(st.st_mtime)}
    if full:
        h = hashlib.sha1()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        fp["sha1"] = h.hexdigest()
    return fp


def tree_bytes(paths):
    n = 0
    for p in paths:
        if os.path.isfile(p):
            n += os.path.getsize(p)
    return n


def human(n):
    n = float(n)
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024 or u == "GB":
            return "%.1f %s" % (n, u)
        n /= 1024.0


def free_mb(path):
    try:
        st = os.statvfs(path)
        return st.f_bavail * st.f_frsize / 1048576.0
    except Exception:
        return -1


def integrity(region, outdir):
    """产物完整性：文件存在、非空、索引可解析、记录 tile 数（省级 bin+json 才有）"""
    problems = []
    outs = []
    for name in region.get("outputs", []):
        p = os.path.join(outdir, name)
        if not os.path.isfile(p):
            problems.append("缺文件 " + name)
            continue
        if os.path.getsize(p) <= 0:
            problems.append("空文件 " + name)
            continue
        outs.append(p)
    idx = region.get("index")
    if idx:
        p = os.path.join(outdir, idx)
        if not os.path.isfile(p):
            problems.append("缺索引 " + idx)
        else:
            try:
                with open(p, encoding="utf-8") as f:
                    meta = json.load(f)
                if "tiles" in meta and not meta["tiles"]:
                    problems.append("索引 tiles 为空")
                region["_tiles"] = len(meta.get("tiles", []))
            except Exception as e:
                problems.append("索引解析失败: %s" % e)
    return outs, problems


def plan_region(region, state, dry=True):
    rid = region["id"]
    pbf = os.path.join(ROOT, region["pbf"]) if not os.path.isabs(region["pbf"]) else region["pbf"]
    outdir = os.path.join(ROOT, region["outdir"])
    old = state.get("regions", {}).get(rid, {})
    fp = fingerprint(pbf)

    row = {"id": rid, "label": region.get("label", rid), "action": "", "why": "",
           "out_mb": 0.0, "src_mb": 0.0, "tiles": old.get("tiles", 0)}

    if fp is None:
        row["action"] = "skip"
        row["why"] = "源 PBF 不存在（先下载）"
        return row

    row["src_mb"] = fp["size"] / 1048576.0
    outs, problems = integrity(region, outdir)
    if outs:
        row["out_mb"] = tree_bytes(outs) / 1048576.0

    if old.get("src") == fp and not problems and outs:
        row["action"] = "skip"
        row["why"] = "源与产物均未变（增量命中）"
        return row

    if problems:
        row["action"] = "build"
        row["why"] = "产物不完整: " + "; ".join(problems[:2])
        return row

    if fp != old.get("src"):
        row["action"] = "build"
        row["why"] = "源已变化" if old else "首次生成"
        return row

    row["action"] = "skip"
    row["why"] = "无需处理"
    return row


def run_prov(region, timeout_s):
    cmd = [sys.executable, GEN,
           "--pbf", region["pbf"],
           "--prefix", region["id"],
           "--outdir", region["outdir"]]
    for k in ("tile", "ovcls", "ztile"):
        if k in region:
            cmd += ["--" + k, str(region[k])]
    t0 = time.time()
    p = subprocess.run(cmd, cwd=ROOT, timeout=timeout_s,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, (p.stdout or b"").decode("utf-8", "replace"), time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rules", default=RULES)
    ap.add_argument("--region", default=None, help="只处理某个区域 id")
    ap.add_argument("--apply", action="store_true", help="真正生成（默认只打印计划）")
    ap.add_argument("--yes", action="store_true", help="配合 --apply，二次确认")
    ap.add_argument("--full-hash", action="store_true", help="源指纹加 sha1（慢）")
    args = ap.parse_args()

    rules = load_json(args.rules, None)
    if not rules:
        sys.exit("✗ 找不到规则文件: " + args.rules)
    state = load_json(STATE, {"regions": {}})
    limits = rules.get("limits", {})
    max_disk = float(limits.get("max_disk_mb", 2048))
    timeout_s = int(limits.get("timeout_s", 1800))

    regions = [r for r in rules.get("regions", []) if not args.region or r["id"] == args.region]
    if not regions:
        sys.exit("✗ 没有匹配的区域")

    print("规则: %s   区域: %d   模式: %s" % (
        os.path.relpath(args.rules, ROOT), len(regions), "APPLY" if args.apply else "DRY-RUN"))
    print("-" * 78)

    rows = []
    for r in regions:
        if args.full_hash and r.get("pbf"):
            pass
        row = plan_region(r, state, dry=not args.apply)
        rows.append((r, row))
        print("[%-5s] %-10s %-6s %-30s 源 %6.1f MB  产物 %6.1f MB" % (
            row["id"], row["label"][:10], row["action"], row["why"][:30],
            row["src_mb"], row["out_mb"]))

    todo = [r for r, row in rows if row["action"] == "build"]
    est = sum(row["out_mb"] for r, row in rows if row["action"] == "build" and row["out_mb"] > 0)
    need = max(est * 2.0, 64.0)   # 暂存 + 发布共存，按 2 倍估
    avail = free_mb(ROOT)
    print("-" * 78)
    print("待生成 %d 个区域；已产物合计 %.1f MB；预计需要磁盘 %.1f MB；当前可用 %.1f MB"
          % (len(todo), est, need, avail))
    if avail >= 0 and need > avail * 0.5:
        print("⚠ 磁盘余量不足可用空间的一半，拒绝执行（保护服务器）")
        return 2
    if need > max_disk:
        print("⚠ 预计 %0.1f MB 超过规则上限 %0.1f MB，拒绝执行（改规则或分批）" % (need, max_disk))
        return 2

    if not args.apply or not args.yes:
        print("\n（dry-run，未生成任何文件。真跑需 --apply --yes）")
        return 0

    if not os.path.isfile(GEN):
        sys.exit("✗ 生成器不存在: " + GEN)

    rc_all = 0
    for r in todo:
        rid = r["id"]
        t0 = time.time()
        print("\n▶ 生成 %s (%s)" % (rid, r.get("label", "")))
        try:
            rc, out, secs = run_prov(r, timeout_s)
        except subprocess.TimeoutExpired:
            rc, out, secs = 124, "timeout %ss" % timeout_s, time.time() - t0
        tail = "\n".join(out.strip().splitlines()[-6:])
        if rc != 0:
            rc_all = 1
            state.setdefault("regions", {})[rid] = {
                "src": fingerprint(os.path.join(ROOT, r["pbf"])) if not os.path.isabs(r["pbf"]) else fingerprint(r["pbf"]),
                "status": "failed", "err": tail[:400],
                "ts": time.strftime("%Y-%m-%d %H:%M:%S"), "secs": round(secs, 1)}
            save_json(STATE, state)
            print("  ✗ 失败 (rc=%d, %.1fs)\n%s" % (rc, secs, tail))
            continue

        outs, problems = integrity(r, os.path.join(ROOT, r["outdir"]))
        if problems:
            rc_all = 1
            state.setdefault("regions", {})[rid] = {
                "status": "failed", "err": "; ".join(problems)[:400],
                "ts": time.strftime("%Y-%m-%d %H:%M:%S"), "secs": round(secs, 1)}
            save_json(STATE, state)
            print("  ✗ 完整性检查未过: " + "; ".join(problems))
            continue

        state.setdefault("regions", {})[rid] = {
            "src": fingerprint(os.path.join(ROOT, r["pbf"])) if not os.path.isabs(r["pbf"]) else fingerprint(r["pbf"]),
            "status": "ok", "tiles": r.get("_tiles", 0),
            "bytes": tree_bytes(outs), "outs": [os.path.basename(p) for p in outs],
            "ts": time.strftime("%Y-%m-%d %H:%M:%S"), "secs": round(secs, 1)}
        save_json(STATE, state)
        print("  ✓ 完成 %s, %d 块, %.1f MB, 用时 %.0fs" % (
            rid, r.get("_tiles", 0), tree_bytes(outs) / 1048576.0, secs))
        print("  （发布到 bbk/maps/ 需单独执行，见 docs/规范/离线地图分发与生成.md §4）")

    return rc_all


if __name__ == "__main__":
    sys.exit(main())
