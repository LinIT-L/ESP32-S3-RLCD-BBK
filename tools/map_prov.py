#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""map_prov.py — 省级地图编译器: OSM PBF → **分块** .ltm + 省级概览

为什么不能像 `map_vec.py roads` 那样一个大文件了事
----------------------------------------------------
1. **文件太大读不进来**: 设备端 `app.fs.read` 上限 512 KB。福建全省路网
   几百万个点 ⇒ 单文件 20+ MB, 直接返回 nil。
2. **内存也放不下**: 8 MB PSRAM 不可能整省常驻。
3. **数据源也换了**: Overpass 拉一个省不现实（实测不带值时直接 504）,
   改用 **openstreetmap.fr / Geofabrik 的省级 PBF** 离线解析。

产物结构（`outdir/`）
--------------------
    fj.json                  元信息: tile 大小 / 网格原点 / 分块清单 / 概览文件名
    fj_road.ltm              **省级概览**: 只收 cls<=ovcls 的主干道, 大抽稀
    fj_poi.ltm               全省地标 (单点要素, 很小)
    tiles/{tx}_{ty}.ltm      分块, 块内按等级分桶 (LOD 机制不变)
                              tx = lon/0.2 向下取整, ty = lat/0.2 向下取整

设备端策略: z<=10 只用概览文件; z>=11 按视口加载 1~4 个块 (±1 块边距)。

⚠ 血泪
------
* **必须按 Content-Length 真实总长分块下载**: 估错总长会少下 20 MB, 拼出来的
  PBF 要到 osmium 解析 blob 时才报 "data error"（不早报）。见 `tools/dl_chunks.sh`。
* **要素 bbox 是 i4** (v2): 块内是相对块 min 的微度, 0.2° 块绰绰有余。
* **点表 poff 必须是**「相对点表起点的绝对偏移」**, 不是相对本等级桶的偏移 —— LOD
  目录给的 p0/p1 是绝对量, 两者不同量纲会把整块路网画错位。

用法
----
    bash tools/dl_chunks.sh <pbf_url> cache_map/fujian.pbf 16
    python3 tools/map_prov.py --pbf cache_map/fujian.pbf --prefix fj --outdir dist/map/fj
