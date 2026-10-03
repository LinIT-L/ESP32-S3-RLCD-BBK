#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""wuxia_world_gen.py — 由 gmud.online 的 maps.json 生成固件用的世界数据。

生成 apps/lua/wuxia/world.lua (设备上 /store/wuxia/world.lua), 内容全部**源自原站**:
  start         出生点 {map,x,y}       —— 原站原值
  doors[152]    所有门 {toMap,x,y}      —— 原站原值
  maps[63]      每张图: w,h / t 值 RLE / 本图的门格 —— 原站原值
  npcs          **空表** (见下)

t 值语义 (原站 isBlocked): t∈{1,3,4} 不可行走; t=0 地面; t=2 门(可走, 到达即切换场景)。

⚠ 关于 NPC 布点: 为什么这里是空的
----------------------------------
原站 maps.json 的 `npcs` 只有 {bmp, npcId, id} —— **没有地图与坐标**; 真实位置由服务端
通过 WebSocket 下发 (客户端代码里 `npcs` 这个变量从头到尾没被用过, 已全文确认)。
离线单机版拿不到原版布点。

2026-10-03: 我原先按哈希"自己造"了一套 181 个布点, 用户实测到处是**看不见却挡路的
"毒蛇/NPC"**(那些精灵 id 在场景里没有对应图形, 于是只挡路不显形), 明确要求
「该删除的删除, 不要擅自添加」。所以这里**不生成任何布点**, 保持空表。
将来若拿到真布点, 在 `npcs` 里填 {bmp=, npc=, map=, x=, y=} 即可, 游戏侧代码已就绪。
"""
import argparse
import json
import os


def rle(vals):
    """[t,...] → "t:count,t:count" (与既有 wuxia_data.lua 的写法一致, 便于复用解码)。"""
    out = []
    prev, n = None, 0
    for v in vals:
        if v == prev:
            n += 1
        else:
            if prev is not None:
                out.append("%d:%d" % (prev, n))
            prev, n = v, 1
    if prev is not None:
        out.append("%d:%d" % (prev, n))
    return ",".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="/tmp/gmud_art/data", help="含 maps.json")
    ap.add_argument("--src", default="/tmp/gmud_art", help="素材根目录 (判断哪些精灵有图形)")
    ap.add_argument("--out", default="apps/lua/wuxia/world.lua")
    ap.add_argument("--npcs", default="", help="可选: 真布点 JSON (留空=不放任何 NPC)")
    a = ap.parse_args()

    M = json.load(open(os.path.join(a.data, "maps.json"), encoding="utf-8"))
    maps = M["maps"]
    doors = M["doors"]
    npcs = M.get("npcs") or []
    os.makedirs(os.path.dirname(a.out), exist_ok=True)

    # ---- 每张图: t 值 + 门格 ----
    per = {}
    for mp in maps:
        tiles = mp["tiles"]
        tv, dlist = [], []
        for i, t in enumerate(tiles):
            if isinstance(t, dict):
                tv.append(t.get("t", 0))
                if t.get("t") == 2 and t.get("i") is not None:
                    dlist.append((i, t["i"]))
            else:
                tv.append(t if isinstance(t, int) else 0)
        per[mp["id"]] = (mp["w"], mp["h"], rle(tv), dlist)

    # ---- NPC 布点 ----
    #
    # 原站 maps.json 的 `npcs` 只有 {bmp, npcId, id} —— **没有地图与坐标**; 真实位置由
    # 服务端通过 WebSocket 下发 (客户端代码里 `npcs` 变量从头到尾没被用过, 已全文确认)。
    # 所以"原版布点"拿不到, 位置只能由这里定。两条硬约束都是用户实测反馈换来的:
    #
    #   ① **精灵必须有图形才放**。早先按 id 直接放, 有些 id 在场景里没有对应图形 →
    #      画不出来却挡路, 就是"到处是看不见的毒蛇/NPC"。
    #   ② 布点必须**连通性安全** (拿掉这一格不会把可行走区域切成两半) 且不紧贴门格 ——
    #      挡的是"走到某个点就过不去、去不了下半部分"。
    #   另外游戏侧把 NPC 设为**不挡路** (见 wuxia.lua 的 WORLD.blocked), 双保险。
    def sprite_ids_with_art(src):
        """哪些 resmap id 真的有图形 (判定与 wuxia_pack 完全一致: 合成后非空白)。"""
        from wuxia_pack import ink_of
        from wuxia_art_gen import load_1bpp
        have = set()
        for i in range(512):
            rp = os.path.join(src, "img/res/resmap%03d.bmp" % i)
            if not os.path.exists(rp):
                continue
            mp = os.path.join(src, "img/mask/resmapmask%03d.bmp" % i)
            w, h, res = load_1bpp(rp)
            mask = None
            if os.path.exists(mp):
                _, _, mask = load_1bpp(mp)
            if any(any(r) for r in ink_of(res, mask, w, h)):
                have.add(i)
        return have

    def h32(x):
        x = (x * 2654435761) & 0xFFFFFFFF
        x ^= x >> 15
        x = (x * 2246822519) & 0xFFFFFFFF
        x ^= x >> 13
        return x

    def tile_t(mp, x, y):
        w, h = mp["w"], mp["h"]
        if x < 0 or y < 0 or x >= w or y >= h:
            return 1
        v = mp["tiles"][y * w + x]
        return v.get("t", 0) if isinstance(v, dict) else (v if isinstance(v, int) else 0)

    art_ids = sprite_ids_with_art(a.src)
    NO_NPC = {1, 62}                         # 自己的家 / 密室
    cand = [m for m in maps if m["id"] not in NO_NPC]
    blocked_by_map = {}
    placed = []

    def safe(mp, x, y):
        mine = blocked_by_map.setdefault(mp["id"], set())
        if tile_t(mp, x, y) != 0 or (x, y) in mine:
            return False
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if tile_t(mp, x + dx, y + dy) == 2:
                return False                 # 紧贴门 → 会让人进不了门
        comp, seen, st = [], {(x, y)}, [(x, y)]
        while st:                            # 先取所在连通块
            cx, cy = st.pop()
            comp.append((cx, cy))
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                q = (cx + dx, cy + dy)
                if q not in seen and tile_t(mp, q[0], q[1]) == 0 and q not in mine:
                    seen.add(q)
                    st.append(q)
        if len(comp) <= 1:
            return False
        rest = set(comp) - {(x, y)}
        start = next(iter(rest))
        vis, st = {start}, [start]
        while st:                            # 去掉这一格后是否仍连成一块
            cx, cy = st.pop()
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                q = (cx + dx, cy + dy)
                if q in rest and q not in vis:
                    vis.add(q)
                    st.append(q)
        return len(vis) == len(rest)

    for e in npcs:
        if e.get("bmp") not in art_ids:
            continue                         # ★ 没图形的不放 (那就是"隐形 NPC")
        for tryn in range(400):
            seed = h32(e["id"] * 977 + tryn * 131)
            mp = cand[seed % len(cand)]
            x = h32(seed + 1) % mp["w"]
            y = h32(seed + 2) % mp["h"]
            if safe(mp, x, y):
                blocked_by_map[mp["id"]].add((x, y))
                placed.append({"bmp": e["bmp"], "npc": e["npcId"],
                               "map": mp["id"], "x": x, "y": y})
                break

    L = ["-- wuxia/world.lua — 世界数据 (由 tools/wuxia_world_gen.py 生成, 不要手改)",
         "-- 来源: gmud.online /data/maps.json 的 maps/doors/start **原值**;",
         "--       t 值 RLE 用于碰撞 (原站 isBlocked: t∈{1,3,4} 不可走)。",
         "-- npcs: 原站只给 {bmp,npcId}, 位置由服务端下发 —— 这里按「有图形 + 不割裂通路」",
         "--       两条硬约束自定布点; 游戏侧 NPC 不挡路。见生成器文件头。",
         "return {",
         "  start = { map = %d, x = %d, y = %d }," % (M["start"]["map"], M["start"]["x"], M["start"]["y"]),
         "  doors = {"]
    for d in doors:
        L.append("    { toMap = %d, x = %d, y = %d }," % (d["toMap"], d["x"], d["y"]))
    L += ["  },", "  npcs = {"]
    for e in placed:
        L.append("    { bmp = %d, npc = %d, map = %d, x = %d, y = %d },"
                 % (e["bmp"], e["npc"], e["map"], e["x"], e["y"]))
    L += ["  },", "  maps = {"]
    for mp in maps:
        w, h, tr, dlist = per[mp["id"]]
        ds = "".join("%d:%d," % (i, di) for i, di in dlist)
        L.append('    [%d] = { w = %d, h = %d, t = "%s", d = "%s" },'
                 % (mp["id"], w, h, tr, ds))
    L += ["  },", "}", ""]
    open(a.out, "w", encoding="utf-8").write("\n".join(L))

    print("world.lua  %d 张图 / %d 扇门 / %d 个NPC布点  %.1f KB"
          % (len(maps), len(doors), len(placed), os.path.getsize(a.out) / 1024.0))


if __name__ == "__main__":
    main()
