#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第二轮数据构建: 质量修复 + 种子合并 + 双文件同步。

做四件事:
  1. 修现有质量缺陷 — BOOT 的 en 里重复的码前缀; BSOD 的 en 尾部 "(0x…)";
  2. 合并 tools/fc_seed_round2.py 的新条目 (按命名空间归一化码去重, 已有则覆盖);
  3. 统一文本: 编号分条补空格、连续空格压一个、①②③ 转 1) 2) 3);
  4. 重写两份 lua 的 DB 段并保证逐字一致 (以 store 版为基准)。

用法:
    python3 tools/fc_round2.py --dry     # 只统计与抽样, 不写文件
    python3 tools/fc_round2.py           # 写入
"""
import os, re, sys, argparse, shutil
from collections import Counter, OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from fc_migrate_v2 import (load_rows, render, sort_key,
                           LAYER_ORDER, DB_HEAD, DB_TAIL)
import fc_seed_round2 as SEED

ROOT = os.path.dirname(HERE)
LUA_PATHS = [
    os.path.join(ROOT, "store/pkgs/故障信息库.lua"),
    os.path.join(ROOT, "apps/lua/apps/app/故障信息库.lua"),
]

def norm_key(s):
    """去重键 — 必须带命名空间, 否则 0x1CA(蓝屏) 与 01CA(固件自检) 会互相覆盖。"""
    t = s.strip().lower()
    if t.startswith("code"):
        return "dev:" + (t[4:].strip().lstrip("0") or "0")
    if t.startswith("0x"):
        u = t[2:]
        return "hex:" + ((u.lstrip("0") or "0") if re.fullmatch(r"[0-9a-f]+", u) else u)
    if re.fullmatch(r"[0-9a-f]+", t):
        return "raw:" + (t.lstrip("0") or "0")
    return "txt:" + t


CIRC = {"①": "1) ", "②": "2) ", "③": "3) ", "④": "4) ", "⑤": "5) "}
TEXT_FIELDS = (1, 2, 4, 5, 6, 7, 8, 11, 15)


def tidy(s):
    """编号分条补空格 + 压缩多余空格 + 圈号降级。"""
    for k, v in CIRC.items():
        s = s.replace(k, v)
    # "硬盘2)" → "硬盘 2)" —— 只对中文后紧贴的编号补空格;
    # 不能匹配任意非空白字符, 否则 (M.2) / SSD0) 会被改成 (M. 2) / SSD 0)
    s = re.sub(r"([\u4e00-\u9fff])(\d\))", r"\1 \2", s)
    s = re.sub(r"  +", " ", s)
    return s.strip()


def fix_quality(rows):
    """现库已知缺陷, 逐条修。返回 (rows, 修复计数)。"""
    n_boot = n_bsod = 0
    for r in rows:
        lay = r[13]
        if lay == "BOOT":
            # BOOT 的 en 常写成 "0xc0000001 - A required device …", 与 code 字段重复
            m = re.match(r"^\s*0x[0-9A-Fa-f]{6,8}\s*[-–—]\s*(.+)$", r[1])
            if m:
                r[1] = m.group(1).strip()
                n_boot += 1
            elif re.match(r"^\s*0x[0-9A-Fa-f]{6,8}\s*$", r[1]):
                r[1] = r[2]
                n_boot += 1
        if lay == "BSOD" and "0x" in r[1]:
            # BSOD 的 en 尾部 "(0x0000007B)" 冗余
            new = re.sub(r"\s*\((?:0x)?[0-9A-Fa-f]{2,8}\)\s*$", "", r[1]).strip()
            if new and new != r[1]:
                r[1] = new
                n_bsod += 1
    return rows, n_boot, n_bsod


def upsert(rows, seeds):
    """按归一化码覆盖/追加。返回 (新增数, 覆盖数)。"""
    idx = {norm_key(r[0]): i for i, r in enumerate(rows)}
    added = replaced = 0
    for s in seeds:
        row = [str(x) for x in s]
        k = norm_key(row[0])
        if k in idx:
            rows[idx[k]] = row
            replaced += 1
        else:
            idx[k] = len(rows)
            rows.append(row)
            added += 1
    return added, replaced


def build():
    rows = load_rows(LUA_PATHS[0])
    rows, nb, ns = fix_quality(rows)
    # SELF 层的 BitLocker 屏幕提示库里已有 (SUP 来源, 内容更全), 不重复收录
    seeds = list(SEED.BSOD) + list(SEED.BOOT) + list(SEED.SYS)
    added, replaced = upsert(rows, seeds)
    for r in rows:
        for i in TEXT_FIELDS:
            r[i] = tidy(r[i])
    # 分层分组 + 层内按码排序
    buckets = OrderedDict((k, []) for k in LAYER_ORDER)
    for r in rows:
        buckets.setdefault(r[13], []).append(r)
    out = []
    for k in LAYER_ORDER:
        out.extend(sorted(buckets.get(k, []), key=sort_key))
    return out, dict(boot=nb, bsod=ns, added=added, replaced=replaced)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    rows, st = build()
    print("总计 %d 条" % len(rows))
    print("分层:", dict(Counter(r[13] for r in rows)))
    print("处置:", dict(Counter(r[9] for r in rows)))
    print("来源:", dict(Counter(r[10] for r in rows)))
    print("修复: BOOT en 前缀 %d 条 / BSOD en 尾缀 %d 条; 种子新增 %d, 覆盖 %d"
          % (st["boot"], st["bsod"], st["added"], st["replaced"]))
    if a.dry:
        return
    block = render(rows)
    bad = [r[0] for r in rows if not re.fullmatch(r"C_[A-Z]+", r[3] or "")]
    if bad or ", None," in block:
        raise SystemExit("拒绝写入: cat 字段非法 %s" % bad[:5])
    src = open(LUA_PATHS[0], encoding="utf-8").read()
    i = src.index(DB_HEAD)
    j = src.index(DB_TAIL, i)
    new = src[:i] + block + src[j:]
    open(LUA_PATHS[0], "w", encoding="utf-8").write(new)
    shutil.copyfile(LUA_PATHS[0], LUA_PATHS[1])
    print("写入", LUA_PATHS[0], len(new), "字节; 已同步", LUA_PATHS[1])


if __name__ == "__main__":
    main()
