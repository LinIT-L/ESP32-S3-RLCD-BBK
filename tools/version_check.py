#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""version_check.py —— 校验"版本号单一来源"与 CHANGELOG / git tag 三者一致。

规则（2026-10-02 定，见 components/system/os/include/lintos_version.h）：
  ① `LINTOS_VERSION_STR` 必须等于 CHANGELOG.md 里**最上面**那条 `## vX.Y.Z` 的版本
  ② `LINTOS_VERSION_DATE` 建议等于该标题括号里的日期
  ③ 若当前 HEAD 上有 vX.Y.Z 形式的 tag，则应与 `LINTOS_VERSION_STR` 一致

用法:
    ~/.espressif/python_env/idf5.5_py3.13_env/bin/python3 tools/version_check.py     # 或任意 python3
退出码: 0=一致, 1=不一致, 2=环境问题(读不到文件/无 git 不致命)
可作为提交前自检；未接入构建（避免因忘改文档而挡住编译）。
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
    return {
        "str": g("LINTOS_VERSION_STR"),
        "date": g("LINTOS_VERSION_DATE"),
        "major": re.search(r"#define\s+LINTOS_VERSION_MAJOR\s+(\d+)", t),
        "minor": re.search(r"#define\s+LINTOS_VERSION_MINOR\s+(\d+)", t),
        "patch": re.search(r"#define\s+LINTOS_VERSION_PATCH\s+(\d+)", t),
    }


def read_changelog_top():
    t = open(CHANGELOG, encoding="utf-8").read()
    m = re.search(r"^##\s+v([0-9]+\.[0-9]+\.[0-9]+)\s*(?:\(([^)]*)\))?", t, re.M)
    return (m.group(1), m.group(2)) if m else (None, None)


def git_top_tag():
    try:
        out = subprocess.run([GIT, "tag", "--points-at", "HEAD"],
                             cwd=ROOT, capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return None
    for line in out.split():
        if re.fullmatch(r"v\d+\.\d+\.\d+", line):
            return line[1:]
    return None


def main():
    vh = read_version_h()
    ch_ver, ch_date = read_changelog_top()
    tag = git_top_tag()
    ok = True

    print("版本号来源 : %s" % os.path.relpath(VER_H, ROOT))
    print("  版本字符串: %s" % vh["str"])
    print("  版本日期  : %s" % vh["date"])
    print("CHANGELOG  : v%s%s" % (ch_ver, (" (%s)" % ch_date) if ch_date else ""))
    print("HEAD 上的 tag: %s" % (("v" + tag) if tag else "(无)"))

    if vh["str"] is None:
        print("✗ lintos_version.h 里读不到 LINTOS_VERSION_STR"); return 2
    if ch_ver is None:
        print("✗ CHANGELOG.md 里找不到 `## vX.Y.Z` 标题"); return 2

    if vh["str"] != ch_ver:
        print("✗ 不一致: 头文件 %s ≠ CHANGELOG v%s" % (vh["str"], ch_ver)); ok = False
    if ch_date and vh["date"] and ch_date.strip() != vh["date"]:
        print("⚠ 日期不一致(仅提醒): 头文件 %s ≠ CHANGELOG %s" % (vh["date"], ch_date.strip()))
    if tag and tag != vh["str"]:
        print("✗ 不一致: HEAD tag v%s ≠ 头文件 %s" % (tag, vh["str"])); ok = False

    # 三段号拼接自检
    nums = []
    for k in ("major", "minor", "patch"):
        nums.append(vh[k].group(1) if vh[k] else "?")
    joined = ".".join(nums)
    if joined != vh["str"]:
        print("✗ LINTOS_VERSION_STR(%s) 与 MAJOR/MINOR/PATCH(%s) 拼不上" % (vh["str"], joined)); ok = False

    print("=> %s" % ("✅ 版本号一致" if ok else "❌ 存在不一致, 请修正"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
