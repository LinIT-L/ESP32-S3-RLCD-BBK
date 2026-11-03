#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_store_index_builtin.py — 由 store/index.json 生成固件内置索引头。

产物: components/apps/pages/store_index_builtin.h
      static const char STORE_INDEX_BUILTIN[] = "<json>";

用途: 设备**离线**（或第一次拉线上索引失败）时用的兜底索引 —— 让商店在没有网络时
      也能把"有哪些资源"列出来（按钮显示"下载"，点下去才提示需要联网）。

为什么必须跟线上一致（2026-10-31）:
  内置索引里带 mirrors[]。如果它还是旧地址（早已 404 的 jsdelivr/raw），
  设备在主源失败时会照着死链再试两次，白等 30 秒。所以每次改 mirrors / 上下架资源,
  都要重跑本脚本并**重新编译固件**。

用法:
  python3 tools/gen_store_index_builtin.py            # 从 store/index.json 生成
  python3 tools/gen_store_index_builtin.py --check    # 只比对，不写文件（CI 用）
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "store", "index.json")
DST = os.path.join(ROOT, "components", "apps", "pages", "store_index_builtin.h")

BANNER = """/* store_index_builtin.h — 固件内置商店索引(离线可用), 自动生成
 *
 * ⚠ 不要手改 —— 由 tools/gen_store_index_builtin.py 从 store/index.json 生成。
 *   改完必须重新编译固件才生效（设备离线兜底 + mirrors 回退列表都在这里）。
 *   生成时间: %s
 */
#pragma once
static const char STORE_INDEX_BUILTIN[] =
"""

# 正文起点: 这一行之后就是字符串字面量。--check 只比对正文，
# 否则"生成时间"一变就必然报不一致（2026-10-31 修: 原来 --check 恒失败）。
BODY_MARK = "STORE_INDEX_BUILTIN[] =\n"


def c_escape(line):
    return line.replace("\\", "\\\\").replace('"', '\\"')


def body(js_text):
    """把 JSON 原文转成 C 相邻字符串字面量（每行一段，便于 diff/阅读）。"""
    lines = js_text.split("\n")
    out = []
    for i, ln in enumerate(lines):
        last = (i == len(lines) - 1)
        out.append('    "%s%s"' % (c_escape(ln), "" if last else "\\n"))
    return "\n".join(out)


def render(js_text):
    import time
    return BANNER % time.strftime("%Y-%m-%d %H:%M:%S") + body(js_text) + ";\n"


def decode_payload(header_text):
    """把已有头文件里的 C 字符串字面量**解码**成真实字节串。

    为什么不能直接比正文文本: 旧的生成方式（build.sh 里那段内联 python）把整个 JSON
    塞进**一个**长字面量，新的按行拆成多个相邻字面量。两者**编译后的字符串完全相同**，
    但源码文本不同。直接比文本会永远报"不一致"（2026-10-31 修的第二个坑）。
    """
    if BODY_MARK not in header_text:
        return None
    tail = header_text.split(BODY_MARK, 1)[1]
    out = []
    i, n = 0, len(tail)
    while i < n:
        ch = tail[i]
        if ch == '"':
            i += 1
            while i < n and tail[i] != '"':
                c = tail[i]
                if c == "\\" and i + 1 < n:
                    nxt = tail[i + 1]
                    out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"', "r": "\r"}.get(nxt, nxt))
                    i += 2
                    continue
                out.append(c)
                i += 1
            i += 1              # 跳过收尾引号
            continue
        if ch == ";":           # 声明结束
            break
        i += 1
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    with open(SRC, encoding="utf-8") as f:
        js = f.read()
    json.loads(js)                      # 先确认源是合法 JSON（避免把坏索引烧进固件）
    out = render(js)

    if args.check:
        cur = open(DST, encoding="utf-8").read() if os.path.exists(DST) else ""
        same = (decode_payload(cur) == js)
        print("内置索引与 store/index.json %s" % ("一致 ✅" if same else "**不一致** ✗"))
        if not same:
            print("  → 跑 python3 tools/gen_store_index_builtin.py 后重新编译固件")
        return 0 if same else 1

    with open(DST, "w", encoding="utf-8") as f:
        f.write(out)
    n = len(json.loads(js).get("pkgs", []))
    print("✓ 已生成 %s（%d 项, %d B）" % (os.path.relpath(DST, ROOT), n, len(out)))
    print("  ⚠ 需要重新编译固件才会生效")
    return 0


if __name__ == "__main__":
    sys.exit(main())
