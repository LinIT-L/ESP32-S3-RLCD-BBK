#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""lua_api_doc.py — LinTOS Lua 桥「API 文档自动化」(对标 LuatOS 的 tools/sys_pub.py)

为什么需要它
------------
手写 API 文档必然过时 —— 用户照着旧文档写就出错。这里把**唯一的真相源**定死为源码:
     ① 注册行  `lua_pushcfunction(L, l_xxx); lua_setfield(L, -2, "name");`   → 有哪些 API
     ② 子表    `lua_newtable(L)` … `lua_setfield(L, -2, "tab")`              → 挂在哪一层
     ③ 函数上方的注释块                                                       → 说明文字
`scan` 生成 docs/Lua桥API参考.md; `--check` 与源码对账, 不一致就非零退出(可挂进构建/CI)。

用法
----
    python3 tools/lua_api_doc.py                 # 生成文档
    python3 tools/lua_api_doc.py --check         # 只对账, 不写文件(exit 1 = 不一致)
    python3 tools/lua_api_doc.py -o /tmp/api.md
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BD = os.path.join(ROOT, "components", "apps", "lua_bridge")
OUT_DEFAULT = os.path.join(ROOT, "docs", "Lua桥API参考.md")

MODULE_DESC = {
    "consts": "常量（`W/H/BLACK/WHITE` 与动作码 `UP/DOWN/LEFT/RIGHT/OK/CONFIRM/BACK/HOME`）",
    "gfx":    "绘制与字模（图元 / 8 套字模 / 位图 / 字体宽度）",
    "input":  "时间与输入（按键 / 按住状态 / 自动重复 / 触摸 / 重绘）",
    "sys":    "系统与提示（日志 / 音效 / 震动 / 退出 / 状态栏，以及 `sys`、`dlg` 子表）",
    "fs":     "文件（TF 卡，`fs` 子表）",
    "store":  "持久化（`nvs` 掉电保留、`db` 核心互通库）",
    "net":    "网络（`net` 子表）",
    "wifi":   "无线（`wifi` 子表）",
    "hid":    "USB 键鼠（`hid` 子表）",
    "kbd":    "软键盘（`kbd` 子表）",
    "img":    "大图与精灵（`img` 子表：1bpp BMP 载入 PSRAM + 窗口位块传输，地图/角色用）",
    "gps":    "定位（NMEA 解析结果，经纬度走整数微度）",
    "espnow": "ESP-NOW 点对点/广播（≤250 字节/包，不用路由器；双机联机、遥控、信标）",
}

REG = re.compile(r'lua_pushcfunction\(L,\s*(l_\w+)\);\s*lua_setfield\(L,\s*-2,\s*"([^"]+)"\)')
NEWTAB = re.compile(r'lua_newtable\(L\)')
SETTAB = re.compile(r'^\s*lua_setfield\(L,\s*-2,\s*"([^"]+)"\);')
FUNC = re.compile(r'^static\s+int\s+(l_\w+)\s*\(lua_State\s*\*\s*L\)')


def trim_desc(txt, limit=120):
    """把函数上方的注释整理成一句话: 丢掉分隔线/装饰符, 只取最后一段, 限长。"""
    if not txt:
        return ""
    txt = re.sub(r"[-=*]{3,}", " ", txt)          # 去掉 ---- ==== *** 这类分隔
    parts = [p.strip() for p in re.split(r"[。;\n]", txt) if p.strip()]
    if not parts:
        return ""
    s = parts[-1] if len(parts[-1]) > 8 else parts[0]   # 优先用最后一句(最贴近函数)
    s = re.sub(r"\s+", " ", s).strip(" -*/\\")
    if len(s) > limit:
        s = s[:limit].rstrip() + "…"
    return s


def strip_comment(lines):
    """把函数上方连续的注释行拼成一句话 (支持 /* */ 与 //)。"""
    out = []
    i = len(lines) - 1
    while i >= 0:
        l = lines[i].strip()
        if l == "" and not out:
            i -= 1
            continue
        if l.startswith("/*") or l.startswith("*") or l.endswith("*/") or l.startswith("//"):
            out.insert(0, l)
            i -= 1
            continue
        break
    if not out:
        return ""
    txt = " ".join(out)
    txt = re.sub(r"/\*+|\*+/", " ", txt)
    txt = re.sub(r"^\s*\*\s*", " ", txt, flags=re.M)
    txt = txt.replace("//", " ")
    txt = re.sub(r"\s+", " ", txt).strip()
    return txt