"""
import argparse
import json
import os
import struct
import sys
import time
from array import array

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from map_vec import (CLS_MAP, CLS_OTHER, CLS_NAME, LODS_POI, FEAT_FMT, FEAT_LEN,   # noqa: E402
                     HDR_FMT, HDR_LEN, LOD_FMT, LOD_LEN, MAGIC, VER,
                     LAYER_ROAD, LAYER_POI, crc32, dp_simplify, clip_polyline,
                     poi_cls, load_roads_lods, pack_ready, pack_ltm,
                     dissolve_ways, build_grid_index)


# ==================================================================
# 分块暂存: 用 array('i') 压住内存
# ==================================================================
# 全省上千万个点, 用 Python tuple/list 存会吃 1 GB+ 内存且慢到不可用;
# array('i') 每点 8 字节、每要素 32 字节, 全省也就几十 MB。
class TileStore:
    """一块 = 一个等级桶列表; 桶 = [要素表 array, 点表 array, 名字表 list]

    要素表每要素 8 个 int:
        cls, npts, nid, poff, bx0, by0, bx1, by1     (绝对微度 / 绝对点下标)
    点表每点 2 个 int (lon_e6, lat_e6), 绝对微度。
    """

    def __init__(self, ncls=8):
        self.ncls = ncls
        self.tiles = {}

    def _bucket(self, tx, ty, cls):
        arr = self.tiles.get((tx, ty))
        if arr is None:
            arr = [[array('i'), array('i'), []] for _ in range(self.ncls)]
            self.tiles[(tx, ty)] = arr
        return arr[cls]

    def add(self, tx, ty, cls, name, pe6, nameids):
        """pe6 = [(lon_e6, lat_e6), ...] 绝对微度, 已抽稀已裁块"""
        # ★ pack_ready 一次解决两件事: 点数 ≤255 (u8) 且段长 ≤3.3 km (i2 增量)。
        #   顺序反了会顶穿 255 —— 见 map_vec.pack_ready 的注释。
        #   tol0=10 µdeg ≈ 1e-5°, 与块内抽稀容差同量级, 只丢形状上最无所谓的点。
        f, p, nm = self._bucket(tx, ty, cls)
        nid = -1
        if name:
            nid = nameids.setdefault(name, len(nameids))
        for ch in pack_ready(pe6, 255, 10):
            poff = len(p) // 2                       # 点下标 (不是字节偏移)
            lons = [a for a, _ in ch]
            lats = [b for _, b in ch]
            f.extend((cls, len(ch), nid, poff,
                      min(lons), min(lats), max(lons), max(lats)))
            for a, b in ch:
                p.append(a)
                p.append(b)
            nm.append(name or "")


def pack_tile(buckets, tile_e6, lods):
    """一块的等级分桶 → .ltm 字节流。

    tile_e6 = (min_lon_e6, min_lat_e6, max_lon_e6, max_lat_e6)  —— **整数微度**,
              不用 float 度: tx*0.2 在二进制里是 116.00000000000001 这种数,
              乘 1e6 再 round 虽然救得回来, 但整条链路换成整数就没有任何余量问题了。
    """
    min_lon_e6, min_lat_e6, max_lon_e6, max_lat_e6 = tile_e6

    # ---- 字符串表: 只放**本块用到的**名字 ----
    # 全省名字全塞进每一块会撑爆 (每块多几十 KB × 几百块)。
    strtab = bytearray()
    smap = {}                                    # 全局 nid → 本文件内偏移
    feats = bytearray()
    ptbl = bytearray()
    fbb = []                                     # 要素 bbox (1e-5 度, 相对块 min) → 建索引用
    cls_span = {}                                # cls → (f_start, f_end, p_start, p_end)
    fcount = 0
    npt_total = 0

    for cls in range(len(buckets)):
        f, p, nm = buckets[cls]
        nfeat = len(f) // 8
        if nfeat == 0:
            continue
        f_start = fcount
        p_start = len(ptbl)
        for k in range(nfeat):
            o = k * 8
            c, npts, nid, poff, bx0, by0, bx1, by1 = f[o:o + 8]
            if nid >= 0 and nid not in smap:
                off = len(strtab)
                if off > 0xFFFE:
                    raise SystemExit("块内字符串表超 64 KB (nameoff 是 u16)")
                strtab.extend((nm[k] if k < len(nm) else "").encode("utf-8"))
                strtab.append(0)
                smap[nid] = off
            noff = smap[nid] if nid >= 0 else 0xFFFF
            # ★ 绝对偏移 (相对点表起点) —— 与 LOD 目录的 p0/p1 同量纲
            new_poff = len(ptbl)
            ptbl += struct.pack("<ii", p[poff * 2 + 1] - min_lat_e6,
                                       p[poff * 2] - min_lon_e6)
            plat = p[poff * 2 + 1]
            plon = p[poff * 2]
            for j in range(1, npts):
                lat = p[(poff + j) * 2 + 1]
                lon = p[(poff + j) * 2]
                ptbl += struct.pack("<hh", lat - plat, lon - plon)
                plat, plon = lat, lon
            # ⛔⛔ 要素 bbox 的单位是 **1e-5 度** (i4), 不是微度 —— 必须 //10。
            #   写漏这一步: 块照样能解析、要素数也对, 但每个要素的 bbox 被放大了 10 倍,
            #   裁剪时全部判成"在视口外" ⇒ 地图**一片空白**。踩过, 靠出样张才发现
            #   ("0/3133 条" 就是这个症状)。
            feats += struct.pack(FEAT_FMT, c, npts, noff, new_poff,
                                 (bx0 - min_lon_e6) // 10, (by0 - min_lat_e6) // 10,
                                 (bx1 - min_lon_e6) // 10, (by1 - min_lat_e6) // 10)
            fbb.append(((bx0 - min_lon_e6) // 10, (by0 - min_lat_e6) // 10,
                        (bx1 - min_lon_e6) // 10, (by1 - min_lat_e6) // 10))
            fcount += 1
            npt_total += npts
        cls_span[cls] = (f_start, fcount, p_start, len(ptbl))

    # ---- LOD 目录 ----
    # 要素按 cls **升序分桶**拼接 ⇒ "cls<=X" 天然是连续前缀, 零冗余。
    cls_all = sorted(cls_span)
    lod_blob = bytearray()
    for L in lods:
        sel = [c for c in cls_all if c <= L["cls_max"]]
        if not sel:
            lod_blob += struct.pack(LOD_FMT, L["cls_max"], L["w"], L["zmin"], 0, 0, 0, 0)
            continue
        lod_blob += struct.pack(LOD_FMT, L["cls_max"], L["w"], L["zmin"],
                                cls_span[sel[0]][0], cls_span[sel[-1]][1],
                                cls_span[sel[0]][2], cls_span[sel[-1]][3])

    off_lod = HDR_LEN
    off_feat = off_lod + LOD_LEN * len(lods)
    off_pt = off_feat + FEAT_LEN * fcount
    off_str = off_pt + len(ptbl)
    head = struct.pack(HDR_FMT, MAGIC, VER, LAYER_ROAD, len(lods),
                       min_lat_e6, min_lon_e6, max_lat_e6, max_lon_e6,
                       off_lod, off_feat, off_pt, off_str,
                       len(strtab), fcount, npt_total, 0)
    # 空间索引 (v3): 没有它设备端每帧要解整块的要素表 (实测 z16 扫 9307 条只画 31 条)
    idx, _im = build_grid_index(fbb,
                                (max_lon_e6 - min_lon_e6) // 10,
                                (max_lat_e6 - min_lat_e6) // 10,
                                origin=(min_lon_e6, min_lat_e6))
    off_idx = (off_str + len(strtab)) if idx else 0
    body = (bytes(head) + bytes(lod_blob) + bytes(feats) + bytes(ptbl)
            + bytes(strtab) + idx)
    crc = crc32(body[HDR_LEN:])
    return body[:52] + struct.pack("<I", crc) + struct.pack("<I", off_idx) + body[60:]


# ==================================================================
# 主流程
# ==================================================================
def main():
    ap = argparse.ArgumentParser(description="省级地图编译器 (PBF → 分块 .ltm)")
    ap.add_argument("--pbf", required=True)
    ap.add_argument("--prefix", default="fj", help="产物前缀 (fj → fj.json / fj.bin)")
    ap.add_argument("--name", default=None, help="包显示名 (默认同前缀), 如 福建省")
    ap.add_argument("--tile", type=float, default=0.15, help="块大小(度), 默认 0.15°≈17km")
    ap.add_argument("--outdir", default="dist/map/fj")
    ap.add_argument("--bbox", default=None, help="裁剪 s,w,n,e (默认用 PBF 完整范围)")
    ap.add_argument("--tol", type=float, default=1e-5, help="块内抽稀容差(度) ≈1.1m")
    ap.add_argument("--ovtol", type=float, default=6e-3, help="概览层抽稀容差(度) ≈660m")
    ap.add_argument("--ovcls", type=int, default=1,
                    help="概览层最高等级 (0=只要高速/国道, 1=再含省道/主干)")
    ap.add_argument("--maxcls", type=int, default=6,
                    help="块内最高等级 (6=含乡道/林道, 7=再含步道)")
    ap.add_argument("--ztile", type=int, default=11, help="从这一档起改用分块 (更小用概览)")
    ap.add_argument("--maxblk", type=int, default=1024 * 1024,
                    help="单块字节上限 (设备端 app.fs.read_at 的护栏)")
    ap.add_argument("--lods", default=None, help="道路 LOD 表 JSON (覆盖内置)")
    ap.add_argument("--no-poi", action="store_true")
    a = ap.parse_args()

    if not (0 < a.tile <= 2.0):
        raise SystemExit("--tile 必须 ∈ (0, 2]")
    if a.ovcls > a.maxcls:
        raise SystemExit("--ovcls 不能大于 --maxcls")

    try:
        import osmium
    except ImportError:
        raise SystemExit("缺 pyosmium: pip install osmium")

    t0 = time.time()
    store = TileStore()
    nameids = {}
    pois = []
    ov_feats = []
    stats = {"way": 0, "seg": 0, "pt": 0, "poi": 0, "skip": 0, "out": 0}

    TILE = int(round(a.tile * 1e6))              # 块宽 (微度)
    bb = None
    if a.bbox:
        s, w, n, e = (float(x) for x in a.bbox.split(","))
        bb = (w, s, e, n)                        # minlon, minlat, maxlon, maxlat
        if not (s < n and w < e):
            raise SystemExit("--bbox 需为 s,w,n,e (南<北 西<东)")

    def tile_of(idx):
        return idx // TILE                       # 正数; Python 的 // 对负数也是向下取整 ✓

    class H(osmium.SimpleHandler):
        def way(self, w):
            t = w.tags
            hw = t.get("highway")
            if not hw or hw in ("proposed", "construction", "razed", "abandoned"):
                return
            cls = CLS_MAP.get(hw, CLS_OTHER)
            if cls > a.maxcls:
                return
            try:
                pts = [(nd.location.lon, nd.location.lat) for nd in w.nodes]
            except osmium.InvalidLocationError:
                stats["skip"] += 1
                return
            if len(pts) < 2:
                return
            stats["way"] += 1
            nm = t.get("name")
            lons = [p[0] for p in pts]
            lats = [p[1] for p in pts]
            w0, w1 = min(lons), max(lons)
            s0, s1 = min(lats), max(lats)
            if bb and (w1 < bb[0] or w0 > bb[2] or s1 < bb[1] or s0 > bb[3]):
                return
            # ---- 概览层: 主干道 + 大抽稀 (全省一个文件) ----
            if cls <= a.ovcls:
                sp = dp_simplify(pts, a.ovtol)
                if len(sp) >= 2:
                    ov_feats.append(dict(cls=cls, pts=sp, name=nm))
            # ---- 分块: 先用 way 的 bbox 定位, 再逐块精确裁剪 ----
            tx0, tx1 = tile_of(int(w0 * 1e6)), tile_of(int(w1 * 1e6))
            ty0, ty1 = tile_of(int(s0 * 1e6)), tile_of(int(s1 * 1e6))
            for ty in range(ty0, ty1 + 1):
                for tx in range(tx0, tx1 + 1):
                    tb = (tx * TILE / 1e6, ty * TILE / 1e6,
                          (tx + 1) * TILE / 1e6, (ty + 1) * TILE / 1e6)
                    for seg in clip_polyline(pts, tb):
                        sp = dp_simplify(seg, a.tol)
                        if len(sp) < 2:
                            continue
                        pe6 = [(int(round(x * 1e6)), int(round(y * 1e6))) for x, y in sp]
                        store.add(tx, ty, cls, nm, pe6, nameids)
                        stats["seg"] += 1
                        stats["pt"] += len(pe6)

        def node(self, n):
            if a.no_poi or not n.tags:
                return
            try:
                if not n.location.valid():
                    return
            except Exception:
                return
            c = poi_cls(n.tags)
            if c is None:
                return
            nm = n.tags.get("name")
            if not nm:
                return
            lon, lat = n.location.lon, n.location.lat
            if bb and not (bb[0] <= lon <= bb[2] and bb[1] <= lat <= bb[3]):
                return
            pois.append((c, nm, lon, lat))
            stats["poi"] += 1

    print("解析 %s (locations 全开) ..." % a.pbf)
    H().apply_file(a.pbf, locations=True)
    print("  way=%d 出块片段=%d 点=%d 地标=%d 无坐标=%d  用时 %.1fs"
          % (stats["way"], stats["seg"], stats["pt"], stats["poi"], stats["skip"],
             time.time() - t0))

    os.makedirs(a.outdir, exist_ok=True)
    lods = load_roads_lods(a.lods)
    print("  LOD: " + " ".join("z%d+:cls%d" % (L["zmin"], L["cls_max"]) for L in lods))

    # ------------------------------------------------------------------
    # 打包: 一个包 = **1 个 .json 索引 + 1 个 .bin 数据体**
    # ------------------------------------------------------------------
    # 为什么不是"一瓦片一个文件":
    #   福建 = 354 道路块 + 两百多个地标块 ⇒ 600+ 个 HTTP 请求。设备端只能一个一个下,
    #   二十分钟都下不完, 每个请求还要重来一遍 TLS 握手。
    # 为什么不是"一个 .ltm 大文件":
    #   `app.fs.read` 有 512 KB 上限, 而整省 17 MB ⇒ 根本读不进来。
    # 所以折中: .bin 里把所有块**顺序平铺**, 索引里记每块的 (off,size), 设备端用
    #   `app.fs.read_at(bin, off, size)` 精确读块 —— 一次请求下完整个省,
    #   内存只驻留当前视口那几块。见 docs/矢量离线地图-方案与实现.md「省级包」。
    lods = load_roads_lods(a.lods)

    # ---- 概览 (z<=10 用; 必须足够小) ----
    if ov_feats:
        # ★ 必须先溶解再打包 —— 见 dissolve_ways 的注释: 福建 cls<=1 有 46k 条 way,
        #   要素表 24 B/条 就是 1.1 MB, 不溶解怎么调容差都压不进 400 KB。
        before = len(ov_feats)
        ov_feats = dissolve_ways(ov_feats)
        pts_all = [(p[0], p[1]) for f in ov_feats for p in f["pts"]]
        ovbb = bb if bb else (min(p[0] for p in pts_all), min(p[1] for p in pts_all),
                              max(p[0] for p in pts_all), max(p[1] for p in pts_all))
        ovbytes, _ = pack_ltm(ov_feats, ovbb, LAYER_ROAD, lods=lods)
        print("  概览溶解 %d → %d 条 (点 %d)"
              % (before, len(ov_feats), sum(len(f["pts"]) for f in ov_feats)))
    else:
        ovbb, ovbytes = (bb or (0.0, 0.0, 1.0, 1.0)), b""

    # ---- 地标按块分桶 (跟着道路块走, 逐块内嵌) ----
    # ⚠ 全省地标塞**一个**文件会挂: 10064 个中文名 ⇒ 字符串表 120 KB,
    #   而 nameoff 只有 u16 (0xFFFF) ⇒ pack 直接抛 "字符串表超 64KB"。
    #   分到块里每个文件只有十几~几十个名字, 顺带把设备端每帧的遍历量也降下来。
    poi_tiles = {}
    for c, nm, lon, lat in pois:
        k = (int(lon * 1e6) // TILE, int(lat * 1e6) // TILE)
        poi_tiles.setdefault(k, []).append(dict(cls=c, pts=[(lon, lat)], name=nm))

    blob = bytearray()
    off = 0

    def put(data):
        nonlocal off
        o = off
        blob.extend(data)
        off += len(data)
        return o

    ov_off = put(ovbytes) if ovbytes else 0

    # 索引**一行一块**: 道路与地标共用同一个网格键, 合成一行 ⇒ 索引条数减半
    # (福建 607 行, 而不是 607+564 条)。设备端建一张 "tx_ty" → 行 的表即可。
    idx = {}
    for (tx, ty) in sorted(poi_tiles):
        tb = (tx * TILE / 1e6, ty * TILE / 1e6, (tx + 1) * TILE / 1e6, (ty + 1) * TILE / 1e6)
        b, _ = pack_ltm(poi_tiles[(tx, ty)], tb, LAYER_POI, lods=LODS_POI)
        idx.setdefault((tx, ty), dict(x=tx, y=ty, o=0, s=0, no=0))
        r = idx[(tx, ty)]
        r["po"], r["ps"] = put(b), len(b)
        r["np"] = len(poi_tiles[(tx, ty)])

    for (tx, ty) in sorted(store.tiles):
        buckets = store.tiles[(tx, ty)]
        b = pack_tile(buckets, (tx * TILE, ty * TILE, (tx + 1) * TILE, (ty + 1) * TILE), lods)
        r = idx.setdefault((tx, ty), dict(x=tx, y=ty))
        r["o"], r["s"] = put(b), len(b)
        r["no"] = sum(len(x[1]) // 2 for x in buckets)

    rows = [idx[k] for k in sorted(idx)]
    for r in rows:
        r.setdefault("po", 0)
        r.setdefault("ps", 0)
        r.setdefault("np", 0)

    binpath = os.path.join(a.outdir, "%s.bin" % a.prefix)
    with open(binpath, "wb") as fp:
        fp.write(blob)
    rs = sorted(r["s"] for r in rows if r["s"]) or [0]
    ps = sorted(r["ps"] for r in rows if r["ps"]) or [0]
    print("  ✓ %s.bin  %.1f MB  (概览 %.0f KB + %d 块[路 中位 %.1f/最大 %.1f KB]"
          " + %d 地标块[中位 %.1f KB])"
          % (a.prefix, len(blob) / 1048576.0, len(ovbytes) / 1024.0, len(rows),
             rs[len(rs) // 2] / 1024.0, rs[-1] / 1024.0,
             sum(1 for r in rows if r["ps"]), ps[len(ps) // 2] / 1024.0))
    if len(ovbytes) > a.maxblk:
        print("  ⚠ 概览 %.0f KB 超单块上限 %.0f KB —— 调大 --ovtol 或调小 --ovcls"
              % (len(ovbytes) / 1024.0, a.maxblk / 1024.0))
    big = [r for r in rows if max(r["s"], r["ps"]) > a.maxblk]
    if big:
        print("  ⚠ %d 块超单块上限 %.0f KB: %s —— 调小 --tile 或调大 --tol"
              % (len(big), a.maxblk / 1024.0,
                 ", ".join("%d_%d(%dKB)" % (r["x"], r["y"], max(r["s"], r["ps"]) // 1024)
                           for r in big[:6])))

    # ---- 索引 ----
    meta = dict(fmt="ltm", ver=VER, id=a.prefix,
                name=a.name or a.prefix,
                bin="%s.bin" % a.prefix,
                tile=a.tile,
                ztile=a.ztile, ovcls=a.ovcls, maxcls=a.maxcls,
                minlon=round(ovbb[0], 6), minlat=round(ovbb[1], 6),
                maxlon=round(ovbb[2], 6), maxlat=round(ovbb[3], 6),
                ov_off=ov_off, ov_size=len(ovbytes),
                count=len(rows), bytes=len(blob),
                tiles=rows)
    with open(os.path.join(a.outdir, "%s.json" % a.prefix), "w", encoding="utf-8") as fp:
        json.dump(meta, fp, ensure_ascii=False, separators=(",", ":"))
    print("  ✓ %s.json  %.1f KB  (z<=%d 概览 / z>=%d 按视口取块)"
          % (a.prefix, os.path.getsize(os.path.join(a.outdir, "%s.json" % a.prefix)) / 1024.0,
             a.ztile - 1, a.ztile))
    print("总用时 %.0f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
