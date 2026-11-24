#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""doc_audit.py — LinTOS 文档只读体检.

只检查、只输出报告，⛔ 绝不修改/删除/创建任何文档。
每月定时任务与重要版本发布后跑一次:

    python3 tools/doc_audit.py            # 终端报告
    python3 tools/doc_audit.py -o report.txt   # 另存报告(报告文件除外,不动任何文档)

检查项:
  1. 失效的源码路径引用      —— 文档里 `components/...` / `main/...` 等路径是否真实存在
  2. 过期的版本号            —— 文档里写死的版本 vs lintos_version.h
  3. API 文档是否同步        —— 调 tools/lua_api_doc.py --check (只读)
  4. 新增模块缺少说明        —— 扫 components/*/ 下有 CMakeLists.txt 但模块地图未提及的
  5. 重复内容                —— 文档间相同标题(三级及以上)出现在多处
  6. 矛盾表述                —— 同一文件里版本串不一致等已知易错点

退出码: 0=无严重问题(可含 WARN), 1=存在 ERROR 项。
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")

CORE_DOCS = [
    "开发总索引.md", "模块地图.md", "API手册.md", "Lua应用.md", "UI模块.md",
    "硬件配置.md", "系统开发.md", "综合手册.md",
]
GEN_API = "手册/API完整参考手册.md"          # 生成工具的固定输出, 保留
VERSION_H = "components/system/os/include/lintos_version.h"

errors, warns, infos = [], [], []


def rd(rel):
    p = os.path.join(ROOT, rel)
    if not os.path.exists(p):
        return ""
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


# ---- 1. 失效源码路径 ----
PATH_RE = re.compile(r"`((?:components|main|apps|tools|server|dist|store|system_image)/[A-Za-z0-9_\-./]+?\.(?:c|h|cpp|lua|py|sh|php|json|csv|md|bin|lf|txt|yml))`")
PLACEHOLDER = ("xxx", "/x.lua", "hello.lua", "app.lua", "page_xxx", "<同名>", "*")
seen_paths = set()
for name in CORE_DOCS + [GEN_API]:
    for ln, line in enumerate(rd(os.path.join("docs", name)).split("\n"), 1):
        # 明确标注为「旧路径 / 已不存在 / 不要再用」的行是有效历史说明, 跳过
        if any(k in line for k in ("旧路径", "已不存在", "不要再", "不再使用")):
            continue
        for p in PATH_RE.findall(line):
            if p in seen_paths or any(x in p for x in PLACEHOLDER):
                continue
            seen_paths.add(p)
            if not os.path.exists(os.path.join(ROOT, p)):
                warns.append("[失效路径] docs/%s:%d → %s" % (name, ln, p))

# ---- 2. 过期版本 ----
vh = rd(VERSION_H)
m_pub = re.search(r'LINTOS_VERSION_PUBLIC\s+"([^"]+)"', vh)
m_str = re.search(r'LINTOS_VERSION_STR\s+"([^"]+)"', vh)
pub = m_pub.group(1) if m_pub else "?"
full = m_str.group(1) if m_str else "?"
for name in CORE_DOCS:
    for ln, line in enumerate(rd(os.path.join("docs", name)).split("\n"), 1):
        if re.search(r"2\.0\.0 LinTOS 固件", line) or "现 **2.0.0**" in line:
            errors.append("[过期版本] docs/%s:%d 仍写 2.0.0, 真值 %s" % (name, ln, full))
        if "LinTOS 1.0" in line and "适用" in line:
            warns.append("[过期版本] docs/%s:%d 出现 '适用 LinTOS 1.0'" % (name, ln))

# ---- 3. API 文档同步(只读 --check) ----
try:
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "lua_api_doc.py"), "--check"],
                       cwd=ROOT, capture_output=True, text=True, timeout=60)
    if r.returncode == 0:
        infos.append("[API] 生成文档与源码一致: " + r.stdout.strip().split("\n")[0])
    else:
        errors.append("[API] 文档与源码不一致, 需重跑 tools/lua_api_doc.py: " + r.stdout.strip()[:200])
except Exception as e:
    warns.append("[API] 无法执行 --check: %s" % e)

# ---- 4. 新增模块缺少说明 ----
map_txt = rd("docs/模块地图.md")
for grp in ["drivers", "services", "system", "engines", "ui", "libs", "tools"]:
    base = os.path.join(ROOT, "components", grp)
    if not os.path.isdir(base):
        continue
    for d in os.listdir(base):
        if not os.path.isdir(os.path.join(base, d)):
            continue
        if not os.path.exists(os.path.join(base, d, "CMakeLists.txt")):
            continue
        if d not in map_txt and d not in ("test",):
            warns.append("[新增模块] components/%s/%s 有 CMakeLists.txt 但模块地图未提及" % (grp, d))

# ---- 5. 重复标题(跨文档) ----
head_owner = {}
for name in CORE_DOCS:
    for ln, line in enumerate(rd(os.path.join("docs", name)).split("\n"), 1):
        m = re.match(r"^#{2,4}\s+(.+?)\s*$", line)
        if not m:
            continue
        t = re.sub(r"\s+", "", m.group(1))
        if len(t) < 8:
            continue
        if t in head_owner and head_owner[t][0] != name:
            errors.append("[重复标题] '%s' 同时出现在 docs/%s 与 docs/%s" % (
                m.group(1)[:30], head_owner[t][1], name))
        else:
            head_owner[t] = (name, name)

# ---- 6. 矛盾: 版本号自述 ----
for name in CORE_DOCS:
    txt = rd(os.path.join("docs", name))
    vers = set(re.findall(r"LinTOS\s*(\d+\.\d+(?:\.\d+)?)", txt))
    vers.discard(pub)
    if len(vers) > 1:
        infos.append("[版本并存] docs/%s 出现多个版本号 %s (可能均属历史叙述,人工确认)" % (name, sorted(vers)))

# ---- 输出 ----
print("=" * 64)
print("LinTOS 文档体检  |  源码版本 %s (%s)  |  核心文档 %d 份" % (pub, full, len(CORE_DOCS)))
print("=" * 64)
for tag, items in [("ERROR", errors), ("WARN", warns), ("INFO", infos)]:
    if items:
        print("\n[%s] %d 项" % (tag, len(items)))
        for it in items:
            print("  " + it)
if not errors and not warns:
    print("\n✅ 未发现问题")
print("\n说明: 本工具只读, 不删除/重写/合并任何文件。WARN 项需人工核实后再处理。")
sys.exit(1 if errors else 0)
