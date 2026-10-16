#!/usr/bin/env python3
"""wuxia_gen_data.py — 把 gmud.online 的 4 个数据文件转成掌机可 load() 的 Lua 数据模块

背景 (linit 2026-10-03: "你解析出来, 然后用它的数据复刻"):
  该站是《英雄坛说》网页复刻, **没有任何图片素材**, 全部内容 = 4 个 JSON:
    /data/maps.json  63 张地图(40x40 瓦片) + 152 门 + 181 NPC 实例 + 24 事件
    /data/npc.json   NPC 数值与对白
    /data/kf.json    功夫
    /data/items.json 物品
  它们的"画面"是用这些数据画出来的 —— 所以"复刻"= 把数据搬进来。

做法:
  ① 结构照搬 (字段名与它一致, 便于以后对齐/扩展);
  ② 输出**紧凑 Lua 数据文件** `apps/lua/wuxia/data.lua` → 设备上 `/store/wuxia_data.lua`,
     应用启动时 `load(app.fs.read(path))()` 读进来 (桥没有 require, 但 Lua 的 load 在);
  ③ 地图瓦片用 **RLE** 压缩 (63×1600 格直接写表会撑爆), 只在需要时展开单张。

⚠ 版权提示: 文本/数值来自该站(**它是《英雄坛说》的爱好者复刻**)。自用没问题;
   若以后要公开分发, 请把 `message`/`description` 这类**散文**替换成自己的文案
   —— 数据结构与数值不受版权保护, 那些长句受。

用法:
    curl -s https://www.gmud.online/data/{maps,npc,kf,items}.json -o /tmp/gm_#1.json   # 或手工下
    python3 tools/wuxia_gen_data.py            # 生成 apps/lua/wuxia/data.lua
    python3 tools/wuxia_gen_data.py --check    # 只校验源文件在不在
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = {"maps": "/tmp/gm_maps.json", "npc": "/tmp/gm_npc.json",
       "kf": "/tmp/gm_kf.json", "items": "/tmp/gm_items.json"}
OUT = os.path.join(ROOT, "apps", "lua", "wuxia_data.lua")


def q(s):
    """Lua 字符串字面量 (转义反斜杠/引号/换行; 控制字符丢掉)"""
    if s is None:
        return '""'
    s = str(s)
    s = s.replace("\\", "\\\\").replace('"', '\\"')
    s = s.replace("\r", "").replace("\n", "\\n").replace("\t", " ")
    s = "".join(ch for ch in s if ord(ch) >= 32)
    return '"%s"' % s


def rle(seq):
    """[1,1,2,2,2] → '1x2,2x3' (紧凑文本; 设备端展开)"""
    out, i = [], 0
    while i < len(seq):
        j = i
        while j + 1 < len(seq) and seq[j + 1] == seq[i]:
            j += 1
        out.append("%d:%d" % (seq[i], j - i + 1))
        i = j + 1
    return ",".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    missing = [p for p in SRC.values() if not os.path.exists(p)]
    if missing:
        sys.exit("✗ 缺源文件: %s\n  (curl -s https://www.gmud.online/data/<maps|npc|kf|items>.json -o /tmp/gm_<x>.json)" % ", ".join(missing))
    if a.check:
        for k, p in SRC.items():
            print("  ✓ %-6s %8d B" % (k, os.path.getsize(p)))
        return

    maps = json.load(open(SRC["maps"], encoding="utf-8"))
    npc = json.load(open(SRC["npc"], encoding="utf-8"))
    kf = json.load(open(SRC["kf"], encoding="utf-8"))
    items = json.load(open(SRC["items"], encoding="utf-8"))

    L = []
    L.append("-- wuxia_data.lua — 《江湖行》数据 (由 tools/wuxia_gen_data.py 生成, 不要手改)")
    L.append("-- 来源: gmud.online (《英雄坛说》网页复刻) 的 /data/*.json")
    L.append("-- 字段与该站一致; 地图瓦片用 RLE 文本压缩, 用到哪张展开哪张。")
    L.append("local D = {}")

    # ---- 物品 ----
    L.append("D.ITEM = {")
    for k, v in items.items():
        L.append('  [%s] = { name=%s, kind=%s, sub=%s, price=%d, exp=%d, grade=%s, attrs=%s, desc=%s },' % (
            k, q(v.get("name")), q(v.get("type")), q(v.get("subtype")),
            int(v.get("price") or 0), int(v.get("exp") or 0), q(v.get("grade")),
            q(json.dumps(v.get("attrs") or {}, ensure_ascii=False)), q(v.get("description"))))
    L.append("}")

    # ---- 功夫 ----
    L.append("D.KF = {")
    for k, v in kf.items():
        L.append('  [%s] = { name=%s, type=%s, weapon=%s, desc=%s },' % (
            k, q(v.get("name")), q(v.get("type")), q(v.get("weapon")), q(v.get("description"))))
    L.append("}")

    # ---- NPC (挑掌机用得上的字段, 散文 message 也带上: 你要"复刻") ----
    L.append("D.NPC = {")
    for k, v in npc.items():
        L.append('  [%s] = { name=%s, pai=%s, hp=%d, maxhp=%d, dmg=%d, def=%d, exp=%d, force=%d, msg=%s },' % (
            k, q(v.get("name")), q(v.get("pai")),
            int(v.get("hp") or 0), int(v.get("maxhp") or 0), int(v.get("damage") or 0),
            int(v.get("defense") or 0), int(v.get("exp") or 0), int(v.get("force") or 0),
            q(v.get("message"))))
    L.append("}")

    # ---- 地图 (元信息 + RLE 瓦片 + 门/NPC/事件) ----
    L.append("D.MAP = { start = { map=%d, x=%d, y=%d }, list = {" % (
        maps["start"]["map"], maps["start"]["x"], maps["start"]["y"]))
    for m in maps["maps"]:
        tiles = m.get("tiles") or []
        # t 字段 = 瓦片类型 (用 t 数组 RLE 足够; g/b/i 需要时再取)
        ts = [int(t.get("t") or 0) for t in tiles]
        bs = [int(t.get("b") or 0) for t in tiles]
        L.append('  { id=%d, name=%s, w=%d, h=%d, t="%s", b="%s", ground={%s} },' % (
            int(m.get("id") or 0), q(m.get("name")), int(m.get("w") or 0), int(m.get("h") or 0),
            rle(ts), rle(bs), ",".join(str(int(x)) for x in (m.get("ground") or []))))
    L.append("}, doors = {")
    for d in maps["doors"]:
        L.append("  { toMap=%d, x=%d, y=%d, id=%d }," % (
            int(d.get("toMap") or 0), int(d.get("x") or 0), int(d.get("y") or 0), int(d.get("id") or 0)))
    L.append("}, npcAt = {")
    for n in maps["npcs"]:
        L.append("  { npcId=%d, bmp=%d, id=%d }," % (
            int(n.get("npcId") or 0), int(n.get("bmp") or 0), int(n.get("id") or 0)))
    L.append("}, events = {")
    for e in maps["events"]:
        L.append("  { name=%s, bmp=%d, id=%d }," % (q(e.get("name")), int(e.get("bmp") or 0), int(e.get("id") or 0)))
    L.append("} }")
    L.append("return D")
    L.append("")

    open(OUT, "w", encoding="utf-8").write("\n".join(L))
    n_item, n_kf, n_npc, n_map = len(items), len(kf), len(npc), len(maps["maps"])
    print("✓ 生成 %s (%.1f KB)" % (os.path.relpath(OUT, ROOT), os.path.getsize(OUT) / 1024.0))
    print("   物品 %d / 功夫 %d / NPC %d / 地图 %d 张 / 门 %d / 事件 %d" % (
        n_item, n_kf, n_npc, n_map, len(maps["doors"]), len(maps["events"])))


if __name__ == "__main__":
    main()
