#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""故障码库格式校验 / 统计 / 导入导出工具。

用法:
    python3 tools/faultcode_lint.py                 # 校验 (默认)
    python3 tools/faultcode_lint.py --stats         # 分类统计
    python3 tools/faultcode_lint.py --csv out.csv   # 导出 CSV (供 Excel 编辑)
    python3 tools/faultcode_lint.py --import in.csv # CSV → Lua 片段 (批量填充)
    python3 tools/faultcode_lint.py --suggest       # 给缺字段的条目建议 layer/vendor

校验内容:
    · 每条长度 = 16 (v2 schema)
    · 必填字段非空 (码 / 英文 / 中文 / 层 / 来源 / 检索词)
    · 枚举字段值域 (layer / severity / source)
    · 码唯一 (归一化后)
    · 检索词纯 ASCII (键盘只能打 ASCII)
"""
import sys, os, re, csv, argparse

LUA_PATHS = [
    "store/pkgs/故障信息库.lua",
    "apps/lua/apps/app/故障信息库.lua",
]
DB_BEGIN = "local DB = {"
DB_END = "\nlocal BYCODE"
CAT_PRE = ("local C_BOOT,C_DSK,C_RAM,C_MB,C_PWR,C_FAN,C_DSP,C_KBD,C_SEC,C_NET,C_BEEP,C_DEV,C_OS "
           "= 1,2,3,4,5,6,7,8,9,10,11,12,13\n")

# ---- v2 schema (顺序即数组下标, 1-based) ----
FIELDS = [
    ("code",  "码原文",        "0175 / 0x0000007B / Code 56 / 1-3-1", True),
    ("en",    "屏幕英文原文",  "Bad CRC1, stop POST task",            True),
    ("zh",    "中文含义",      "EEPROM 校验和不正确, 自检中止",        True),
    ("cat",   "硬件类别",      "C_MB (详情页展示, 不作导航)",          True),
    ("scope", "适用范围",      "ThinkPad 全系 / Windows 10/11",        True),
    ("cause", "可能原因",      "① … ② … ③ …",                         True),
    ("steps", "处置步骤",      "① … ② … ③ …",                         True),
    ("advice","结论建议",      "先试重刷官方 BIOS; 无效按主板故障处理",  True),
    ("risk",  "数据风险",      "无 / 高: 先备份",                       True),
    ("severity", "处置等级",   "现场可修 / 需备件 / 必须返厂",          True),
    ("source","来源代号",      "HMM / KB / SUP / MS / X390",           True),
    ("note",  "备注",          "FRU 指向 / 版本差异 (可空)",            False),
    ("keys",  "检索词",        "ASCII: 0175 bios eeprom 校验",          True),
    ("layer", "码所在层",      "POST/BEEP/BOOT/BSOD/DEVMGR/SYS/SELF",   True),
    ("vendor","厂商/体系",     "AMI/Phoenix/Insyde/Microsoft/通用",     True),
    ("verify","复验/二次确认", "怎么确认根因、怎么确认已修好 (分条)",    True),
]

CATS = ["C_BOOT", "C_DSK", "C_RAM", "C_MB", "C_PWR", "C_FAN",
        "C_DSP", "C_KBD", "C_SEC", "C_NET", "C_BEEP", "C_DEV", "C_OS"]

LAYERS = {
    "POST":   "固件自检数字码 (0175 / 01C8)",
    "BEEP":   "蜂鸣组合码 (1-3-1 / 1 短)",
    "BOOT":   "引导失败 (0xC0000225 / NTLDR is missing)",
    "BSOD":   "蓝屏 STOP (0x0000007B / DPC_WATCHDOG_VIOLATION)",
    "DEVMGR": "设备管理器代码 (Code 10 / Code 56)",
    "SYS":    "系统与更新 (0x80070005)",
    "SELF":   "自检现象 (开机黑屏仅电源灯亮, 非码)",
}
SEVERITIES = {"现场可修", "需备件", "必须返厂"}
SOURCES = {"HMM", "KB", "SUP", "X390", "MS", "OEM", "经验"}


def norm_code(s):
    """归一化去重键, 带命名空间。

    三层命名空间必须分开, 否则会跨体系误判成同一个码:
      dev:  Code 56      (设备管理器)
      hex:  0x000001CA   (蓝屏 STOP, 带 0x)
      raw:  01CA / 0175  (固件自检 POST, 不带 0x)
      txt:  BitLocker 恢复 (屏幕文字)
    """
    t = s.strip().lower()
    if t.startswith("code"):
        t = t[4:].strip().lstrip("0") or "0"
        return "dev:" + t
    if t.startswith("0x"):
        u = t[2:]
        if re.fullmatch(r"[0-9a-f]+", u):
            return "hex:" + (u.lstrip("0") or "0")
        return "hex:" + u
    if re.fullmatch(r"[0-9a-f]+", t):
        return "raw:" + (t.lstrip("0") or "0")
    return "txt:" + t


def extract_db(path):
    src = open(path, encoding="utf-8").read()
    if DB_BEGIN not in src or DB_END not in src:
        raise SystemExit(f"{path}: 找不到 DB 段")
    body = DB_BEGIN + src.split(DB_BEGIN, 1)[1].split(DB_END, 1)[0]
    try:
        import lupa
    except ImportError:
        raise SystemExit("需要 lupa: pip install lupa")
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    DB = L.execute(CAT_PRE + body + "\nreturn DB")
    rows = []
    for i in range(1, len(DB) + 1):
        r = DB[i]
        rows.append([str(r[k]) for k in range(1, len(r) + 1)])
    # cat 是常量 C_* 在 Lua 里求值成数字, 导出时还原成常量名, 否则回导会变字符串
    for r in rows:
        if len(r) > 3 and r[3].isdigit():
            n = int(r[3])
            r[3] = CATS[n - 1] if 1 <= n <= len(CATS) else r[3]
    return rows


def suggest_layer(code, cat):
    c = code.strip()
    if cat in ("11", "C_BEEP") or re.search(r"\d\s*-\s*\d", c) or "短" in c or "长" in c:
        return "BEEP"
    if re.fullmatch(r"0x[0-9A-Fa-f]{8}", c):
        return "BSOD"
    if re.fullmatch(r"[A-Z][A-Z0-9_]{5,}", c):
        return "BSOD"
    if re.match(r"^0xC0", c, re.I):
        return "BOOT"
    if re.fullmatch(r"[0-9A-Fa-f]{4}", c) or re.fullmatch(r"[0-9A-Fa-f]{2,3}", c):
        return "POST"
    return "BOOT"


def suggest_vendor(code, cat, scope):
    if cat in ("11", "C_BEEP"):
        return "通用"
    if re.fullmatch(r"0x[0-9A-Fa-f]{8}", code) or re.fullmatch(r"[A-Z][A-Z0-9_]{5,}", code):
        return "Microsoft"
    return "通用"


def validate(rows, path):
    errs, warns = [], []
    seen = {}
    for i, r in enumerate(rows, 1):
        tag = f"{path} 第{i}条({r[0] if r else '?'})"
        if len(r) != len(FIELDS):
            errs.append(f"{tag}: 字段数 {len(r)}, 应为 {len(FIELDS)}")
            continue
        d = {FIELDS[k][0]: r[k] for k in range(len(FIELDS))}
        for k, (name, cn, _, need) in enumerate(FIELDS):
            if need and not d[name].strip():
                errs.append(f"{tag}: 必填字段 {name}({cn}) 为空")
        if d["layer"] not in LAYERS:
            errs.append(f"{tag}: layer='{d['layer']}' 不在枚举内 {sorted(LAYERS)}")
        if d["severity"] not in SEVERITIES:
            errs.append(f"{tag}: severity='{d['severity']}' 不在枚举内 {sorted(SEVERITIES)}")
        if d["source"] not in SOURCES:
            warns.append(f"{tag}: source='{d['source']}' 建议改用 {sorted(SOURCES)}")
        if not re.fullmatch(r"[\x20-\x7e]*", d["keys"]):
            errs.append(f"{tag}: keys 含非 ASCII 字符 (键盘打不出)")
        n = norm_code(d["code"])
        if n in seen:
            errs.append(f"{tag}: 归一化码 '{n}' 与第{seen[n]}条重复")
        else:
            seen[n] = i
    return errs, warns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--csv")
    ap.add_argument("--import", dest="imp")
    ap.add_argument("--suggest", action="store_true")
    a = ap.parse_args()

    paths = [a.file] if a.file else [p for p in LUA_PATHS if os.path.exists(p)]
    if not paths:
        raise SystemExit("找不到目标文件")

    if a.imp:
        n = 0
        with open(a.imp, encoding="utf-8") as f:
            for row in csv.reader(f):
                if not row or row[0].startswith("#"):
                    continue
                row = [c.strip() for c in row]
                if len(row) < len(FIELDS):
                    row += [""] * (len(FIELDS) - len(row))
                q = ",\n  ".join(('"%s"' % c.replace("\\", "\\\\").replace('"', '\\"'))
                                 if not re.fullmatch(r"C_[A-Z]+", c) else c
                                 for c in row[:len(FIELDS)])
                print("{%s}," % q)
                n += 1
        print(f"-- 已生成 {n} 条, 粘贴到 DB 表末尾", file=sys.stderr)
        return

    fails = 0
    for p in paths:
        rows = extract_db(p)
        if a.csv:
            with open(a.csv, "w", newline="", encoding="utf-8-sig") as f:
                w = csv.writer(f)
                w.writerow([n for n, _, _, _ in FIELDS])
                w.writerows(rows)
            print(f"{p} → {a.csv} ({len(rows)} 条)")
            continue
        if a.stats:
            from collections import Counter
            c = Counter(r[9] for r in rows if len(r) > 9)
            print(f"\n== {p} 共 {len(rows)} 条 ==")
            print("处置等级:", dict(c))
            continue
        if a.suggest:
            for r in rows:
                print(f"{r[0]:<20} layer={suggest_layer(r[0], r[3])}  vendor={suggest_vendor(r[0], r[3], r[4])}")
            continue
        errs, warns = validate(rows, p)
        print(f"\n== {os.path.basename(p)}: {len(rows)} 条, {len(errs)} 错误, {len(warns)} 提示 ==")
        for e in errs[:40]:
            print("  ✗", e)
        if len(errs) > 40:
            print(f"  ... 另有 {len(errs) - 40} 条错误")
        for w in warns[:10]:
            print("  !", w)
        if errs:
            fails += 1
    if fails:
        sys.exit(1)


if __name__ == "__main__":
    main()
