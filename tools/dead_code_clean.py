#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按"文件:行 → 符号名"删除死函数/死常量定义块 (括号或分号配对), 并顺带打印待人工判断的未用局部变量。
用法: dead_code_clean.py <worklist.json> [--apply]
worklist: [{"file":..., "line":..., "name":..., "kind":"func|const|var"}]
不 --apply 时只做干跑(打印将要删除的行范围)。"""
import io, json, os, re, shutil, sys, time

def find_end_func(lines, start):
    """从 start 行(含)开始, 找到函数定义体的结束行(含 `}`)。"""
    depth = 0
    seen_open = False
    for i in range(start, len(lines)):
        # 去掉字符串/字符字面量与注释的粗略影响就够用(这些文件里函数体不含裸花括号字面量)
        line = lines[i]
        for ch in line:
            if ch == '{':
                depth += 1; seen_open = True
            elif ch == '}':
                depth -= 1
                if seen_open and depth == 0:
                    return i
    return None

def find_end_decl(lines, start):
    """数组/变量定义: 找到第一个以 ';' 结尾的行(含)。"""
    for i in range(start, min(start + 60, len(lines))):
        if ';' in lines[i]:
            return i
    return None

def strip_leading_comment(lines, start):
    """把紧贴定义之上的连续注释块一起带走。
    ⚠ 只吃注释, **遇到空行/非注释行立即停** —— 避免把上方无关的注释/宏定义误删。"""
    i = start - 1
    while i >= 0:
        t = lines[i].strip()
        if t.startswith('*/') or t.startswith('//') or (t.startswith('*') and t != '*/'):
            i -= 1
        else:
            break
    # 只有真的吃到了注释(i < start-1)才生效; 否则从定义行本身开始
    return (i + 1) if i < start - 1 else start

def main():
    wl = json.load(io.open(sys.argv[1], encoding='utf-8'))
    apply = '--apply' in sys.argv
    per_file = {}
    for e in wl:
        per_file.setdefault(e['file'], []).append(e)

    for f, items in sorted(per_file.items()):
        lines = io.open(f, encoding='utf-8').read().split('\n')
        # 从后往前删, 避免行号漂移
        items = sorted(items, key=lambda x: -x['line'])
        plan = []
        for it in items:
            s = it['line'] - 1
            if it['kind'] == 'func':
                e = find_end_func(lines, s)
            else:
                e = find_end_decl(lines, s)
            if e is None:
                print("  !! %s:%d %s 找不到结尾, 跳过" % (f, it['line'], it['name'])); continue
            c = strip_leading_comment(lines, s)
            plan.append((c, e, it['name'], it['kind']))
        for c, e, name, kind in plan:
            print("  %s %s:%d-%d 删 %s (%s, %d 行)" %
                  ("[APPLY]" if apply else "[DRY]  ", f, c + 1, e + 1, name, kind, e - c + 1))
        if apply and plan:
            shutil.copy2(f, f + ".bak_dead_" + time.strftime("%Y%m%d_%H%M%S"))
            for c, e, name, kind in plan:
                for i in range(c, e + 1):
                    if i < len(lines):
                        lines[i] = '\x00'   # 标记删除
            out = [l for l in lines if l != '\x00']
            io.open(f, 'w', encoding='utf-8').write('\n'.join(out))
    return 0

if __name__ == '__main__':
    sys.exit(main())
