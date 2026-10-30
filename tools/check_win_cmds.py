#!/usr/bin/env python3
"""check_win_cmds.py — Windows 注入命令静态体检 (2026-10-30)

为什么需要它:
  这些命令是**用 HID 键盘一个字符一个字符打出去**的, 编译器只能看 C 字符串,
  看不到下面这些致命问题:
    · 命令里混进非 ASCII (HID 映射表里没有中文/中文标点) → 打出来是缺字, 命令跑到一半;
    · 双引号少一个 → cmd/PowerShell 会等下一行, 把后续命令全吃进去;
    · PowerShell 行里出现双引号 → 会把 cmd 那层 `-c "..."` 提前闭合;
    · %VAR% 写错 → cmd 原样输出 %VAR%, 重定向路径变字面量;
    · wmic 回来了 → Win11 24H2 上整条链一个文件都不产出。

检查对象 (只读源码, 不执行任何 Windows 命令):
  components/apps/pages/page_logdiag.c    采集链 / 回传链 / 链首
  components/apps/pages/page_winassist.c  40 条快捷命令

用法:  python3 tools/check_win_cmds.py [-q]
退出码: 0 = 全部通过, 1 = 有 P0/P1 问题
"""
import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 这些前缀开头的算"要打到 Windows 上的一条命令"
CMD_PROGS = (
    "cmd", "powershell", "wmic", "wevtutil", "driverquery", "ipconfig",
    "systeminfo", "netsh", "netstat", "schtasks", "reg ", "net ", "arp ",
    "route ", "type ", "dir ", "md ", "echo ", "set ", "exit", "for ", "cd ",
    # winassist 的 40 条 (GUI/控制台程序名)
    "control", "devmgmt.msc", "msinfo32", "regedit", "cleanmgr", "diskmgmt.msc",
    "powercfg", "devices.msc", "mmsys.cpl", "main.cpl", "ncpa.cpl", "firewall.cpl",
    "wf.msc", "ms-settings:", "msdt ", "eventvwr.msc", "msconfig", "rstrui",
    "chkdsk", "services.msc", "appwiz.cpl", "osk", "magnify", "mfc", "dxdiag",
    "perfmon", "mdsched", "bootrec", "sfc ",
)

# HID 键盘能打出来的字符 (与 page_logdiag.c ld_char_hid / page_winassist.c wa_char_hid 同口径)
HID_KEYS = set("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
               " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~")

P0, P1, INFO = "P0", "P1", "info"


def _parse_fragment(src, i):
    """从 src[i]=='"' 开始解析一个片段。返回 (文本, 结束下标, 是否闭合)"""
    n = len(src)
    j = i + 1
    buf = []
    while j < n:
        d = src[j]
        if d == "\\":
            e = src[j + 1] if j + 1 < n else ""
            if e == "n":
                buf.append("\n")
            elif e == "t":
                buf.append("\t")
            elif e == "r":
                buf.append("\r")
            elif e == "\\":
                buf.append("\\")
            elif e == '"':
                buf.append('"')
            elif e == "x":
                try:
                    buf.append(chr(int(src[j + 2:j + 4], 16)))
                except ValueError:
                    buf.append("?")
                j += 2
            else:
                buf.append(e)
            j += 2
            continue
        if d == '"':
            return "".join(buf), j + 1, True
        if d == "\n":
            break
        buf.append(d)
        j += 1
    return "".join(buf), j, False


def c_literals(path):
    """提取 C 文件里的字符串字面量。返回 [(行号, 文本)]"""
    return c_literals_text(open(path, encoding="utf-8").read())


def c_literals_text(src):
    """提取 C 源码文本里的字符串字面量 (跳过注释, 处理转义, **把相邻字面量拼成一条**)。
    返回 [(行号, 文本)]"""
    n = len(src)
    i = 0
    out = []
    while i < n:
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = (j + 2) if j >= 0 else n
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = j if j >= 0 else n
            continue
        if src[i] == '"':
            start_line = src.count("\n", 0, i) + 1
            parts, ok = [], False
            while True:
                text, i, closed = _parse_fragment(src, i)
                parts.append(text)
                if not closed:
                    break
                ok = True
                # 跳到下一个字面量: 中间只允许空白/**注释** (C 允许注释夹在拼接的字面量之间)
                k = i
                while k < n:
                    if src[k] in " \t\r\n":
                        k += 1
                    elif src.startswith("/*", k):
                        e = src.find("*/", k + 2)
                        k = (e + 2) if e >= 0 else n
                    else:
                        break
                if k < n and src[k] == '"':
                    i = k              # 紧邻下一个字面量 → C 会拼接, 继续
                    continue
                i = k
                break
            if ok:
                out.append((start_line, "".join(parts)))
            continue
        i += 1
    return out


