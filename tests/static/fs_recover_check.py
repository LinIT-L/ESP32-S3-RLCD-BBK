#!/usr/bin/env python3
"""tests/static/fs_recover_check.py — 全项目文件恢复/原子写一致性检查

背景: 第3回合把"写 tmp 再 rename"收敛成 os_file 公共层, 各模块陆续接入。
本脚本回答两个问题:

  Q1 哪些模块**用了** os_file 写盘, 但**读取时没调** os_file_recover_auto?
     → 掉电留下的 .old 永远不会被复位, 该模块的存档会"凭空消失"。

  Q2 还有没有模块**绕过**公共层, 直接用裸 fopen("wb") 写存档/配置?
     → 这些地方仍然保留"先截断再写"的老问题 (掉电即毁档)。

⚠ 这是**清单工具**, 不是判据: 命中要人看。例如
   · book_reader 的索引文件用 tmp+rename 自管, 属既定例外;
   · 某些模块只读不写, 自然没有写盘点。

用法: python3 tests/static/fs_recover_check.py [项目根] [--verbose]
退出码: 恒 0 (清单工具)
"""
import os
import re
import sys

ROOT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") \
    else os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
VERBOSE = "--verbose" in sys.argv

SCAN_DIRS = ["components", "main"]
SKIP_PARTS = ("/managed_components/", "/build", "/test", "/tests/")

WRITE_ATOMIC = re.compile(r"\bos_file_write_atomic\s*\(")
REPLACE = re.compile(r"\bos_file_replace\s*\(")
RECOVER = re.compile(r"\bos_file_recover(_auto)?\s*\(")
RAW_WB = re.compile(r'\bfopen\s*\([^;]*?"wb"')
TMP_RENAME = re.compile(r'\brename\s*\(')


def strip_comments(s):
    """去注释但**保留换行**, 否则报出来的行号会整体偏移 (曾因此把'读'误报成'写')。"""
    s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def is_storage_like(path, text):
    """只关心"落盘数据"的文件: 出现 .sav/.bkp/.bak/.bmp/.dat/.cfg 等字面量。"""
    return re.search(r'\.(sav|bkp|bak|bmp|dat|cfg|json|log|idx|state)"', text) is not None


def main():
    print("文件恢复 / 原子写一致性检查 (清单工具, 命中不等于 Bug)")
    print("项目根: %s\n" % ROOT)

    writers = []      # (rel, has_recover)
    raw_writers = []  # (rel, lines)
    recover_only = []

    for d in SCAN_DIRS:
        for dirpath, _dirnames, filenames in os.walk(os.path.join(ROOT, d)):
            if any(p in dirpath.replace("\\", "/") + "/" for p in SKIP_PARTS):
                continue
            for fn in filenames:
                if not fn.endswith((".c", ".cpp")):
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, ROOT)
                try:
                    with open(full, "r", encoding="utf-8", errors="replace") as fp:
                        raw = fp.read()
                except OSError:
                    continue
                text = strip_comments(raw)
                if "os_file_" not in text and '"wb"' not in text:
                    continue

                has_atomic = WRITE_ATOMIC.search(text) is not None
                has_replace = REPLACE.search(text) is not None
                has_recover = RECOVER.search(text) is not None
                if has_atomic or has_replace:
                    writers.append((rel, has_recover))
                elif has_recover:
                    recover_only.append(rel)

                if RAW_WB.search(text):
                    hits = []
                    tlines = text.split("\n")
                    for i, ln in enumerate(tlines):
                        if not RAW_WB.search(ln):
                            continue
                        # 往上看 6 行找路径变量名, 判断是否在写 .tmp/.part (安全模式)
                        ctx = "\n".join(tlines[max(0, i - 6):i + 1])
                        safe = (".tmp" in ctx) or (".part" in ctx) or \
                               ("tmp" in ln and "fopen" in ln) or ("part" in ln and "fopen" in ln)
                        hits.append((i + 1, ln.strip()[:88], safe))
                    if hits:
                        raw_writers.append((rel, hits))

    print("== A. 使用 os_file 写盘的模块, 读取侧是否做了 recover ==")
    print("   (没有 recover ⇒ 掉电留下的 .old 不会被复位, 该档会'消失')")
    missing = [r for r, rec in writers if not rec]
    ok = [r for r, rec in writers if rec]
    for rel in sorted(ok):
        print("  OK      %s" % rel)
    for rel in sorted(missing):
        print("  待核实  %s   ← 有原子写但没有 recover_auto" % rel)
    if not writers:
        print("  (未发现 os_file 使用点)")

    print("\n== B. 仍用裸 fopen(\"wb\") 写文件的点 ==")
    print("   [safe] = 写的是 .tmp/.part 之后再 rename, 属正确模式, 不是问题")
    print("   [看]   = 直接写正式文件, 需判断该数据是否值得原子化")
    if raw_writers:
        for rel, hits in sorted(raw_writers):
            for ln, code, safe in hits:
                tag = "[safe]" if safe else "[看]  "
                print("   %s %s:%d   %s" % (tag, rel, ln, code))
    else:
        print("  (无)")

    if VERBOSE and recover_only:
        print("\n== C. 只调 recover、没有写盘 (只读模块, 正常) ==")
        for rel in sorted(recover_only):
            print("          %s" % rel)

    print("\n---- 汇总 ----")
    print("使用 os_file 写盘: %d 个文件, 其中 %d 个缺 recover_auto"
          % (len(writers), len(missing)))
    print("裸 fopen(\"wb\") 写数据文件: %d 个文件" % len(raw_writers))
    print("\n⚠ 以上名单**不是缺陷清单**, 需按模块语义逐条核实后写入报告。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
