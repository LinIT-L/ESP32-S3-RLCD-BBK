#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_app_registry.py — 内置应用「能不能打开」静态自检 (不需要编译/烧录)

背景 / 它解决什么
----------------
2026-09-29 事故: 10 个 C 版内置应用被加回 s_builtin_apps[], 但它们的
os_page_*_register() 调用还留在 apps_registry.c 的 `#if 0` 里。
症状: 应用管家里**看得到**这些应用, 点下去**一点反应都没有**
      (os_push 因模块未注册而失败, 只在 UART 日志里留一行 push: page N not registered)。
真机复现要十几分钟, 而这类错配**纯静态就能查出来**。

对账三方
--------
  ① 应用表 s_builtin_apps[]              → 有哪些应用、指向哪个 OS_PAGE_*
  ② 各页注册函数 os_page_*_register()    → 谁注册了哪个 OS_PAGE_*
  ③ apps_register_all() 的函数体         → 哪些注册函数**真的被调用**
     (按 #if 0 / #if 1 条件编译状态判定)

检查项
------
  [E] 应用指向的页面没有任何注册函数            → 点它必打不开
  [E] 页面有注册函数, 但调用被 #if 0 关掉       → 点它必打不开 (本次事故类型)
  [E] 页面有注册函数, 但根本没写在 apps_register_all() 里
  [E] .id 重复
  [E] 两个应用指向同一个 OS_PAGE_*
  [E] .icon_idx 越界 (>= MAIN_ICON_COUNT) 或未定义
  [W] 页面有注册函数却没有任何应用引用它 (孤儿页, 可能是忘了加进应用表)
  [W] 应用表为空 / 解析到 0 个应用 (解析器失效时避免"假绿")

用法
----
    python3 tools/check_app_registry.py            # 人读报告
    python3 tools/check_app_registry.py --strict   # 有 E 就 exit 1 (接进构建/CI)
    python3 tools/check_app_registry.py -v         # 多打印对账明细

退出码: 0 = 无 E; 1 = 有 E; 2 = 解析失败 (工具自身问题)
"""

import os
import re
import sys
import collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 允许用环境变量换掉被检查的装配文件 —— 便于用"故意改坏的副本"做自测
# (验证工具本身有没有失效), 正常使用不必设置。
REGISTRY_C = os.environ.get("LINTOS_APP_REGISTRY") or os.path.join(
    ROOT, "components", "apps", "apps_registry.c")
OS_HEADER = os.path.join(ROOT, "components", "system", "os", "include", "os.h")
ICON_ENUM = os.path.join(ROOT, "components", "ui", "ui_common", "icons_main_enum.h")

# 扫注册函数的范围 (apps 层页面 + os 内核内置页)
SCAN_DIRS = [
    os.path.join(ROOT, "components", "apps"),
    os.path.join(ROOT, "components", "system", "os"),
]


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def walk_c_files(dirs):
    for d in dirs:
        for base, _dirs, files in os.walk(d):
            for fn in files:
                if fn.endswith((".c", ".h")):
                    yield os.path.join(base, fn)


# ---------------------------------------------------------------- ① 应用表
def parse_os_page_enum():
    """OS_PAGE_XXX → 序号 (按 os.h 里的枚举顺序, 显式赋值优先)。"""
    txt = read(OS_HEADER)
    m = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*os_page_t\s*;", txt, re.S)
    if not m:
        m = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*\w*\s*;", txt, re.S)
    if not m:
        raise RuntimeError("os.h 里找不到 OS_PAGE_* 枚举")
    body = m.group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    val, out = 0, {}
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        name, _, rhs = item.partition("=")
        name = name.strip()
        if rhs.strip():
            try:
                val = int(rhs.strip(), 0)
            except ValueError:
                pass
        out[name] = val
        val += 1
    return out


def parse_icon_enum():
    """ICON_XXX → 下标, 以及 MAIN_ICON_COUNT。"""
    txt = read(ICON_ENUM)
    cnt = None
    m = re.search(r"#define\s+MAIN_ICON_COUNT\s+(\d+)", txt)
    if m:
        cnt = int(m.group(1))
    body = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*icon_id_t\s*;", txt, re.S)
    out = {}
    if body:
        for item in body.group(1).split(","):
            item = item.strip()
            if not item:
                continue
            name, _, rhs = item.partition("=")
            name = name.strip()
            if rhs.strip().lstrip("-").isdigit():
                out[name] = int(rhs.strip())
    if cnt is None and out:
        cnt = max(out.values()) + 1
    return out, (cnt if cnt is not None else 0)


def parse_app_table():
    """解析 s_builtin_apps[] 的每一行 (C 里是一行一条, 用花括号初始化)。"""
    txt = read(REGISTRY_C)
    m = re.search(r"s_builtin_apps\[\]\s*=\s*\{(.*?)\n\};", txt, re.S)
    if not m:
        raise RuntimeError("apps_registry.c 里找不到 s_builtin_apps[]")
    body = m.group(1)
    apps = []
    for lineno_off, line in enumerate(body.split("\n"), start=1):
        s = re.sub(r"/\*.*?\*/", "", line, flags=re.S)
        s = re.sub(r"//.*$", "", s)
        if "{" not in s or "}" not in s:
            continue
        # 字段: .key=value  (value 可能是 "str" 或 标识符 或 true/false)
        fields = dict(re.findall(r"\.(\w+)\s*=\s*(\"(?:[^\"\\]|\\.)*\"|[A-Za-z_][\w:]*|-?\d+)", s))
        if ".id" not in fields and "id" not in fields:
            continue
        def clean(v):
            if v is None:
                return None
            v = v.strip()
            if v.startswith('"'):
                try:
                    return v[1:-1].encode().decode("unicode_escape", "replace")
                except Exception:
                    return v[1:-1]
            return v
        apps.append({
            "id": clean(fields.get("id")),
            "label": clean(fields.get("label")),
            "page": fields.get("page"),
            "icon": fields.get("icon_idx"),
            "hidden": fields.get("hidden", "false") == "true",
            "cat": clean(fields.get("cat")),
        })
    return apps


# ---------------------------------------------------------------- ② 注册函数
def _strip_comments(text):
    """去注释, 但保留换行数以维持大致行号 (行号仅用于提示)。"""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def find_function_body(text, name):
    """返回函数体 (花括号内文本, 正确处理字符串/字符字面量里的花括号)。"""
    m = re.search(r"(?:^|\n)[^\n;{]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text)
    if not m:
        return None
    i = text.index("{", m.end() - 1)
    depth = 0
    j = i
    n = len(text)
    while j < n:
        c = text[j]
        if c == '"':
            j += 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
        elif c == "'":
            j += 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[i + 1:j], text[:i].count("\n") + 1
        j += 1
    return None


def parse_register_functions():
    """
    返回 {register_func_name: {page_enum, module_var, file, line}}
    一个 register 函数可能注册多个模块 (如 os_page_engine_register_all),
    因此值里用列表: {'pages': [OS_PAGE_X, ...], 'vars': [...]}
    """
    # 先建立 模块变量 → page_id 的映射
    var2page = {}
    for path in walk_c_files(SCAN_DIRS):
        txt = _strip_comments(read(path))
        # 注: 模块定义有两种写法 —— `static const os_module_t x = {` 与 `static os_module_t x = {`
        # (page_fav.c 是后者), const 必须可选, 否则漏解析 → 误报"页面没定义"。
        for m in re.finditer(r"(?:static\s+)?(?:const\s+)?os_module_t\s+(\w+)\s*=\s*\{", txt):
            var = m.group(1)
            start = m.end()
            # 取该初始化块 (到配对的 '}')
            depth, j = 1, start
            while j < len(txt) and depth:
                if txt[j] == "{":
                    depth += 1
                elif txt[j] == "}":
                    depth -= 1
                j += 1
            blk = txt[start:j]
            pm = re.search(r"\.page_id\s*=\s*(OS_PAGE_\w+)", blk)
            nm = re.search(r"\.name\s*=\s*\"([^\"]*)\"", blk)
            var2page[var] = {
                "page": pm.group(1) if pm else None,
                "name": nm.group(1) if nm else var,
                "file": os.path.relpath(path, ROOT),
            }

    regs = {}
    for path in walk_c_files(SCAN_DIRS):
        txt = _strip_comments(read(path))
        for m in re.finditer(r"\b(os_page_\w+_register(?:_all)?)\s*\(void\)\s*\{", txt):
            fname = m.group(1)
            body_info = find_function_body(txt, fname)
            if not body_info:
                continue
            body, line = body_info
            calls = re.findall(r"os_register\s*\(\s*&?\s*(\w+)\s*\)", body)
            # 修掉 O(N^2) 风险: 同一函数只记一次
            entry = regs.setdefault(fname, {
                "vars": [], "pages": [],
                "file": os.path.relpath(path, ROOT), "line": line,
            })
            for v in calls:
                if v in entry["vars"]:
                    continue
                entry["vars"].append(v)
                p = var2page.get(v, {}).get("page")
                if p:
                    entry["pages"].append(p)
    return regs, var2page


# ---------------------------------------------------------------- ③ 调用点
def parse_registry_calls():
    """
    返回 apps_register_all() 里**实际生效**的注册函数名集合,
    以及被 `#if 0` 关掉的注册函数名 (附带行号)。
    """
    raw = read(REGISTRY_C)
    # 只在 apps_register_all 函数体内分析条件编译
    if "apps_register_all" not in raw:
        raise RuntimeError("apps_registry.c 里找不到 apps_register_all()")
    body, base_line = find_function_body(raw, "apps_register_all")

    active, disabled = set(), {}
    cond_stack = []   # 每层: True=生效
    for off, line in enumerate(body.split("\n")):
        lineno = base_line + off
        stripped = line.strip()
        mm = re.match(r"#\s*if\s+(.*)", stripped)
        if mm:
            expr = mm.group(1).strip()
            parent_ok = all(cond_stack) if cond_stack else True
            if expr == "0":
                cond_stack.append(False)
            elif expr == "1":
                cond_stack.append(True)
            else:
                # 未知条件 (如 #ifdef X): 保守视为生效, 并提示
                cond_stack.append(True)
                disabled.setdefault("__unknown_cond__", []).append(lineno)
            continue
        if re.match(r"#\s*else", stripped):
            if cond_stack:
                cond_stack[-1] = not cond_stack[-1]
            continue
        if re.match(r"#\s*el(if|se)", stripped):
            continue
        if re.match(r"#\s*endif", stripped):
            if cond_stack:
                cond_stack.pop()
            continue
        for m in re.finditer(r"\b(os_page_\w+_register(?:_all)?)\s*\(", stripped):
            fn = m.group(1)
            if all(cond_stack) if cond_stack else True:
                active.add(fn)
            else:
                disabled.setdefault(fn, []).append(lineno)
    return active, disabled


# ---------------------------------------------------------------- 主流程
def main():
    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    strict = "--strict" in sys.argv

    try:
        page_enum = parse_os_page_enum()
        icon_enum, icon_count = parse_icon_enum()
        apps = parse_app_table()
        regs, var2page = parse_register_functions()
        active, disabled = parse_registry_calls()
    except Exception as e:
        print("[解析失败] %s" % e)
        return 2

    errors, warns, notes = [], [], []

    if not apps:
        errors.append("应用表解析到 0 条 —— 解析器可能已失效, 请检查 apps_registry.c 的写法")

    # page_enum → 注册函数 (仅在"生效调用"里的)
    page2reg_active = {}
    page2reg_alldef = {}
    for fn, info in regs.items():
        for p in info["pages"]:
            page2reg_alldef.setdefault(p, []).append(fn)
            if fn in active:
                page2reg_active.setdefault(p, []).append(fn)

    # 每个应用的检查
    seen_ids = {}
    seen_pages = collections.defaultdict(list)
    for a in apps:
        aid = a["id"] or "<无 id>"
        page = a["page"] or "<无 page>"

        if a["id"] in seen_ids:
            errors.append("[E] .id 重复: %r (应用表第 %d 条, 首次出现在 %s)"
                          % (aid, apps.index(a) + 1, seen_ids[a["id"]]))
        else:
            seen_ids[a["id"]] = "应用表第 %d 条" % (apps.index(a) + 1)

        if page in page_enum or page.startswith("<"):
            seen_pages[page].append(aid)

        # 页面是否注册?
        if page in page_enum:
            if page in page2reg_active:
                pass  # ✅
            elif page in page2reg_alldef:
                funcs = ", ".join(sorted(set(page2reg_alldef[page])))
                offs = []
                for f in set(page2reg_alldef[page]):
                    offs += disabled.get(f, [])
                where = ("apps_registry.c:%s" % ",".join(map(str, sorted(offs)))) if offs else "apps_register_all() 里没调用"
                errors.append(
                    "[E] 应用 %-10s 的页面 %s 有注册函数 (%s) 但**未生效调用** —— 位置 %s。\n"
                    "        → 真机症状: 应用能看到、点了一点反应都没有。把 #if 0 改成 #if 1 或补上调用。"
                    % (aid, page, funcs, where))
            elif page in ("OS_PAGE_PLACEHOLDER_SECURE", "OS_PAGE_PLACEHOLDER_ALARM"):
                notes.append("[i] 应用 %-10s 指向占位页 %s (开发中, 属预期)" % (aid, page))
            else:
                errors.append(
                    "[E] 应用 %-10s 的页面 %s **没有任何 os_page_*_register() 定义它** —— 点它必打不开。"
                    % (aid, page))

        # 图标越界
        ic = a["icon"]
        if ic and ic in icon_enum:
            if icon_count and icon_enum[ic] >= icon_count:
                errors.append("[E] 应用 %-10s 图标 %s=%d 越界 (MAIN_ICON_COUNT=%d)"
                              % (aid, ic, icon_enum[ic], icon_count))
        elif ic and ic.startswith("ICON_"):
            errors.append("[E] 应用 %-10s 图标 %s 未在 icons_main_enum.h 里定义" % (aid, ic))

    # 同页冲突
    for page, ids in seen_pages.items():
        if len(ids) > 1:
            errors.append("[E] 多个应用指向同一页面 %s: %s" % (page, ", ".join(ids)))

    # 孤儿页 (有注册函数但没应用指向) —— 只对这些"像应用"的页提示
    used = set(a["page"] for a in apps if a["page"])
    for page in sorted(page2reg_alldef):
        if page in used or page not in page_enum:
            continue
        if page in ("OS_PAGE_DIALOG", "OS_PAGE_SPONSOR", "OS_PAGE_MAIN",
                    "OS_PAGE_SETTINGS_DISPLAY", "OS_PAGE_SETTINGS_TIME",
                    "OS_PAGE_SETTINGS_BT", "OS_PAGE_SETTINGS_SD", "OS_PAGE_SETTINGS_INFO",
                    "OS_PAGE_BTINFO", "OS_PAGE_KEY_CONFIG", "OS_PAGE_VOLUME",
                    "OS_PAGE_RETURN_GAME", "OS_PAGE_GAME_SETTINGS",
                    # 容器/演示页: 由代码内部 os_push 打开 (Lua 宿主), 不进应用表
                    "OS_PAGE_LUA"):
            continue   # 系统内部页/子页/容器页, 正常无应用入口
        warns.append("[W] 页面 %s 已注册 (%s) 但没有任何应用指向它 —— 忘了加进应用表?"
                     % (page, ", ".join(sorted(set(page2reg_alldef[page])))))

    if "__unknown_cond__" in disabled:
        warns.append("[W] apps_registry.c 存在无法静态判定的条件编译 (#ifdef 等) 于第 %s 行, "
                     "本工具按'生效'处理, 可能漏报"
                     % ",".join(map(str, disabled["__unknown_cond__"][:8])))

    # ---------------------------------------------------------------- 输出
    print("=" * 68)
    print(" 内置应用注册自检  (静态, 无需编译/烧录)")
    print("=" * 68)
    print("  应用表条目        : %d  (其中 hidden=%d)"
          % (len(apps), sum(1 for a in apps if a["hidden"])))
    print("  可注册页 ({:>2} 个) : {}".format(len(page_enum), "按 os.h 枚举"))
    print("  注册函数定义      : %d" % len(regs))
    print("  实际生效的注册调用: %d" % len(active))
    print("-" * 68)

    if verbose:
        print("\n[明细] 生效的注册调用:")
        for fn in sorted(active):
            pages = ", ".join(sorted(set(regs.get(fn, {}).get("pages", [])))) or "?"
            print("   %-34s -> %s" % (fn, pages))
        if disabled:
            print("\n[明细] 被条件编译关掉的注册调用:")
            for fn, lines in sorted(disabled.items()):
                if fn == "__unknown_cond__":
                    continue
                print("   %-34s @ 行 %s" % (fn, ",".join(map(str, lines))))
        print()

    for w in warns:
        print(w)
    for n in notes:
        print(n)
    for e in errors:
        print(e)

    print("-" * 68)
    if errors:
        print(" ❌ %d 个错误 / %d 个警告 —— 上面带 [E] 的每一条都会导致'应用打不开'" % (len(errors), len(warns)))
    else:
        print(" ✅ 应用表 / 注册函数 / 调用点 三方对账一致 (0 错误, %d 警告)" % len(warns))
    print("=" * 68)

    if errors and strict:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