def winassist_commands(path):
    """从 cat_items 表里抠出 { "标签", KT_CMD, "命令" } 的命令字段"""
    src = open(path, encoding="utf-8").read()
    res = []
    for m in re.finditer(r'\{\s*"([^"]*)"\s*,\s*KT_CMD\s*,\s*"([^"]*)"\s*\}', src):
        line = src[:m.start()].count("\n") + 1
        res.append((line, m.group(2), m.group(1)))
    return res


def check_cmd(text, where, problems, stats):
    """对一条命令做静态体检"""
    stats.append((where, len(text)))

    # C1 只能 ASCII 可打印 (HID 表里没有中文/中文标点)
    bad = [(k, ch) for k, ch in enumerate(text) if not (0x20 <= ord(ch) <= 0x7E)]
    if bad:
        problems.append((P0, where, "含非 ASCII 字符: %r (位置 %d)" % (bad[0][1], bad[0][0])))

    # C2 每个字符都必须在 HID 映射表里
    miss = [ch for ch in text if ch not in HID_KEYS]
    if miss:
        problems.append((P0, where, "HID 映射表打不出来的字符: %r" % sorted(set(miss))))

    # C3 双引号必须偶数 (否则 cmd 会吞掉后续命令)
    if text.count('"') % 2 != 0:
        problems.append((P0, where, "双引号不成对 (共 %d 个)" % text.count('"')))

    # C4 PowerShell: 外层只允许一对双引号 (-c "..."), 内部必须用单引号
    low = text.lower()
    if low.startswith("powershell"):
        if text.count('"') != 2:
            problems.append((P0, where, "PowerShell 行有 %d 个双引号 (应为 2: -c \"...\")"
                             % text.count('"')))
        if text.count("'") % 2 != 0:
            problems.append((P0, where, "单引号不成对 (共 %d 个)" % text.count("'")))
        if text.count("(") != text.count(")"):
            problems.append((P0, where, "圆括号不配对: %d 开 / %d 闭"
                             % (text.count("("), text.count(")"))))
        if text.count("{") != text.count("}"):
            problems.append((P1, where, "花括号不配对"))

    # C5 cmd /k 必须是 "cmd /k \"...\"" 两条引号包住整段
    if low.startswith('cmd /k "'):
        if not text.endswith('"'):
            problems.append((P0, where, "cmd /k 的引号没有闭合在行尾"))

    # C6 %VAR% 语法 (只对 cmd 命令; PowerShell 里 % 是 ForEach-Object 的别名)
    if not low.startswith("powershell"):
        # for 循环变量 %i 不是环境变量: 先把它剔掉再检查, 否则会被当成坏变量
        chk = text
        fm = re.match(r"^for %([A-Za-z]) in ", chk)
        if fm:
            chk = chk.replace("%" + fm.group(1), "")
        if fm and not re.search(r" do ", chk):
            problems.append((P1, where, "for 循环缺 do"))
        for m in re.finditer(r"%([^%]*)%", chk):
            name = m.group(1)
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                problems.append((P1, where, "%%VAR%% 写法可疑: %%%s%%" % name))
        if chk.count("%") % 2 != 0:
            problems.append((P0, where, "% 号不成对 → 后面的变量展开会错位"))

    # C7 PowerShell 行里不允许把用户路径塞进字符串字面量:
    #    路径中的 ' 会提前闭合字符串 (P0, 2026-10-30 实际踩过)
    if low.startswith("powershell"):
        if "%userprofile%" in low or re.search(r"'[a-z]:\\", low):
            problems.append((P0, where, "PowerShell 行内联了绝对路径/%%USERPROFILE%% "
                                        "⇒ 用户名带单引号时会语法错; 应改用相对文件名"))

    # C8 WMIC 已弃用 (Win11 24H2 默认没有)
    if re.search(r"\bwmic\b", low):
        problems.append((P0, where, "仍在使用 wmic (Win11 24H2 起默认不可用)"))


