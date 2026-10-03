#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""biossim_gen_lua.py — 从 apps/biossim/biossim.html 的 @DATA 段抽数据 → 生成掌机版 Lua 的数据表

为什么这么做:
  HTML 版和掌机 Lua 版必须是**同一份数据**（7 品牌 / 51 页 / 8 任务 / 100+ 选项）。
  手抄一遍必然两边漂移；所以让 HTML 做单一数据源，Lua 侧的数据段由本脚本**重新生成**。

机制:
  1) 从 HTML 里切出 `@DATA-BEGIN` … `@DATA-END` 之间的纯数据 JS（无函数、无副作用）
  2) 用 node 求值 → JSON.stringify({ITEMS,BOOT_DEVICES,BRANDS,PAGES,SUBPAGES,TASKS,ACTIONS,GPM})
  3) 把 JSON 转成 Lua 表字面量, **整段替换** 掉 biossim.lua 里 `-- @@DATA-BEGIN@@ … -- @@DATA-END@@` 之间的内容

幂等: 重复跑结果一致（数据段被整段替换，不追加）。

用法:
  python3 tools/biossim_gen_lua.py            # 生成并写回
  python3 tools/biossim_gen_lua.py --check    # 只校验 Lua 里的数据段是否与 HTML 同步
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTML = os.path.join(ROOT, "apps", "biossim", "biossim.html")
LUA = os.path.expanduser("~/Desktop/LinTOS_Lua应用/src/biossim.lua")
NODE = "/Users/linit/.workbuddy/binaries/node/versions/22.22.2-3/bin/node"

BEGIN, END = "/* @DATA-BEGIN", "/* @DATA-END */"
LBEGIN, LEND = "-- @@DATA-BEGIN@@", "-- @@DATA-END@@"


def slice_data(html: str) -> str:
    i = html.index(BEGIN)
    i = html.index("*/", i) + 2
    j = html.index(END, i)
    return html[i:j]