def parse_module(path):
    """返回 (apis, errors)。apis = [(full_name, func, desc, lineno)]

    ⚠ 子表名在**块的末尾**才出现 (`lua_newtable` … `lua_setfield(L,-2,"tab")`),
    所以不能边扫边定名 —— 必须**两遍**: 先找出每个子表的 [起, 止] 行范围与名字,
    再把落在这个范围里的注册行归到该子表。
    """
    lines = open(path, encoding="utf-8").read().split("\n")
    funcs = {}
    for i, l in enumerate(lines):
        m = FUNC.match(l)
        if m:
            funcs[m.group(1)] = (i, trim_desc(strip_comment(lines[:i])))

    # ---- 第一遍: 找出 子表 的名字 + 行范围 ----
    tables = []          # (first_line, last_line, name)
    for i, l in enumerate(lines):
        if not NEWTAB.search(l):
            continue
        for j in range(i + 1, len(lines)):
            if REG.search(lines[j]):        # 带 pushcfunction 的不是收尾
                continue
            m2 = SETTAB.match(lines[j])
            if m2:
                tables.append((i, j, m2.group(1)))
                break

    def tab_of(ln):
        for a, b, name in tables:
            if a < ln < b:
                return name
        return None

    # ---- 第二遍: 归一化注册行 ----
    apis, errors, seen = [], [], {}
    for i, l in enumerate(lines):
        m = REG.search(l)
        if not m:
            continue
        fn, field = m.group(1), m.group(2)
        tab = tab_of(i)
        full = ("app.%s.%s" % (tab, field)) if tab else ("app.%s" % field)
        if full in seen:
            errors.append("%s: 字段 %s 重复注册（第 %d 行 与 第 %d 行）" % (
                os.path.basename(path), full, seen[full], i + 1))
        seen[full] = i + 1
        apis.append((full, fn, funcs.get(fn, (0, ""))[1], i + 1))
    return apis, errors


def collect():
    all_apis, errors, mods = {}, [], []
    for fn in sorted(os.listdir(BD)):
        if not fn.startswith("luat_lib_") or not fn.endswith(".c"):
            continue
        mod = fn[len("luat_lib_"):-2]
        mods.append(mod)
        apis, errs = parse_module(os.path.join(BD, fn))
        all_apis[mod] = apis
        errors += errs
    return mods, all_apis, errors


def render(mods, all_apis):
    total = sum(len(v) for v in all_apis.values())
    out = ["# Lua 桥 API 参考（自动生成，勿手改）", "",
           "> 由 `tools/lua_api_doc.py` 从 `components/apps/lua_bridge/luat_lib_*.c` **自动提取**。",
           "> 「说明」一列取的是源码里函数上方的注释 —— **改代码就是改文档**，不需要手改本文件。",
           "> 共 **%d** 个 API / %d 个模块。加 API 见技能 `lintos-lua-app-pipeline` §18。「定义」列是源码位置。" % (total, len(mods)),
           ""]
    for mod in mods:
        apis = all_apis.get(mod) or []
        if not apis:
            continue
        out += ["## `%s` — %s" % (mod, MODULE_DESC.get(mod, "")), "",
                "| API | 说明 | 定义 |", "|---|---|---|"]
        for full, fn, desc, ln in apis:
            d = desc if desc else "（源码未写注释）"
            d = d.replace("|", "\\|")
            out.append("| `%s` | %s | `luat_lib_%s.c:%d` |" % (full, d, mod, ln))
        out.append("")
    out += ["---", "",
            "## 约定（所有 API 通用）", "",
            "- 动作码：`1=UP 2=DOWN 3=LEFT 4=RIGHT 5=CONFIRM(OK) 6=BACK 7=HOME`；",
            "  `app.UP..app.HOME` 与此一一对应，`consts` 模块用 `_Static_assert` 与 `os_action_t` 钉死。",
            "- 颜色：仅黑白两色，`app.BLACK=0` / `app.WHITE=1`（反射式 1bpp 屏，白纸黑字）。",
            "- 屏幕：`400×300`；16px 中文请用 **`app.zh`**（`font_zh16` 真 16×16，与系统 UI 同款），",
            "  ⛔ 不要用 `app.zhsb`（那是 24×24 降采样，会糊）。ASCII 用 `app.textsb`（8×16）。",
            "- 回调契约：`on_enter / render / poll / action(a) / on_exit`；按键**只走 `action(a)`** 一条路。",
            ""]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只对账不写文件; 不一致则 exit 1")
    ap.add_argument("-o", "--out", default=OUT_DEFAULT)
    args = ap.parse_args()

    mods, all_apis, errors = collect()
    total = sum(len(v) for v in all_apis.values())
    if not total:
        sys.exit("✗ 一个 API 都没扫到 —— lua_bridge/ 目录或生成的文件不对")

    text = render(mods, all_apis)

    # ---- 对账: 已生成的文档 vs 源码 ----
    old = open(args.out, encoding="utf-8").read() if os.path.exists(args.out) else ""
    names_new = set(re.findall(r"^\| `(app\.[\w.]+)` \|", text, re.M))
    names_old = set(re.findall(r"^\| `(app\.[\w.]+)` \|", old, re.M))
    missing = sorted(names_new - names_old)
    extra = sorted(names_old - names_new)

    print("扫到 %d 个 API / %d 个模块: %s" % (total, len(mods), " ".join(mods)))
    for e in errors:
        print("  ✗ %s" % e)
    if args.check:
        if errors or missing or extra:
            if missing:
                print("  ✗ 文档缺这些 API: %s" % " ".join(missing))
            if extra:
                print("  ✗ 文档里有源码中已不存在的 API: %s" % " ".join(extra))
            sys.exit(1)
        print("  ✅ 文档与源码一致")
        return

    open(args.out, "w", encoding="utf-8").write(text)
    print("✓ 已生成 %s (%d 行)" % (os.path.relpath(args.out, ROOT), text.count("\n")))
    if missing:
        print("  (本次新增 %d 个 API 条目)" % len(missing))
    if extra:
        print("  ⚠ 源码中已移除但仍残留在旧文档里的: %s" % " ".join(extra))
    if errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
