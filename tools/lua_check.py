#!/usr/bin/env python3
"""lua_check.py — Lua 单文件程序 **语法 + 契约** 闸门 (纯 Python, 无需 lua 二进制)

三道检查 (都是踩过的坑, 每条都对应一次真实事故):
  1. 语法: 用 luaparser 真解析 (不是数括号)
  2. 契约可见性: render/poll/action/on_enter/on_exit 必须是**全局**函数。
     写成 `local function render()` 桥侧根本看不到 → 界面全白、无报错, 极难查。
  3. 颜色参数: app.px/fill/rect/hline/vline 末位是**颜色** (app.BLACK=0 / app.WHITE=1),
     固件用 luaL_checkinteger 取它 —— 传 true/false 在设备上直接 Lua error,
     少传一个参数同样报错。

用法:
    python3 tools/lua_check.py              # 校验 apps/lua/ 全部 .lua
    python3 tools/lua_check.py a.lua b.lua  # 校验指定文件
    python3 tools/lua_check.py -v           # 额外打印每个文件的函数定义/回调
退出码: 0 全通过; 1 有语法/契约错误
"""
import glob
import os
import re
import sys

try:
    from luaparser import ast
except ImportError:
    print("缺少 luaparser: pip install luaparser")
    sys.exit(2)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lua_strings import blank_comments     # noqa: E402  (剥注释, 等长, 行号不变)
try:
    from fw_fonts import FwFonts
    _FW = FwFonts()
except Exception as _e:            # 字模缺失时不阻断语法检查, 只跳过覆盖率检查
    _FW = None
    _FW_ERR = str(_e)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LUA_DIR = os.path.join(ROOT, "apps", "lua")
CONTRACT = ("on_enter", "on_exit", "render", "action", "poll")


def bridge_api_names():
    """直接从 page_lua.c 抽桥注册的全部字段名 (顶层 + wifi/net/fs/nvs/db/hid/kbd 子表)。

    用途: 抓 app.xxx 的**拼写错误**。拼错在设备上表现为 "attempt to call a nil value" 或者
    静默不画 —— 纯语法检查抓不到。白名单从源码实时抽, 不用手工维护 (桥加了新 API 自动生效)。
    抽到的名字太少 (<50) 认为解析失败 → 返回 None 跳过, 防止误报一片。
    """
    path = os.path.join(ROOT, "components", "apps", "pages", "page_lua.c")
    try:
        src = open(path, encoding="utf-8").read()
    except OSError:
        return None
    names = set(re.findall(r'lua_setfield\(L,\s*-2,\s*"([^"]+)"\)', src))
    return names if len(names) >= 50 else None


_API_NAMES = bridge_api_names()

# 绘制 API → (最少参数, 最多参数)。★ app.textc 只有 (y, str[, inv]) —— 水平居中按**屏宽**算,
#   想"按给定 cx 居中"必须用 app.mixc。(曾把 textc 当 (cx,y,s) 用 → 每格把 y 坐标当字符串画出来)
ARITY = {
    "clear": (0, 1),
    "px": (3, 3), "fill": (5, 5), "rect": (5, 5), "store_rect": (5, 5),
    "hline": (4, 4), "vline": (4, 4),
    "text": (3, 4), "texts": (3, 4), "textm": (3, 4), "textsb": (3, 4),
    "zh": (3, 4), "zhs": (3, 4), "zhsm": (3, 4), "zhsb": (3, 4), "zh24": (3, 4),
    "mix": (3, 4), "mixc": (3, 4),
    "textw": (1, 1), "mixw": (1, 1),
    "big": (3, 5), "bigc": (3, 5),
    "bmp": (5, 5),
    "textc": (2, 3),
}
_ARITY_RE = re.compile(r"app\.(%s)\s*\(" % "|".join(ARITY))
# app.kbd.* (软键盘 kbd_lib 统一入口) 的参数个数
KBD_ARITY = {"use": (1, 5), "reset": (0, 0), "draw": (0, 1), "hit": (2, 2), "geom": (0, 0),
             "height": (0, 0), "count": (0, 0), "press": (1, 1), "char": (1, 1), "label": (1, 1),
             "layer_on": (1, 1), "get": (0, 1), "set": (2, 2)}
