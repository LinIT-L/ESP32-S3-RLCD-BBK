#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""version_check.py —— 校验版本号三处一致（两套号：对外 粗 / 内部 细）。

规则（2026-10-02 定，见 components/system/os/include/lintos_version.h）：
  ① `LINTOS_VERSION_STR`（内部三段）必须等于 CHANGELOG.md **最上面**那条
     `## vX.Y.Z` 的版本
  ② 若 HEAD 上有 `vX.Y.Z` 形式的 tag，应与 `LINTOS_VERSION_STR` 一致
  ③ `LINTOS_VERSION_PUBLIC`（对外，主.次）单独报告；并与 CHANGELOG 里最近一条
     两段式 `## vX.Y` 大节做**提醒性**比对（不一致只提醒，不算失败）
  ④ 三段号与 MAJOR/MINOR/PATCH 宏拼接必须自洽

用法:
    python3 tools/version_check.py        # 任意 python3 均可（无第三方依赖）
退出码: 0=一致, 1=不一致, 2=环境问题(读不到文件)
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VER_H = os.path.join(ROOT, "components/system/os/include/lintos_version.h")
CHANGELOG = os.path.join(ROOT, "CHANGELOG.md")
GIT = "/opt/homebrew/bin/git" if os.path.exists("/opt/homebrew/bin/git") else "git"


def read_version_h():
    t = open(VER_H, encoding="utf-8").read()
    def g(name):
        m = re.search(r'#define\s+%s\s+"([^"]*)"' % name, t)
        return m.group(1) if m else None
    def n(name):
        m = re.search(r"#define\s+%s\s+(\d+)" % name, t)
        return m.group(1) if m else None
    return {
        "public": g("LINTOS_VERSION_PUBLIC"),
        "str": g("LINTOS_VERSION_STR"),
        "date": g("LINTOS_VERSION_DATE"),
        "major": n("LINTOS_VERSION_MAJOR"),
        "minor": n("LINTOS_VERSION_MINOR"),
        "patch": n("LINTOS_VERSION_PATCH"),
    }


def read_changelog():
    t = open(CHANGELOG, encoding="utf-8").read()
    m3 = re.search(r"^##\s+v(\d+\.\d+\.\d+)\s*(?:\(([^)]*)\))?", t, re.M)
    m2 = re.search(r"^##\s+v(\d+\.\d+)\b\s*(?:[—\-].*)?$", t, re.M)
    return {
        "top3": (m3.group(1), m3.group(2)) if m3 else (None, None),
        "top2": m2.group(1) if m2 else None,
    }


def git_top_tag():
    try:
        out = subprocess.run([GIT, "tag", "--points-at", "HEAD"],
                             cwd=ROOT, capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return None
    for line in out.split():
        m = re.fullmatch(r"v(\d+\.\d+\.\d+)", line)
        if m:
            return m.group(1)
    return None


def main():
    vh = read_version_h()
    ch = read_changelog()
    tag = git_top_tag()
    ok = True

    print("版本号唯一来源 : %s" % os.path.relpath(VER_H, ROOT))
    print("  对外版本     : %s        (推送用, 只在推给外面时改)" % vh["public"])
    print("  内部版本     : %s        (自测用, 小步走)" % vh["str"])
    print("  内部日期     : %s" % vh["date"])
    print("CHANGELOG 内部 : v%s%s" % (ch["top3"][0], (" (%s)" % ch["top3"][1]) if ch["top3"][1] else ""))
    print("CHANGELOG 对外 : %s" % (("v" + ch["top2"]) if ch["top2"] else "(无两段式大节)"))
    print("HEAD 上的 tag  : %s" % (("v" + tag) if tag else "(无)"))

    if vh["str"] is None:
        print("✗ 读不到 LINTOS_VERSION_STR"); return 2
    if ch["top3"][0] is None:
        print("✗ CHANGELOG 里找不到 `## vX.Y.Z` 标题"); return 2

    # ③ 自洽
    joined = ".".join([vh["major"] or "?", vh["minor"] or "?", vh["patch"] or "?"])
    if joined != vh["str"]:
        print("✗ LINTOS_VERSION_STR(%s) 与 MAJOR/MINOR/PATCH(%s) 拼不上" % (vh["str"], joined)); ok = False

    # ① 内部号 ↔ CHANGELOG 顶部
    if vh["str"] != ch["top3"][0]:
        print("✗ 不一致: 头文件内部 %s ≠ CHANGELOG v%s" % (vh["str"], ch["top3"][0])); ok = False
    if ch["top3"][1] and vh["date"] and ch["top3"][1].strip() != vh["date"]:
        print("⚠ 日期不一致(仅提醒): 头文件 %s ≠ CHANGELOG %s" % (vh["date"], ch["top3"][1].strip()))

    # ② tag ↔ 内部号
    if tag and tag != vh["str"]:
        print("✗ 不一致: HEAD tag v%s ≠ 头文件内部 %s" % (tag, vh["str"])); ok = False

    # ④ 对外号 ↔ CHANGELOG 两段式大节（仅提醒）
    if vh["public"] and ch["top2"] and vh["public"] != ch["top2"]:
        print("⚠ 对外版本 %s 与 CHANGELOG 最近的两段式大节 v%s 不同(仅提醒, 不影响构建)"
              % (vh["public"], ch["top2"]))

    print("=> %s" % ("✅ 版本号一致" if ok else "❌ 存在不一致, 请修正"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
