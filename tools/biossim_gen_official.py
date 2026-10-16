#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""biossim_gen_official.py — 联想官方 BIOS 模拟器的数据 → 应用的 Lua 数据段

数据来源（子代理从联想官方模拟器直接抓的**原始数据文件**，不是截图）：
    apps/biossim/ref/lenovo_official/*.json
    (官方源: download.lenovo.com/bsco/public/<机型>.json — 新版 SPA 渲染时按机型拉的那份)

产出：把下面这段写进 ~/Desktop/LinTOS_Lua应用/src/biossim.lua 的
      `-- @@OFFICIAL-BEGIN@@ … -- @@OFFICIAL-END@@` 之间（**只动这一段**）
      ```lua
      local OFFICIAL_MODELS = { { key=..., menus=..., pages=..., subpages=..., items=... }, ... }
      ```
应用侧在 `local BRAND_KEYS = BRAND_ORDER` 之后有一段合并代码，把每个机型变成品牌网格里的一个图标位。

用法：
    python3 tools/biossim_gen_official.py            # 生成/更新数据段
    python3 tools/biossim_gen_official.py --check     # 只校验是否与源同步(不同步 exit 1)
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.expanduser("~/Desktop/LinTOS_Lua应用/src/biossim.lua")
REF = os.path.join(ROOT, "apps", "biossim", "ref", "lenovo_official")
MARK_B = "-- @@OFFICIAL-BEGIN@@"
MARK_E = "-- @@OFFICIAL-END@@"

# (品牌键, 源文件, 图标短名, 网格下显示名, 标题栏)
MODELS = [
    ("v530",   "lenovo_v530_15arr_10y3_10v3.json",      "V530",   "联想 V530-15ARR（台式机 文本版）"),
    ("k14",    "lenovo_k14_g3_imu_21kl_21km.json",      "K14",    "联想 K14 G3（普通本）"),
    ("legion", "lenovo_legion_pro_7_16irx8h_82wq.json", "拯救者", "联想 拯救者 Legion Pro 7"),
]
# 6 个内置品牌: 官方菜单树挖到后**用同名 key 覆盖**原来手编的骨架 (2026-10-02)
#   ⚠ key 必须与内置 BRAND_ORDER 里的键一致, 才能覆盖而不是新增。
VENDORS = [
    ("award",  "Award",  "Award BIOS"),
    ("ami",    "AMI",    "AMI Aptio Setup Utility"),
    ("asus",   "华硕",   "ASUS UEFI BIOS Utility"),
    ("dell",   "戴尔",   "Dell BIOS Setup"),
    ("hp",     "惠普",   "HP BIOS Setup Utility"),
    ("huawei", "华为",   "HUAWEI MateBook UEFI Setup Utility"),
]


def _font_safe(s):
    """固件字库只有 **ASCII + 汉字**; 其它字符(° ç ñ 等)在设备上是空白格。
    这里做转写: 带音标的拉丁字母降级成 ASCII(ç→c / ñ→n), 降不掉的(°)丢弃。
    (2026-10-02: 官方数据里有 "32°C" / "Français" / "Español", lua_check 闸门直接拦下了。)"""
    import unicodedata
    out = []
    for ch in str(s):
        o = ord(ch)
        if o < 0x80:
            out.append(ch); continue
        if 0x3000 <= o <= 0x303F or 0x4E00 <= o <= 0x9FFF or 0xFF00 <= o <= 0xFFEF:
            out.append(ch); continue                      # 汉字/中文标点/全角: 字库有
        a = "".join(c for c in unicodedata.normalize("NFKD", ch) if ord(c) < 0x80)
        if a:
            out.append(a)
    return "".join(out)


def q(s):
    """Lua 字符串字面量: 必须转义 反斜杠/引号/**换行** —— 官方 tips 里有多行文本,
    不转义换行会导致 "unfinished string" 整包载入失败(2026-10-02 踩过)。"""
    t = _font_safe(s).replace("\\", "\\\\").replace('"', '\\"')
    t = t.replace("\r\n", "\\n").replace("\n", "\\n").replace("\r", "\\n").replace("\t", " ")
    return '"' + t + '"'


def build_one(key, path, short, label):
    d = json.load(open(path, encoding="utf-8"))
    pages, subpages, items = {}, {}, {}
    n = [0]
    collisions = []

    def add_item(it):
        n[0] += 1
        k = "%s_%04d" % (key, n[0])
        t = it.get("type") or "item"
        name = it.get("name") or ""
        vals = it.get("values")
        cur = it.get("current")
        if not vals:
            vals = [cur] if cur is not None else [""]
        else:
            vals = list(vals)
        try:
            df = vals.index(cur) + 1 if cur is not None else 1
        except ValueError:
            df = 1
        ro = bool(it.get("disabled")) or t in ("info", "separator") or not it.get("values")
        items[k] = {"name": name, "vals": vals, "def": df, "ro": ro,
                    "help": it.get("tip") or it.get("tips") or ""}
        return k

    def add_list(out):
        """把 items 列表转成行的引用列表; 子菜单递归注册到 subpages"""
        for it in out:
            if (it.get("type") == "submenu") and isinstance(it.get("sub"), dict):
                sub = it["sub"]
                nm = sub.get("name") or it.get("name") or "?"
                if nm in subpages:
                    if nm not in collisions:
                        collisions.append(nm)
                else:
                    subpages[nm] = None          # 占位, 先防递归自引用
                    subpages[nm] = add_list(sub.get("items") or [])
                rows_ref.append("@" + nm)
            else:
                rows_ref.append(add_item(it))

    for m in d.get("menus") or []:
        rows_ref = []
        add_list(m.get("items") or [])
        pages[m.get("name") or "?"] = rows_ref

    return {
        "key": key, "short": short, "label": label,
        "title": d.get("sim_title") or ("Lenovo BIOS Setup — " + (d.get("model") or key)),
        "thint": d.get("sim_footer") or "",
        "source": d.get("source_url") or "",
        "data_source": d.get("data_source_file") or "",
        "menus": [m.get("name") for m in (d.get("menus") or [])],
        "pages": pages, "subpages": subpages, "items": items,
        "collisions": collisions,
    }


def emit_lua(models):
    L = [MARK_B, "-- 由 tools/biossim_gen_official.py 从联想官方模拟器的数据文件生成，**不要手改**。",
         "-- 改数据请改 apps/biossim/ref/lenovo_official/*.json 或本脚本，然后重跑。",
         "local OFFICIAL_MODELS = {"]
    for m in models:
        L.append("  {")
        L.append("    key = %s, short = %s, icon = \"ibm\"," % (q(m["key"]), q(m["short"])))
        L.append("    label = %s," % q(m["label"]))
        L.append("    title = %s, thint = %s," % (q(m["title"]), q(m["thint"])))
        L.append("    menus = {%s}," % ", ".join(q(x) for x in m["menus"]))
        L.append("    pages = {")
        for pname, refs in m["pages"].items():
            L.append("      [%s] = {%s}," % (q(pname), ", ".join(q(r) for r in refs)))
        L.append("    },")
        L.append("    subpages = {")
        for sname, refs in m["subpages"].items():
            L.append("      [%s] = {%s}," % (q(sname), ", ".join(q(r) for r in (refs or []))))
        L.append("    },")
        L.append("    items = {")
        for k, it in m["items"].items():
            L.append("      [%s] = {name = %s, vals = {%s}, def = %d, ro = %s, help = %s},"
                     % (q(k), q(it["name"]), ", ".join(q(v) for v in it["vals"]),
                        it["def"], "true" if it["ro"] else "false", q(it["help"])))
        L.append("    },")
        L.append("  },")
    L.append("}")
    L.append(MARK_E)
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    models, stat = [], []
    for key, fn, short, label in MODELS:
        p = os.path.join(REF, fn)
        if not os.path.exists(p):
            sys.exit("✗ 缺数据文件: %s" % p)
        m = build_one(key, p, short, label)
        models.append(m)
        stat.append((key, len(m["menus"]), len(m["pages"]), len(m["subpages"]), len(m["items"]),
                     m["collisions"]))
    vdir = os.path.join(ROOT, "apps", "biossim", "ref", "vendors")
    for key, short, label in VENDORS:
        p = os.path.join(vdir, key + ".json")
        if not os.path.exists(p):
            print("  (跳过 %s: 没找到 %s)" % (key, p))
            continue
        m = build_one(key, p, short, label)
        m["title"] = (m.get("model") or label)
        models.append(m)
        stat.append((key, len(m["menus"]), len(m["pages"]), len(m["subpages"]), len(m["items"]),
                     m["collisions"]))

    block = emit_lua(models)
    s = open(SRC, encoding="utf-8").read()
    if MARK_B not in s or MARK_E not in s:
        sys.exit("✗ %s 里没有 %s / %s 标记段" % (SRC, MARK_B, MARK_E))
    # ⚠ 必须用**函数式替换**: re.sub 的替换串会把 \n 当转义解释掉,
    #   那样刚转义好的 "\\n" 又变回真换行 → Lua 报 "unfinished string"(2026-10-02 踩过)。
    new = re.sub(re.escape(MARK_B) + r".*?" + re.escape(MARK_E),
                 lambda _m: block, s, count=1, flags=re.S)

    for key, nm, np_, ns, ni, col in stat:
        print("  %-7s 菜单 %d · 页面 %d · 子菜单 %d · 条目 %d%s"
              % (key, nm, np_, ns, ni, ("  ⚠ 同名子菜单(已共用): " + ", ".join(col)) if col else ""))

    if args.check:
        if new != s:
            sys.exit("✗ 数据段与源 JSON 不同步 —— 跑一次 python3 tools/biossim_gen_official.py")
        print("  ✅ 数据段与源 JSON 一致")
        return

    if new == s:
        print("  (无变化)")
        return
    open(SRC, "w", encoding="utf-8").write(new)
    print("✓ 已更新 %s 里的 OFFICIAL_MODELS 段" % os.path.basename(SRC))
    print("  ⚠ 记得同步给设备: python3 tools/sync_apps.py biossim && bash flash_safe.sh")


if __name__ == "__main__":
    main()