CHAIN_ARRAYS = [("s_c_sys", 0, "Sys"), ("s_c_log", 1, "Log"),
                ("s_c_net", 2, "Net"), ("s_c_acc", 3, "Acc")]


def chain_source(src, name):
    """取出某个采集链数组的源码切片"""
    m = re.search(r"static const char \*const %s\[\] = \{(.*?)\n\};" % name, src, re.S)
    return m.group(1) if m else ""


def written_files(cmd):
    """从一条命令里抠出它写到哪些 .txt: cmd 重定向 `> X.txt` 与 PS 的 Out-File"""
    got = set()
    for m in re.finditer(r">\s*\"?([A-Za-z0-9_.\\]+\.txt)\"?", cmd):
        got.add(m.group(1).split("\\")[-1])
    for m in re.finditer(r"Out-File[^|;]*-Encoding \w+ ([A-Za-z0-9_.\\]+\.txt)", cmd):
        got.add(m.group(1).split("\\")[-1])
    return got


def expected_files():
    """从 diag_core.c 里读"应有文件清单" (与设备侧判定同源)"""
    src = open(os.path.join(ROOT, "components/apps/pages/diag_core.c"), encoding="utf-8").read()
    out = {}
    for arr, idx, _cat in CHAIN_ARRAYS:
        names = ("s_f_" + arr.split("_")[-1], idx)
        m = re.search(r"static const char \*const %s\[\] = \{(.*?)\};" % names[0], src, re.S)
        out[idx] = re.findall(r'"([^"]+\.txt)"', m.group(1)) if m else []
    return out


# 模拟的用户目录: 空格 / 中文 / 单引号 / & —— 真实世界里都存在
PROFILES = [
    r"C:\Users\linit",
    r"C:\Users\John Smith",
    r"C:\Users\张三",                    # 中文用户名
    r"C:\Users\O'Brien",
    r"C:\Users\a&b",
]


def sim_paths(page_logdiag_src, problems):
    """把 %USERPROFILE% 换成各种"难缠"的真实路径, 验证命令不会被拆开。

    规则 (与 cmd 的解析一致):
      · 路径**必须整体在双引号里** —— 引号外的空格/& 会切分命令;
      · 双引号内 cmd 不做转义, 所以路径里的 ' & 空格 中文 都安全;
      · PowerShell 行**不得出现任何用户路径** (这是上一轮的 P0 根因)。
    """
    for prof in PROFILES:
        for line in ("for %i in (Sys Log Net Acc) do @md \"%USERPROFILE%\\Desktop\\Logs\\%i\" 2>nul",
                     "cd /d \"%USERPROFILE%\\Desktop\\Logs\\Sys\""):
            cmd = line.replace("%USERPROFILE%", prof)
            if cmd.count('"') != 2:
                problems.append((P0, "sim", "路径含 %r 时引号数 != 2: %s" % (prof, cmd)))
                continue
            inner = cmd.split('"')[1]
            if inner.count("%") == 0 and cmd.startswith("cd") and prof not in inner:
                problems.append((P0, "sim", "cd 路径被美元符号/百分号吃掉: %s" % cmd))
            # 末尾反斜杠后面紧跟引号: cmd 无所谓, 但要保证不是 "\\" 结尾造成的歧义
            if inner.endswith('\\'):
                problems.append((P1, "sim", "路径以反斜杠结尾, 拼接时容易出歧义: %s" % cmd))

    # PowerShell 行里只要没有用户路径, 用户名再怪也影响不到它
    for _, cmd in c_literals_text(page_logdiag_src):
        low = cmd.lower()
        if low.startswith("powershell") and "%userprofile%" in low:
            problems.append((P0, "sim", "PowerShell 行仍含 %%USERPROFILE%%: " + cmd[:60]))



def macro_literal(src, name):
    """取出 `#define NAME "..."` 的字面量 (单行宏), 用来在切片里做文本展开。"""
    # 这两个宏都是**单行**定义, 用"行内最外层的一对引号"抓即可 (不需要处理跨行拼接)
    m = re.search(r'#define\s+%s\s+("[^\n]*")' % re.escape(name), src)
    return m.group(1) if m else ""


