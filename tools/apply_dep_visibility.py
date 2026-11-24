#!/usr/bin/env python3
"""apply_dep_visibility.py — 把只在源文件里用到的依赖从 REQUIRES 降为 PRIV_REQUIRES。

规则 (ESP-IDF 官方 Build System 文档):
    REQUIRES      = 被本组件**公共头文件** #include 的依赖
    PRIV_REQUIRES = 只被**源文件/私有头** #include 的依赖

默认只打印计划, 加 --apply 才写文件。改写是**逐行**的: 注释行原样保留,
只从依赖行里删掉目标 token, 绝不重排整块 (重排会把行尾注释拆成单词)。
"""
import os
import re
import sys
import glob
import importlib.util
from collections import defaultdict, deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_saved_argv = list(sys.argv)
sys.argv = [sys.argv[0]]

spec = importlib.util.spec_from_file_location(
    "cdv", os.path.join(ROOT, "tools", "check_dep_visibility.py"))
cdv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cdv)
sys.argv = _saved_argv

APPLY = "--apply" in sys.argv

# 参数块之间的分隔关键字: 遇到它们就说明当前块结束了
STOP = ("REQUIRES", "PRIV_REQUIRES", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS",
        "SRCS", "EMBED_FILES", "EMBED_TXTFILES", "LDFRAGMENTS",
        "WHOLE_ARCHIVE", "REQUIRED_IDF_TARGETS")
STOP_RE = re.compile(r"^\s*(%s)\b" % "|".join(STOP))


def locate_block(lines, key):
    """返回 (start, end) 行号: start = key 所在行, end = 块结束后第一行(不含).
    PRIV_REQUIRES 不会被误认成 REQUIRES: 行首必须是完整关键字。"""
    start = None
    for i, ln in enumerate(lines):
        if re.match(r"^\s*" + key + r"\s", ln) or re.match(r"^\s*" + key + r"\s*$", ln):
            start = i
            break
    if start is None:
        return None
    depth = lines[start].count("(") - lines[start].count(")")
    end = start + 1
    while end < len(lines):
        s = lines[end]
        if depth <= 0 or ")" in s or STOP_RE.match(s) and end != start:
            # ')' 可能和最后一个依赖同行, 该行仍需处理 ⇒ 交给调用方判断
            if ")" in s or STOP_RE.match(s):
                break
        depth += s.count("(") - s.count(")")
        end += 1
    return start, min(end + 1, len(lines))


def strip_tokens(lines, start, end, targets):
    """从 [start, end) 行区间里删掉 targets 里的 token; 返回 (新行列表, 实际删掉的)"""
    out, removed = [], set()
    for i in range(len(lines)):
        ln = lines[i]
        if not (start <= i < end):
            out.append(ln)
            continue
        stripped = ln.strip()
        if not stripped or stripped.startswith("#"):
            out.append(ln)
            continue
        # 只删"独立出现"的 token, 不碰行尾注释里的词
        code, sep, comment = ln.partition("#")
        for t in targets:
            pat = re.compile(r'(?<![\w.-])"?%s"?(?![\w.-])' % re.escape(t))
            if pat.search(code):
                code = pat.sub("", code, count=1)
                removed.add(t)
        code = re.sub(r"[ \t]{2,}", " ", code).rstrip()
        if code.strip() in ("", '"""') or not code.strip():
            continue                       # 整行只剩空白 ⇒ 丢掉该行
        out.append(code + (sep + comment if sep else "") + "\n")
    return out, removed


def main():
    comps = cdv.find_components()
    reg = {n: cdv.parse_register(c) for n, c in comps.items()}

    owner, pub_hdrs = {}, {}
    for n, c in comps.items():
        hs = cdv.public_headers(c, reg[n]["inc"])
        pub_hdrs[n] = hs
        for h in hs:
            owner.setdefault(os.path.basename(h), n)
    edges = defaultdict(set)
    for n in comps:
        for h in pub_hdrs[n]:
            try:
                txt = open(h, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            for inc in cdv.INCLUDE_RE.findall(txt):
                b = os.path.basename(inc)
                if b in owner and owner[b] != n:
                    edges[n].add(owner[b])

    def reachable(n):
        seen, q = set(), deque(edges[n])
        while q:
            x = q.popleft()
            if x in seen:
                continue
            seen.add(x)
            q.extend(edges[x] - seen)
        return seen

    total = 0
    for n in sorted(comps):
        pub = reachable(n)
        priv_now = set(reg[n]["priv"])
        targets = [d for d in reg[n]["requires"]
                   if d not in cdv.IDF_COMMON and d not in cdv.INTERNAL
                   and d in comps and d not in pub and d not in priv_now]
        if not targets:
            continue

        p = os.path.join(comps[n], "CMakeLists.txt")
        lines = open(p, encoding="utf-8").readlines()
        rng = locate_block(lines, "REQUIRES")
        if not rng:
            print("!! %s 无 REQUIRES 块, 跳过" % n)
            continue
        new, moved = strip_tokens(lines, rng[0], rng[1], targets)
        if not moved:
            continue

        add = " ".join(sorted(moved))
        prng = locate_block(new, "PRIV_REQUIRES")
        if prng:
            # 追加到 PRIV_REQUIRES 块内最后一个非注释行末尾
            for i in range(prng[1] - 1, prng[0], -1):
                s = new[i].rstrip("\n")
                if s.strip() and not s.strip().startswith("#"):
                    code, sep, comment = s.partition("#")
                    new[i] = (code.rstrip() + " " + add + (("  " + sep + comment) if sep else "") + "\n")
                    break
        else:
            head = rng[0]
            indent = re.match(r"(\s*)", new[head]).group(1)
            new.insert(head, indent + "PRIV_REQUIRES " + add + "\n")

        print("%s %-20s %2d 条 → PRIV: %s" % ("✅" if APPLY else "·", n, len(moved), add))
        total += len(moved)
        if APPLY:
            open(p, "w", encoding="utf-8").writelines(new)

    print("\n%s %d 条 (组件 %d)" % ("已写入" if APPLY else "计划(未写入, 加 --apply 生效)",
                                   total, 0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
