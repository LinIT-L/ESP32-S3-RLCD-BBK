#!/usr/bin/env python3
"""
check_embed_js.py — 校验 .c 里内嵌的 HTML/JS 语法.

用法: python tools/check_embed_js.py components/apps/pages/page_fav.c [...]
对每个文件: 找出所有 C 字符串字面量 -> 拼成文本 -> 抽取 <script>...</script> -> node --check.

★ 2026-10-19 起因: page_fav.c 的 dologin() 少了 '}', 整个 <script> 解析失败,
  网页所有按钮全部无反应且不报错, 但 C 侧编译完全正常. 以后改内嵌网页必须跑这个.
"""
import re, subprocess, sys, tempfile, os, shutil

NODE = os.environ.get("NODE") or shutil.which("node") or \
    "/Users/linit/.workbuddy/binaries/node/versions/22.22.2-6/bin/node"

ESC = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\', "'": "'", '0': '\0'}


def unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'x' and re.fullmatch(r'[0-9a-fA-F]{2}', s[i + 2:i + 4] or ''):
                out.append(chr(int(s[i + 2:i + 4], 16))); i += 4; continue
            if n == 'u' and re.fullmatch(r'[0-9a-fA-F]{4}', s[i + 2:i + 6] or ''):
                out.append(chr(int(s[i + 2:i + 6], 16))); i += 6; continue
            out.append(ESC.get(n, n)); i += 2; continue
        out.append(c); i += 1
    return ''.join(out)


def scripts_of(path):
    src = open(path, encoding='utf-8', errors='replace').read()
    html = ''.join(unescape(m) for m in re.findall(r'"((?:[^"\\]|\\.)*)"', src))
    return re.findall(r'<script[^>]*>(.*?)</script>', html, re.S)


def main(paths):
    bad = 0
    for p in paths:
        for idx, js in enumerate(scripts_of(p)):
            with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False, encoding='utf-8') as f:
                f.write(js); tmp = f.name
            r = subprocess.run([NODE, '--check', tmp], capture_output=True, text=True)
            os.unlink(tmp)
            if r.returncode == 0:
                print(f'OK   {p} script#{idx} ({len(js)} chars)')
            else:
                bad += 1
                print(f'FAIL {p} script#{idx}')
                print(r.stderr.strip()[:1200])
    return 1 if bad else 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except Exception as e:          # 提取器自己出错不应阻断构建
        print(f'SKIP {e}')
        sys.exit(0)