def cwd_model_check(ld_src, problems):
    """采集链的工作目录模型检查 (2026-10-30 第三轮)。

    真实执行关系 (代码事实, 对应 ld_run_chain()):
        每条链的 index 0 → ld_win_cmd() → Win+R 打字 + Enter   ⇒ **新起一个 cmd 进程**
        index 1..n-1     → ld_send_line() → 在当前焦点窗口打字 + Enter ⇒ 同一个 cmd 进程
    所以: 每条链**恰好一次** Win+R, 之后 cd 与所有相对文件名的 `>` 都在同一个进程里,
    cd 持续有效; 换链 = 换进程 = 重新 cd。这里把这三条钉成可执行检查:
      ① 每条链恰好一条以 "cmd" 开头的命令 (且必须是第 0 条);
      ② 链里必须有一条 `cd /d "..."`;
      ③ 所有 `> xxx.txt` 的目标必须是**裸文件名** (不能带路径/环境变量) ——
         一旦有人改回绝对路径, 用户名带空格/引号就会重新炸, 而且字符数暴涨。
    """
    head = macro_literal(ld_src, "CHAIN_HEAD")
    dirs = macro_literal(ld_src, "DIRS_CMD")
    for arr, _idx, cat in CHAIN_ARRAYS:
        body = chain_source(ld_src, arr).replace("CHAIN_HEAD", head).replace("DIRS_CMD", dirs)
        cmds = [c for _, c in c_literals_text(body)]
        if not cmds:
            problems.append((P0, "cwd:%s" % cat, "没解析出命令 (数组被切碎?)"))
            continue
        wins = [i for i, c in enumerate(cmds) if c.strip().lower().startswith("cmd")]
        if len(wins) != 1:
            problems.append((P0, "cwd:%s" % cat,
                             "Win+R 起 cmd 的次数为 %d (应恰好 1 次, 否则每条命令都是新进程, cd 作废)"
                             % len(wins)))
        elif wins[0] != 0:
            problems.append((P0, "cwd:%s" % cat, "起 cmd 的命令不在第 0 条 (实际第 %d 条)" % wins[0]))
        if not any(c.lower().startswith("cd /d ") for c in cmds):
            problems.append((P0, "cwd:%s" % cat, "缺少 `cd /d` —— 相对文件名会落到别的目录"))
        for c in cmds:
            for tgt in re.findall(r"(?<!2)>\s*(\S+?)?\.txt", c):
                if tgt and ("\\" in tgt or "%" in tgt):
                    problems.append((P0, "cwd:%s" % cat,
                                     "重定向目标不是裸文件名 (%s) ⇒ 与 cd 模型冲突" % tgt))
        # rc.txt 要带真实工作目录: 万一 cd 没生效, 这是唯一能看出来的证据
        if not any("DIR=%CD%" in c for c in cmds):
            problems.append((P1, "cwd:%s" % cat, "rc.txt 未记录 %CD% (cd 是否生效无从查证)"))


def send_script_check(ld_src, problems):
    """回传脚本的硬性要求 (每条都对应一次真实事故或一个必须守住的语义)。"""
    body = chain_source(ld_src, "s_c_send")
    cmds = [c for _, c in c_literals_text(body)]
    ps = [c for c in cmds if c.lower().startswith("powershell")]
    if len(ps) != 1:
        problems.append((P0, "send", "回传链应有 1 条 PowerShell 命令, 实际 %d 条" % len(ps)))
        return ps[0] if ps else ""
    t = ps[0]
    must = [
        ("Join-Path $env:USERPROFILE", "根目录必须用 Join-Path 拼 (不再用 $a+'\\b' 相邻拼接)"),
        ("-LiteralPath $d", "Get-ChildItem 必须用 -LiteralPath (路径含 [ ] 时不被当通配符)"),
        ("ReadAllText($f,$o)", "按文件声明编码读取 (BOM 优先 / 无 BOM 走 $o)"),
        ("OEMCP", "无 BOM 的文件必须按 **OEM** 代码页解 (cmd 重定向写的就是 OEM)"),
        ("##### SKIP '+$f", "单文件读不了要留痕, 不能静默跳过"),
        ("##### END '+$n+' '+$e", "END 行要同时给成功数与跳过数"),
        ("$s.Encoding=[Text.Encoding]::UTF8", "串口必须显式 UTF-8"),
        ("VID_303A", "只写乐鑫那个 CDC 口 (不遍历所有 COM)"),
    ]
    for frag, why in must:
        if frag not in t:
            problems.append((P0, "send", "缺少 `%s` —— %s" % (frag, why)))
    # 反向锁: 旧写法必须绝迹
    for frag, why in [
        ("::Default.GetString(", "又回到 ANSI 解码 (会把 OEM 的中文解成乱码)"),
        ("$env:USERPROFILE+'", "又回到相邻拼接路径 (用户名带单引号会炸)"),
        ("ReadAllText($_.FullName)", "读取没传编码 (.NET 默认 UTF-8)"),
    ]:
        if frag in t:
            problems.append((P0, "send", "仍存在 `%s` —— %s" % (frag, why)))
    if not re.search(r"catch\{\$e\+\+", t):
        problems.append((P1, "send", "逐文件 catch 里没有累加失败数"))
    return t