_KBD_RE = re.compile(r"app\.kbd\.(%s)\s*\(" % "|".join(KBD_ARITY))
# 末位是"颜色"的 API (0=BLACK 画黑 / 1=WHITE 画白)
COLOR_ARGS = {"px", "fill", "rect", "store_rect", "hline", "vline"}
# 纯 ASCII 字体 (传汉字只会得到一连串 '?')
ASCII_ONLY = ("texts", "textm", "textsb")
# 汉字专用字体 (每字固定占位, ASCII 落在里面是空白格)
CJK_ONLY = ("zh", "zhs", "zhsm", "zhsb", "zh24")
_FONT_RE = re.compile(r"app\.(%s)\s*\(" % "|".join(ASCII_ONLY + CJK_ONLY))
# 所有会画字符串的 API (做字库覆盖率检查用)
_TEXT_RE = re.compile(r"app\.(%s)\s*\(" % "|".join(ASCII_ONLY + CJK_ONLY + ("text", "mix", "mixc")))
_STR_LIT = re.compile(r"""^\s*(['"])(.*)\1\s*$""", re.S)


def _line_no(src, pos):
    return src[:pos].count("\n") + 1


def _split_args(src, i):
    """从 src[i] (左括号之后) 开始, 按括号配平切出顶层实参; 返回 (args, 结束位置)。"""
    depth, args, cur = 1, [], ""
    while i < len(src) and depth:
        ch = src[i]
        if ch in "\"'":
            q = ch
            cur += ch
            i += 1
            while i < len(src) and src[i] != q:
                if src[i] == "\\":
                    cur += src[i]
                    i += 1
                cur += src[i]
                i += 1
            if i < len(src):
                cur += src[i]
        elif ch == "(":
            depth += 1
            cur += ch
        elif ch == ")":
            depth -= 1
            if depth == 0:
                # 空实参表 f() 就是 0 个参数 —— 不能把 "" 当成一个参数,
                # 否则 "参数个数 1, 应为 0" 这类假报警会淹没真问题。
                if cur.strip() or args:
                    args.append(cur)
                return args, i
            cur += ch
        elif ch == "," and depth == 1:
            args.append(cur)
            cur = ""
        else:
            cur += ch
        i += 1
    return None, i


