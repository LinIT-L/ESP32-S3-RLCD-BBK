#!/usr/bin/env python3
"""ui_parity.py — C 版页面 ↔ Lua 版的 **UI 字符串一致性** 自动复核

口径 (linit 2026-09-22 定案): 视觉"差不多"即可, 但 ①菜单选项与规则分类 ②处理逻辑
③点击逻辑 必须完全一致。本工具查的是其中**可自动化的那部分**: C 版页面里的 UI 文案,
Lua 版是不是都有 (漏了 = 菜单/按钮少一条, 属于口径①的硬伤)。

★ 两个提取器都必须按**词法**走, 朴素正则必错 (踩过, 见 docs/应用一致性复核报告.md):
    - Lua 侧: 用 tools/lua_strings.py (跳过 行注释 / 长注释 / 长括号串 / 短串转义)
    - C 侧  : 跳过 /* */ 与 //, 并把 "\\xe6\\x97\\xa5" 还原成 UTF-8
  第一版就靠坏提取器得出"7 个应用 25 处缺失"的错误结论 —— 审计工具的**错报比漏报更危险**。

用法:
    python3 tools/ui_parity.py                 # 复核内置配对表里的全部应用
    python3 tools/ui_parity.py wb wifiprobe    # 只查指定应用
    python3 tools/ui_parity.py wb --dump       # 额外打印 C 版抽到的每条 UI 串
退出码: 0 无缺失; 1 有缺失
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lua_strings import values as lua_values, utf8_fix   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.expanduser("~/Desktop/LinTOS_Lua应用/src")

# 应用名 → C 版页面文件 (相对仓库根)
PAIRS = {
    "calc":      "components/apps/pages/page_calculator.c",
    "morse":     "components/apps/pages/page_morse.c",
    "pomodoro":  "components/apps/pages/page_pomodoro.c",
    "diag":      "components/apps/pages/page_diag.c",
    "logdiag":   "components/apps/pages/page_logdiag.c",
    "term":      "components/apps/pages/page_term.c",
    "winassist": "components/apps/pages/page_winassist.c",
    "wb":        "components/apps/pages/page_wb.c",
    "wifiprobe": "components/apps/pages/page_wifiprobe.c",
    "netdect":   "components/apps/pages/page_netdect.c",
}

# 已逐条查证、**有意不覆盖**的 C 版串 (详见 docs/应用一致性复核报告.md 第二节)。
# 写进来是为了让输出诚实: 它们不是"还没做", 而是"查过了, 决定不这样写"。
KNOWN = {
    # ÷ × − 不在固件字库里 (设备上是空白格) → calc 的 SCI 键位有意改写成 ASCII;
    # 底一2 是 utf8_fix 对 "底数2" 的解码假象 (Lua 里确有 底数2)
    "calc": {"÷", "×", "−", "底一2"},
    # 弹窗顶部不再画"这是什么弹窗"(linit 2026-09-22 定案) → Lua 版不画 C 的列表标题
    # SSH- : C 版靠读 banner 抓 SSH 版本号, Lua 没有裸 socket 读不到 banner → 无法覆盖
    "netdect": {"设备详情", "SSH-",
                # 下面这些是 C 版**自绘的经典键盘**键面文字; Lua 版一律走统一软键盘
                # (app.kbd), 所以键面文案由 kbd_lib 决定, 不再逐字重复。
                "zxcvbnm,.", "-_=+[]{};:'", "小写", "大写", "删除", "&123", "空格", "回车"},
}
# 还没做的应用: 不当失败, 只提示
TODO = set()

_C_STR = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def _unesc(s):
    """还原 C 串里真正影响比对的转义 (\\xNN 由 utf8_fix 处理, 这里处理 \\\\ 与 \\")."""
    s = utf8_fix(s)
    return s.replace('\\\\', '\\').replace('\\"', '"')


def c_strings(path):
    """C 源码里的字符串字面量 (跳过注释与 #include, 已还原 \\xNN / \\\\)。"""
    src = open(path, encoding="utf-8", errors="replace").read()
    out, i, n = [], 0, len(src)
    bol = True                               # 是否在行首 (允许前导空白)
    while i < n:
        c = src[i]
        if c == "\n":
            bol = True
            i += 1
            continue
        if c.isspace():
            i += 1
            continue
        if bol and c == "#":                 # 预处理指令: 跳过整行 (#include "os.h" 不是 UI 串)
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        bol = False
        if c == "/" and i + 1 < n and src[i + 1] == "*":          # 块注释
            j = src.find("*/", i + 2)
            if j < 0:
                i = n
            else:
                i = j + 2
                bol = False
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":           # 行注释
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "'":                                              # 字符字面量: 跳过
            # ⚠ C 里有 `'"'` 这样的字符常量, 里面的 " 会被误当字符串起点, 后面全错位
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'":
                    j += 1
                    break
                j += 1
            i = j
            continue
        if c == '"':                                              # 字符串
            m = _C_STR.match(src, i)
            if m:
                out.append(_unesc(m.group(1)))
                i = m.end()
                continue
        i += 1
    return out


def norm(s):
    """比对前归一: 分隔符 ·(U+00B7) 与 ・(U+30FB) 视为同一个字。

    原因: U+00B7 **不在固件字库**里 (设备上是空白格), 所以 C 版报告里那一堆 "· xxx" 在设备上
    的圆点是**看不见**的; Lua 版换成字库里有的 ・(U+30FB) 才真能显示。这属于"字形替换",
    不是内容差异, 比对时归一。同理 '—'(U+2014, 字库里有) 保持原样。"""
    return s.replace("\u00b7", "\u30fb")


def is_ui(s):
    """过滤非 UI 串: 路径 / 格式串 / 宏名 / 日志 / 纯标识符 / 太短或太长。"""
    if not s or not s.strip():
        return False
    if s.startswith("/") or s.startswith("%"):
        return False
    if "/" in s and " " not in s:            # 路径 / MIME
        return False
    if "%" in s:                             # printf 格式串
        return False
    if re.fullmatch(r"(\\[A-Za-z0-9]+)+", s):   # 纯转义序列 (如 "\n"), 不是文案
        return False
    if re.fullmatch(r"[A-Za-z0-9_]+", s):    # 纯标识符 / TAG / 键名
        return False
    if re.fullmatch(r'"[^"]*"', s):          # JSON 键等带引号的名字 (如 "location")
        return False
    if "\\n" in s:                           # 导出到文件用的整段文本 (带换行转义), 不是屏幕文案
        return False
    if re.fullmatch(r"[A-Z_]+", s):          # 宏名
        return False
    if len(s) > 60:                          # 整段日志
        return False
    return True


def check(name, dump=False):
    cpath = os.path.join(ROOT, PAIRS[name])
    lpath = os.path.join(SRC_DIR, name + ".lua")
    if not os.path.exists(cpath):
        return None, "找不到 C 版: %s" % cpath
    if not os.path.exists(lpath):
        return None, "找不到 Lua 版: %s" % lpath
    cset = [norm(s) for s in c_strings(cpath) if is_ui(s)]
    lset = set(norm(s) for s in lua_values(open(lpath, encoding="utf-8").read()))
    # C 侧的 UI 串在 Lua 里应当是**子串**关系 (Lua 常做拼接: "信道 " .. n)
    ljoin = "\n".join(lset)
    known = KNOWN.get(name, set())
    missing = [s for s in cset if s not in lset and s not in ljoin and s not in known]
    if dump:
        print("    C 版抽到 %d 条 UI 串:" % len(cset))
        for s in cset:
            mark = "  " if (s in lset or s in ljoin) else ("~ " if s in known else "✗ ")
            print("      %s%s" % (mark, s))
    return (len(cset), missing), None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    dump = "--dump" in sys.argv
    names = args or list(PAIRS)
    print("══ UI 串一致性 (C 版 ↔ Lua 版) ══")
    bad = 0
    for n in names:
        if n not in PAIRS:
            print("  ? %s 不在配对表里" % n)
            continue
        r, err = check(n, dump)
        if err:
            if n in TODO:
                print("  ⏳ %-11s 还没做 (%s)" % (n, os.path.basename(PAIRS[n])))
            else:
                print("  ✗ %-11s %s" % (n, err))
                bad += 1
            continue
        total, missing = r
        if missing:
            bad += 1
            print("  ✗ %-11s %d 条中缺 %d 条:" % (n, total, len(missing)))
            for s in missing:
                print("        · %s" % s)
        else:
            print("  ✅ %-10s %d 条全部覆盖" % (n, total))
    print("\n" + ("✅ 无缺失" if not bad else "❌ %d 个应用有缺失" % bad))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