def main():
    quiet = "-q" in sys.argv
    problems, stats, cmds = [], [], []

    ld = os.path.join(ROOT, "components/apps/pages/page_logdiag.c")
    wa = os.path.join(ROOT, "components/apps/pages/page_winassist.c")

    # 采集链里有 CHAIN_HEAD / DIRS_CMD 两个宏: 不展开的话每条链少算 2 条命令
    # (前两轮"85 条"就是这么来的 —— 统计口径偏小, 见第三轮报告 §2.4)
    ld_src = open(ld, encoding="utf-8").read()
    ld_exp = (ld_src.replace("    CHAIN_HEAD,", macro_literal(ld_src, "CHAIN_HEAD") + ",")
                    .replace("    DIRS_CMD,", macro_literal(ld_src, "DIRS_CMD") + ","))
    for line, text in c_literals_text(ld_exp):
        if text.startswith(CMD_PROGS):
            cmds.append(("page_logdiag.c:%d" % line, text))
    wa_tbl = winassist_commands(wa)
    tbl_set = set(t for _, t, _ in wa_tbl)
    for line, text, label in wa_tbl:
        cmds.append(("page_winassist.c:%d(%s)" % (line, label), text))
    for line, text in c_literals(wa):
        # 表里已有就不重复计; 表外出现的命令串单独列出来 (提醒: 可能漏了 CJK 标签)
        if text.startswith(CMD_PROGS) and text not in tbl_set:
            cmds.append(("page_winassist.c:%d(表外)" % line, text))

    for where, text in cmds:
        check_cmd(text, where, problems, stats)

    sim_paths(ld_src, problems)
    cwd_model_check(ld_src, problems)
    send_script_check(ld_src, problems)

    # ---- 交叉校验: 命令真实写的文件名 vs diag_core 的"应有文件" ----
    exp = expected_files()
    for arr, idx, cat in CHAIN_ARRAYS:
        have = set()
        for _, cmd in c_literals_text(chain_source(ld_src, arr)):
            have |= written_files(cmd)
        have.discard("rc.txt")
        miss = [n for n in exp[idx] if n not in have]
        extra = [n for n in have if n not in exp[idx]]
        if miss:
            problems.append((P0, "chain:%s" % cat,
                             "清单里有、命令里没写: %s" % ", ".join(miss)))
        if extra:
            problems.append((P1, "chain:%s" % cat,
                             "命令写了、清单里没有: %s" % ", ".join(extra)))
        if not quiet:
            print("  [%s] 应有 %d 个, 命令实写 %d 个" % (cat, len(exp[idx]), len(have)))

    total = sum(s[1] for s in stats)
    if not quiet:
        print("=== 注入命令清单 (%d 条, 共 %d 字符 ≈ 按键耗时 %.0f 秒 @50ms/字符) ===" %
              (len(stats), total, total * 0.05))
        for where, ln in stats:
            print("  %-42s %4d 字符" % (where, ln))

    if problems:
        print("\n=== 问题 (%d) ===" % len(problems))
        for sev, where, msg in problems:
            print("  [%s] %-38s %s" % (sev, where, msg))
        return 1
    print("\n✅ 全部通过: ASCII/映射表/引号/%VAR%/无 wmic 均符合")
    return 0


if __name__ == "__main__":
    sys.exit(main())