def contract_findings(src):
    """返回契约问题列表 (语法之外)。"""
    out = []
    # ---- 1) 契约函数必须是全局 ----
    for name in CONTRACT:
        if re.search(r"(^|[^\w.])local\s+function\s+%s\b" % name, src) or \
           re.search(r"^\s*local\s+%s\s*=" % name, src, re.M):
            out.append("契约函数 %s 写成了 local → 桥看不到它 (界面空白且无报错); "
                       "必须写成全局 `function %s(...)`" % (name, name))
    if not re.search(r"^\s*function\s+render\s*\(", src, re.M):
        out.append("缺少全局 render() —— 桥靠它绘制界面")
    # ---- 2) 参数个数 + 颜色参数 ----
    # 颜色参数只在 px/fill/rect/hline/vline 上; 其余 API 只查个数。
    for m in _ARITY_RE.finditer(src):
        name = m.group(1)
        args, _ = _split_args(src, m.end())
        if args is None:
            continue
        ln = _line_no(src, m.start())
        lo, hi = ARITY[name]
        n = len(args)
        if n < lo or n > hi:
            hint = ""
            if name == "textc":
                hint = (" —— app.textc 只有 (y, str[, inv]), 水平居中按**屏宽**算; "
                        "要按给定 cx 居中请用 app.mixc(cx, y, str)")
            out.append("L%d app.%s() 参数个数 %d, 应为 %s%s"
                       % (ln, name, n, ("%d" % lo) if lo == hi else ("%d~%d" % (lo, hi)), hint))
            continue
        # app.textc 的陷阱签名: 按 (cx, y, str) 调用时, 第 3 个参数是**字符串字面量**
        if name == "textc" and n == 3 and _STR_LIT.match(args[-1]):
            out.append("L%d app.textc() 第 3 个参数是字符串 → 按 (cx, y, str) 调用了; "
                       "固件签名是 (y, str[, inv]) 且居中按**屏宽**算。"
                       "要按给定 cx 居中请改用 app.mixc(cx, y, str)" % ln)
            continue
        if name in COLOR_ARGS and name != "textc":
            last = args[-1].strip()
            if re.fullmatch(r"true|false|not\s+[\w.]+", last):
                out.append("L%d app.%s() 颜色参数传了布尔值 `%s` → 固件是 luaL_checkinteger, "
                           "设备上必报错; 改成 app.BLACK / app.WHITE" % (ln, name, last))
    # ---- 2b) app.kbd.* 参数个数 ----
    for m in _KBD_RE.finditer(src):
        name = m.group(1)
        args, _ = _split_args(src, m.end())
        if args is None:
            continue
        lo, hi = KBD_ARITY[name]
        n = len(args)
        if n < lo or n > hi:
            hint = ""
            if name == "use":
                hint = (" —— 用法: use('std'[,y]) / use('edit'[,y]) / use('rows',n[,y]) / "
                        "use('dialog',x,y,w,row_h)")
            elif name == "hit":
                hint = " —— 用法: hit(tx, ty) —— **先 x 后 y**"
            out.append("L%d app.kbd.%s() 参数个数 %d, 应为 %s%s"
                       % (_line_no(src, m.start()), name, n,
                          ("%d" % lo) if lo == hi else ("%d~%d" % (lo, hi)), hint))
    # ---- 3) 字体选错: 汉字送进 ASCII 字体会变 ???; 纯 ASCII 送进汉字字体是空白格 ----
    for m in _FONT_RE.finditer(src):
        name = m.group(1)
        args, _ = _split_args(src, m.end())
        if args is None or len(args) < 3:
            continue
        lit = _STR_LIT.match(args[2])
        if not lit:
            continue
        text = lit.group(2)
        has_cjk = any(ord(c) > 0x7F for c in text)
        ln = _line_no(src, m.start())
        if name in ASCII_ONLY and has_cjk:
            out.append("L%d app.%s(\"%s\") 是**纯 ASCII 字体**, 汉字会画成一串 '?'; "
                       "混排请用 app.mix / app.mixc" % (ln, name, text))
        elif name in CJK_ONLY and text and not has_cjk:
            out.append("L%d app.%s(\"%s\") 是**汉字专用字体**, ASCII 会画成等宽空白格; "
                       "混排请用 app.mix, 纯 ASCII 用 app.texts / app.text" % (ln, name, text))
    # ---- 4) 字库覆盖率: 字符不在固件字库里 → 设备上整字空白 (踩过: ÷ × − 这类 2 字节符号) ----
    if _FW is not None:
        avail = _FW.chars16() | _FW.chars24()
        for m in _TEXT_RE.finditer(src):
            args, _ = _split_args(src, m.end())
            if args is None or len(args) < 3:
                continue
            lit = _STR_LIT.match(args[2])
            if not lit:
                continue
            body = lit.group(2)
            bad = []
            for ch in body:
                if ord(ch) > 0x7F and ch not in avail and ch not in bad:
                    bad.append(ch)
            if bad:
                ln = _line_no(src, m.start())
                out.append("L%d app.%s(\"%s\") 含固件字库里**没有**的字符: %s "
                           "→ 设备上这些字是空白格 (整字不显示); 请换 ASCII 或换字库里有的字"
                           % (ln, m.group(1), body,
                              " ".join("%s(U+%04X)" % (c, ord(c)) for c in bad[:6])))
    # ---- 5) app.* API 名拼写 (白名单实时从 page_lua.c 抽, 见 bridge_api_names) ----
    if _API_NAMES:
        for m in re.finditer(r"(?<![\w.])app\.([A-Za-z_]\w*)\s*\(", src):
            n = m.group(1)
            if n not in _API_NAMES:
                out.append("L%d app.%s() 不是固件桥暴露的 API (拼错了? 桥里没有这个名字) —— "
                           "设备上会是 attempt to call a nil value"
                           % (_line_no(src, m.start()), n))
        for m in re.finditer(r"(?<![\w.])app\.([A-Za-z_]\w*)\.([A-Za-z_]\w*)\s*\(", src):
            grp, n = m.group(1), m.group(2)
            if grp in _API_NAMES and n not in _API_NAMES:
                out.append("L%d app.%s.%s() 不是固件桥暴露的 API (拼错了? 桥里没有这个名字)"
                           % (_line_no(src, m.start()), grp, n))
    # ---- 6) app.fs.listdir 的返回是**数组** (page_lua.c 的 lua_fs_listdir 用 lua_rawseti,
    #      1..n = 路径名), 用 pairs() 迭代拿到的是**下标**不是文件名 ----
    for m in re.finditer(r"local\s+(\w+)\s*=\s*app\.fs\.listdir\s*\(", src):
        var = m.group(1)
        if re.search(r"\bpairs\s*\(\s*%s\s*\)" % re.escape(var), src):
            out.append("L%d app.fs.listdir 的返回值是**数组** (1..n 的路径名), "
                       "`pairs(%s)` 拿到的是**下标** 1/2/3… → 目录会列成 \"1 2 3\"; "
                       "请用 `for i = 1, #%s do local name = %s[i]`"
                       % (_line_no(src, m.start()), var, var, var))
    return out