def export_json(js: str) -> dict:
    script = js + "\nconsole.log(JSON.stringify({ITEMS:ITEMS,BOOT_DEVICES:BOOT_DEVICES,"
    script += "BRANDS:BRANDS,PAGES:PAGES,SUBPAGES:SUBPAGES,TASKS:TASKS,ACTIONS:ACTIONS}));"
    r = subprocess.run([NODE, "-e", script], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("node 求值失败:\n" + (r.stderr or "")[-2000:])
    return json.loads(r.stdout.strip().splitlines()[-1])


# ------------------------------------------------- 固件字库覆盖率兜底
# 掌机固件字库只有「ASCII + 汉字」；下面这些符号在设备上是**整字空白**。
# HTML 版留着好看，Lua 版必须换掉 —— 所以在生成时统一替换（一处改、两个版本都不会漏）。
SYMBOL_MAP = [
    ("\u2192", "->"), ("\u2190", "<-"), ("\u2191", "^"), ("\u2193", "v"),
    ("\u21d2", "=>"), ("\u21d4", "<->"), ("\u21c4", "<->"),
    ("\u26a0", "[!]"), ("\u2714", "[OK]"), ("\u2718", "[X]"), ("\u2713", "[OK]"),
    ("\u25b6", ">"), ("\u25c0", "<"), ("\u25b2", "^"), ("\u25bc", "v"),
    ("\u25cf", "*"), ("\u25cb", "o"), ("\u2026", "..."), ("\u00b7", "-"),
    ("\u00d7", "x"), ("\u00f7", "/"), ("\u2212", "-"), ("\u2264", "<="), ("\u2265", ">="),
    ("\u00b2", "2"), ("\u2103", "C"), ("\u2109", "F"), ("\u2014", "-"), ("\u2013", "-"),
    ("\u00b0", ""),                      # 45°C -> 45C（固件字库没有 °）
    ("\u2018", "'"), ("\u2019", "'"), ("\u201c", '"'), ("\u201d", '"'),
]

# 汉字也有字库覆盖问题（闸门 lua_check.py 会扫全部字符串字面量）→ 整串替换
STRING_FIX = [
    ("\u65e5\u672c\u8a9e", "\u65e5\u6587"),      # 日本語 -> 日文（語 不在固件字库）
]


def sanitize(v, report):
    if isinstance(v, str):
        for bad, good in STRING_FIX:
            if bad in v:
                report.add(bad)
                v = v.replace(bad, good)
        for bad, good in SYMBOL_MAP:
            if bad in v:
                report.add(bad)
                v = v.replace(bad, good)
        return v
    if isinstance(v, list):
        return [sanitize(x, report) for x in v]
    if isinstance(v, dict):
        return dict((k, sanitize(x, report)) for k, x in v.items())
    return v


# ---------------------------------------------------------------- Lua 序列化
KEY_OK = re.compile(r"^[A-Za-z_][A-Za-z_0-9]*$")


def lua_str(s: str) -> str:
    """Lua 双引号字符串（注意先转义反斜杠与引号；中文直接放进去，源文件是 UTF-8）"""
    out = s.replace("\\", "\\\\").replace('"', '\\"')
    out = out.replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    return '"' + out + '"'


def lua_key(k: str) -> str:
    return k if KEY_OK.match(k) else "[" + lua_str(k) + "]"


def is_list(v) -> bool:
    return isinstance(v, list)


def ser(v, ind=0) -> str:
    sp = "  " * ind
    sp1 = "  " * (ind + 1)
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        if isinstance(v, float) and v == int(v):
            return str(int(v))
        return repr(v)
    if isinstance(v, str):
        return lua_str(v)
    if is_list(v):
        if not v:
            return "{}"
        # 全是标量 → 一行放得下就放一行（文件小很多）
        if all(isinstance(x, (str, int, float, bool)) or x is None for x in v):
            widths = [len(ser(x)) for x in v]
            if sum(widths) + 2 * (len(v) - 1) + 2 <= 92:
                return "{" + ", ".join(ser(x) for x in v) + "}"
        parts = [ser(x, ind + 1) for x in v]
        return "{\n" + "".join(sp1 + p + ",\n" for p in parts) + sp + "}"
    if isinstance(v, dict):
        if not v:
            return "{}"
        parts = ["%s%s = %s" % (sp1, lua_key(k), ser(val, ind + 1)) for k, val in v.items()]
        # 短表压成一行
        if len(parts) <= 2 and sum(len(p) for p in parts) < 90:
            return "{ " + ", ".join(p.strip() for p in parts) + " }"
        return "{\n" + "".join(p + ",\n" for p in parts) + sp + "}"
    raise TypeError(type(v))


ICON_DIR = os.path.join(ROOT, "apps", "biossim", "brand_icons")
ICON_W = ICON_H = 64


def brand_icons():
    """读 apps/biossim/brand_icons/<品牌>.pbm (P4 1bpp 64x64) → {品牌: hex}

    图标是**可替换资源**(linit: 图标可网上下载后替换, 规格 64x64)。
    换成自己的图后重跑本脚本即可嵌进 Lua 的 BRAND_ICON 表。
    用 hex 而不是二进制: 走 app.bmp(x,y,w,h,hex) 通道(限 2048B, 64x64=512B 够用),
    也避免把裸二进制写进 .lua 源文件。"""
    out = {}
    if not os.path.isdir(ICON_DIR):
        return out
    for fn in sorted(os.listdir(ICON_DIR)):
        if not fn.endswith(".pbm"):
            continue
        d = open(os.path.join(ICON_DIR, fn), "rb").read()
        head, _, rest = d.partition(b"\n")
        dims, _, body = rest.partition(b"\n")
        if head.strip() != b"P4":
            raise SystemExit("%s 不是 P4 格式" % fn)
        w, h = [int(x) for x in dims.split()]
        if (w, h) != (ICON_W, ICON_H):
            raise SystemExit("%s 尺寸 %dx%d, 要求 %dx%d" % (fn, w, h, ICON_W, ICON_H))
        want = (w // 8) * h
        if len(body) != want:
            raise SystemExit("%s 数据 %d 字节, 应为 %d" % (fn, len(body), want))
        out[fn[:-4]] = body.hex()
    return out


def emit(data: dict) -> str:
    order = ["ITEMS", "BOOT_DEVICES", "BRANDS", "PAGES", "SUBPAGES", "TASKS", "ACTIONS"]
    out = [LBEGIN,
           "-- ⚠ 本段由 tools/biossim_gen_lua.py 从 apps/biossim/biossim.html 的 @DATA 段生成，**不要手改**。",
           "--    改选项/任务/菜单请改 HTML 那一段，然后重跑脚本；两边永远同源。",
           "-- 显示顺序：照 HTML 里的书写顺序（第一个就是默认项）。"]
    # 顺序表单独发一份：Lua 的 pairs() 无序，靠它把"第一个任务"稳定成 win11 这种有意义的默认
    for name, src in (("BRAND_ORDER", "BRANDS"), ("TASK_ORDER", "TASKS")):
        ks = list(data[src].keys())
        out.append("local %s = {%s}" % (name, ", ".join(lua_str(k) for k in ks)))
    for k in order:
        out.append("local %s = %s" % (k, ser(data[k], 1)))
    # 品牌图标（可替换资源；没有 pbm 就是空表 → 界面自动退化成"只有名字"）
    ic = brand_icons()
    if ic:
        out.append("-- 品牌图标 64x64 1bpp（来自 apps/biossim/brand_icons/*.pbm, 可替换）")
        out.append("local BRAND_ICON = %s" % ser(ic, 1))
    else:
        out.append("local BRAND_ICON = {}   -- 没找到 brand_icons/*.pbm → 界面只显示品牌名")
    out.append(LEND)
    return "\n".join(out) + "\n"


def to_lua_index(data: dict) -> dict:
    """JS 的数组下标是 **0 起**，Lua 是 **1 起**。
    这里把"下标语义"的字段统一加 1 —— 否则 Lua 侧 vals[0] 恒为 nil，
    表现为 evaluate 里 "attempt to concatenate a nil value"。
      · ITEMS.*.def            → 默认值下标
      · TASKS.*.rules[][2]     → 期望值下标（rules = { {key, want, why}, ... }）
    """
    for it in data["ITEMS"].values():
        d = it.get("def", 0)
        it["def"] = (int(d) + 1) if isinstance(d, (int, float)) else 1
    for t in data["TASKS"].values():
        for r in t.get("rules", []):
            if len(r) >= 2 and isinstance(r[1], (int, float)):
                r[1] = int(r[1]) + 1
    return data


def main():
    check = "--check" in sys.argv
    html = open(HTML, encoding="utf-8").read()
    data = export_json(slice_data(html))
    report = set()
    data = dict((k, sanitize(v, report)) for k, v in data.items())
    data = to_lua_index(data)
    if report:
        print("· 已把 %d 个固件字库没有的符号换成 ASCII: %s"
              % (len(report), " ".join(sorted(report))))
    blob = emit(data)
    if check:
        cur = open(LUA, encoding="utf-8").read()
        m = re.search(re.escape(LBEGIN) + r".*?" + re.escape(LEND) + "\n", cur, re.S)
        if not m:
            print("✗ biossim.lua 里找不到 @@DATA@@ 标记段")
            sys.exit(1)
        if m.group(0) == blob:
            print("✓ 掌机版数据段与 HTML 同源（无差异）")
            return
        print("✗ 掌机版数据段与 HTML **不同步** → 跑 `python3 tools/biossim_gen_lua.py`")
        sys.exit(1)

    cur = open(LUA, encoding="utf-8").read()
    m = re.search(re.escape(LBEGIN) + r".*?" + re.escape(LEND) + "\n", cur, re.S)
    if not m:
        raise SystemExit("✗ biossim.lua 里找不到 @@DATA@@ 标记段")
    open(LUA, "w", encoding="utf-8").write(cur[:m.start()] + blob + cur[m.end():])
    n_item = len(data["ITEMS"])
    n_page = sum(len(v) for v in data["PAGES"].values())
    print("✓ 已同步: %d 选项 / %d 品牌 / %d 页面 / %d 任务 / %d 引导设备  → %s"
          % (n_item, len(data["BRANDS"]), n_page, len(data["TASKS"]),
             len(data["BOOT_DEVICES"]), LUA))


if __name__ == "__main__":
    main()
