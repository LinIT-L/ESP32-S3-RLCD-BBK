#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""version_bump.py —— 小步改版本号（内部 细步 / 对外 只在推送时动）。

用法:
    # 看要改什么（默认干跑，不写盘）
    python3 tools/version_bump.py --patch --name "蓝牙手柄档案表"
    python3 tools/version_bump.py --minor --name "频谱仪重构"
    python3 tools/version_bump.py --public 1.4          # 只改对外号（推送前）

    # 真改
    python3 tools/version_bump.py --patch --name "xxx" --apply

改完后它会把「发版 4 步」打出来（构建 → version_check → 提交 → 打 tag）。

设计要点:
  * **对外版本**（`LINTOS_VERSION_PUBLIC`，主.次）只在"推给外面"时改 → 用 `--public X.Y`
  * **内部版本**（主.次.修订）小步走 → `--patch`（默认）/ `--minor`（次+1 且修订归 0）
  * 内部更细的粒度**不用手改**：显示时的 `+N` 由 git 自动算（距最近 tag 的提交数）
  * 版本主题若含中文，会**转成 \\xNN 转义**写进头文件（该头文件全篇用转义，保持一致）
"""
import argparse
import datetime
import io
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VER_H = os.path.join(ROOT, "components/system/os/include/lintos_version.h")
CHANGELOG = os.path.join(ROOT, "CHANGELOG.md")


def esc_utf8(s: str) -> str:
    """把非 ASCII 转成 \\xNN 转义（与该头文件既有风格一致）。"""
    out = []
    for ch in s:
        if ord(ch) < 128:
            out.append(ch)
        else:
            out.extend("\\x%02x" % b for b in ch.encode("utf-8"))
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--patch", action="store_true", help="内部 修订+1（默认）")
    g.add_argument("--minor", action="store_true", help="内部 次+1、修订归 0")
    ap.add_argument("--public", metavar="X.Y", help="改对外版本（推送前用）")
    ap.add_argument("--name", metavar="主题", help="本次迭代主题（中文会自动转义）")
    ap.add_argument("--date", metavar="YYYY-MM-DD", help="迭代日期（默认今天）")
    ap.add_argument("--apply", action="store_true", help="真正写盘（默认只干跑）")
    a = ap.parse_args()

    src = io.open(VER_H, encoding="utf-8").read()

    def get(pats):
        for p in pats:
            m = re.search(p, src)
            if m:
                return m.group(1)
        return None

    major = int(get([r"#define\s+LINTOS_VERSION_MAJOR\s+(\d+)"]))
    minor = int(get([r"#define\s+LINTOS_VERSION_MINOR\s+(\d+)"]))
    patch = int(get([r"#define\s+LINTOS_VERSION_PATCH\s+(\d+)"]))
    public = get([r'#define\s+LINTOS_VERSION_PUBLIC\s+"([^"]*)"'])
    date = a.date or datetime.date.today().isoformat()

    new_major, new_minor, new_patch = major, minor, patch
    if a.minor:
        new_minor, new_patch = minor + 1, 0
    elif a.patch or not a.public:
        new_patch = patch + 1
    new_str = "%d.%d.%d" % (new_major, new_minor, new_patch)
    old_str = "%d.%d.%d" % (major, minor, patch)
    new_public = a.public or public

    print("内部版本 : %s  →  %s" % (old_str, new_str))
    print("对外版本 : %s  →  %s%s" % (public, new_public, "" if a.public else "  (未动)"))
    print("迭代日期 : %s" % date)
    if a.name:
        print("迭代主题 : %s" % a.name)
    if not a.apply:
        print("\n(干跑，未写盘；确认后加 --apply)")
        return 0

    out = src
    out = re.sub(r"(#define\s+LINTOS_VERSION_MAJOR\s+)\d+", r"\g<1>%d" % new_major, out)
    out = re.sub(r"(#define\s+LINTOS_VERSION_MINOR\s+)\d+", r"\g<1>%d" % new_minor, out)
    out = re.sub(r"(#define\s+LINTOS_VERSION_PATCH\s+)\d+", r"\g<1>%d" % new_patch, out)
    out = re.sub(r'(#define\s+LINTOS_VERSION_STR\s+)"[^"]*"', r'\g<1>"%s"' % new_str, out)
    out = re.sub(r'(#define\s+LINTOS_VERSION_DATE\s+)"[^"]*"', r'\g<1>"%s"' % date, out)
    out = re.sub(r'(#define\s+LINTOS_VERSION_PUBLIC\s+)"[^"]*"', r'\g<1>"%s"' % new_public, out)
    if a.name:
        out = re.sub(r'(#define\s+LINTOS_VERSION_NAME\s+)"[^"]*"',
                     lambda m: '%s"%s"' % (m.group(1), esc_utf8(a.name)), out)

    shutil.copy2(VER_H, VER_H + ".bak_bump_" + datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    io.open(VER_H, "w", encoding="utf-8").write(out)
    print("[写入] %s" % os.path.relpath(VER_H, ROOT))

    # CHANGELOG 插一节（紧跟在头部说明之后）
    ch = io.open(CHANGELOG, encoding="utf-8").read()
    anchor = "> 固件版本的变更说明见下，完整功能介绍见 [README](README.md)。\n"
    if anchor in ch and ("## v%s" % new_str) not in ch:
        stub = ("\n## v%s (%s) — 内部迭代：%s\n\n> 占位：请在提交前把本节的改动条目写全"
                "（对外版本当前 %s；推送时才改对外号）。\n" % (new_str, date, a.name or "（待填）", new_public))
        ch = ch.replace(anchor, anchor + stub, 1)
        io.open(CHANGELOG, "w", encoding="utf-8").write(ch)
        print("[写入] CHANGELOG.md 已插入 `## v%s` 小节（待补条目）" % new_str)
    elif ("## v%s" % new_str) in ch:
        print("[跳过] CHANGELOG 里已有 `## v%s`" % new_str)

    print("""
下一步（发版 4 步）:
  1) 补全 CHANGELOG.md 里 `## v%s` 的改动条目
  2) 构建:   <sanitized env> idf.py build        # 确认 0 error
             确认: python3 tools/version_check.py
  3) 提交:   git add -A && git commit -m "feat(v%s): ..."
  4) 打 tag: git tag -a v%s -m "LinTOS 内部迭代 v%s — ..."
  提示: 设备上还会显示自动的迭代号 `+N`（距 v%s 的提交数），不必手改。
""" % (new_str, new_str, new_str, new_str, new_str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