# 短字符串字面量 (含转义), 用于"字库覆盖率"全量扫描
_LIT_RE = re.compile(r'"((?:[^"\\]|\\.)*)"' + r"|'((?:[^'\\]|\\.)*)'")


def glyph_findings(src):
    """扫描**所有**字符串字面量里"固件字库没有"的字符。

    为什么不能只看 app.xxx("字面量") 的实参: 键表/菜单项的值往往先存进表,
    再经变量传给 app.mix —— 于是 ÷ × − 这类字符绕过了实参检查, 到设备上整字空白。
    (实测: calc 的 ÷ × − 三键全空白)
    个别非显示用途的串可用行尾注释 `-- font-ok` 豁免。
    """
    if _FW is None:
        return []
    avail = _FW.chars16() | _FW.chars24()
    out = []
    for i, ln in enumerate(src.split("\n"), 1):
        if ln.lstrip().startswith("--") or "font-ok" in ln:
            continue
        for m in _LIT_RE.finditer(ln):
            body = m.group(1) if m.group(1) is not None else m.group(2)
            if body is None:
                continue
            bad = []
            for ch in body:
                if ord(ch) > 0x7F and ch not in avail and ch not in bad:
                    bad.append(ch)
            if bad:
                out.append("L%d 字面量 \"%s\" 含固件字库没有的字符: %s → 设备上是空白格; "
                           "换 ASCII 或换字库里有的字 (确非显示用可加 `-- font-ok`)"
                           % (i, body, " ".join("%s(U+%04X)" % (c, ord(c)) for c in bad[:6])))
    return out


def check(path, verbose=False):
    src = open(path, encoding="utf-8").read()
    name = os.path.basename(path)
    try:
        ast.parse(src)
    except Exception as e:
        msg = str(e).replace("\n", " ")[:160]
        print("  ✗ %-22s 语法: %s" % (name, msg))
        return False
    # ⚠ 正则类检查一律跑在**剥掉注释**的源码上 (等长, 行号不变)。
    #   否则注释里写 `app.text (24px 高)` 这种说明文字会被当成真调用 → 假报警。
    code = blank_comments(src)
    # ★ 2026-10-12: `_*.lua` = 公共库 (项目约定: 放 apps/ 根, 不进应用列表),
    #   库本就不该有 render/poll/action ⇒ 只做语法 + 字库检查, 跳过契约检查。
    #   (否则每加一个公共库都要在闸门里报"缺少全局 render()")
    _is_lib = os.path.basename(path).startswith("_")
    problems = ([] if _is_lib else contract_findings(code)) + glyph_findings(code)
    if problems:
        print("  ✗ %-22s 契约: %d 个问题" % (name, len(problems)))
        for p in problems:
            print("      · " + p)
        return False
    info = ""
    if verbose:
        fns = [n.name for n in ast.walk(ast.parse(src))
               if n.__class__.__name__ == "Function" and getattr(n, "name", None)]
        top = [f for f in CONTRACT if f in fns]
        info = "  回调: %s" % (", ".join(top) if top else "无")
    print("  ✓ %-22s %4d 行%s" % (name, src.count("\n") + 1, info))
    return True


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    verbose = "-v" in sys.argv
    # 递归扫: 应用实际放在 apps/lua/{app,ops}/*.lua (分类子目录), 早先的非递归 glob 会漏掉全部
    files = args or sorted(glob.glob(os.path.join(LUA_DIR, "**", "*.lua"), recursive=True))
    if not files:
        print("没有找到 .lua 文件 (%s)" % LUA_DIR)
        return 0
    print("Lua 语法 + 契约校验 (%d 个文件)" % len(files))
    # ⚠ 不能用 all(生成器): 会在第一个失败处短路, 后面的文件根本不检查, 看不到全部问题
    ok = True
    for f in files:
        if not check(f, verbose):
            ok = False
    print("\n" + ("✅ 全部通过" if ok else "❌ 存在问题"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
