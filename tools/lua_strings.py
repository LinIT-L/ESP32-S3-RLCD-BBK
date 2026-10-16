#!/usr/bin/env python3
"""lua_strings.py — 正确提取 Lua 源码里的字符串字面量 (供一致性对照审计用)。

为什么不能直接用正则 `"..."`:
  - Lua 的**长括号串** `[[...]]` / `[==[...]==]` 里可以随便出现 `"`,正则会从里面错位开始匹配,
    产出跨行的垃圾片段, 还会把后续真正要查的串吞掉 → 审计结果全是假"缺失"。
    (实测: logdiag.lua 因为命令链用了 `[==[ ]==]`, 14 条 UI 串全被误报为缺失)
  - 注释里的引号同理。
本模块按 Lua 词法逐个跳过: 行注释 / 长注释 / 长括号串 / 短串转义。
"""
import re


def extract(src, include_long=True):
    """返回源码里所有字符串字面量的值 (按出现顺序, 允许重复)。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        # 行注释
        if c == "-" and src.startswith("--", i):
            # 长注释 --[[ ... ]] / --[==[ ... ]==]
            m = re.match(r"--\[(=*)\[", src[i:])
            if m:
                close = "]" + m.group(1) + "]"
                j = src.find(close, i + m.end())
                i = n if j < 0 else j + len(close)
            else:
                j = src.find("\n", i)
                i = n if j < 0 else j + 1
            continue
        # 长括号串 [[...]] / [==[...]==]
        m = re.match(r"\[(=*)\[", src[i:])
        if m:
            close = "]" + m.group(1) + "]"
            start = i + m.end()
            j = src.find(close, start)
            body = src[start:] if j < 0 else src[start:j]
            if include_long:
                out.append(("LONG", body))
            i = n if j < 0 else j + len(close)
            continue
        # 短串 '...' / "..."
        if c in "\"'":
            j, buf = i + 1, []
            while j < n:
                ch = src[j]
                if ch == "\\" and j + 1 < n:
                    esc = src[j + 1]
                    simple = {"n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b",
                              "f": "\f", "v": "\v", "\\": "\\", '"': '"', "'": "'"}
                    if esc in simple:
                        buf.append(simple[esc]); j += 2; continue
                    if esc == "x":                                  # \xNN
                        hx = src[j + 2:j + 4]
                        try:
                            buf.append(chr(int(hx, 16))); j += 4; continue
                        except ValueError:
                            pass
                    if esc.isdigit():                               # \ddd
                        m2 = re.match(r"\d{1,3}", src[j + 1:])
                        buf.append(chr(int(m2.group(0)) & 0xFF)); j += 1 + len(m2.group(0)); continue
                    buf.append("\\" + esc); j += 2; continue
                if ch == c:
                    break
                buf.append(ch); j += 1
            out.append(("STR", "".join(buf)))
            i = j + 1
            continue
        i += 1
    return out


def values(src, include_long=True):
    return [v for _, v in extract(src, include_long)]


def blank_comments(src):
    """把注释**原地抹成空格**(换行保留), 返回同样长度的字符串。

    用途: 闸门 / 语法扫描前先过一遍 —— 否则注释里的 `app.text (24px 高)`、引号、括号
    都会被正则当成真代码, 产出假报警 (踩过: 注释里写 app.text 被报"参数个数 1")。
    换行与列位置保持不变, 所以行号仍然准确。
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "-" and src.startswith("--", i):
            m = re.match(r"--\[(=*)\[", src[i:])
            if m:                                   # 长注释
                close = "]" + m.group(1) + "]"
                j = src.find(close, i + m.end())
                end = n if j < 0 else j + len(close)
            else:                                   # 行注释 (换行本身不属于注释)
                j = src.find("\n", i)
                end = n if j < 0 else j
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:end]))
            i = end
            continue
        if c == "[":                                # 长括号串: 原样保留
            m = re.match(r"\[(=*)\[", src[i:])
            if m:
                close = "]" + m.group(1) + "]"
                j = src.find(close, i + m.end())
                end = n if j < 0 else j + len(close)
                out.append(src[i:end])
                i = end
                continue
        if c in "\"'":                              # 短串: 原样保留
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == c:
                    j += 1
                    break
                j += 1
            out.append(src[i:j])
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def utf8_fix(t):
    """把 C 源码里 \\xNN 形式的 UTF-8 还原成真字符 (C 侧用)。"""
    b = re.sub(r"\\x([0-9a-fA-F]{2})", lambda m: chr(int(m.group(1), 16)), t)
    try:
        return b.encode("latin1").decode("utf-8")
    except Exception:
        return t


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        s = open(p, encoding="utf-8", errors="replace").read()
        e = extract(s)
        print("%s: %d 个串 (%d 短串 / %d 长串)"
              % (p, len(e), sum(1 for k, _ in e if k == "STR"), sum(1 for k, _ in e if k == "LONG")))
