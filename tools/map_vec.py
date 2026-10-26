#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""map_vec.py — LinTOS 离线【矢量】地图编译器 (OSM/DEM → .ltm 矢量包)

为什么不用 map_build.py 那套"预渲染 1bpp 图片"
------------------------------------------------
linit 2026-10-05 定案: **"我不需要图片地图, 我必须要使用矢量图"**
  位图地图  → 每个 zoom 档都要单独渲一份, 体积随档数线性膨胀, 且文字放大就糊。
  矢量地图  → 只存**折线点串**, 设备端按当前 zoom 实时投影+画线:
              任意缩放都清晰, 文字永远是固件 16px 点阵 (不放大不糊),
              同一份数据覆盖全部 zoom 档。

产物 `.ltm` 结构 (全小端, 与 Lua `string.unpack` 逐字节对齐)
--------------------------------------------------------------
    Header            64 B
    LOD 目录     20 B × nlod
    要素表       24 B × nfeat   (v2: bbox 由 i2 扩成 i4)
    点表              (见下)
    字符串表          UTF-8 路名, \\0 分隔
    ── 四段顺序固定, 段间无填充 (偏移都写在头里)

★ **LOD = 按缩放等级分档** (linit: "根据放大尺寸显示清晰度")
    缩略 (z≤12) 只画高速/国道/省道  → 一屏上万平方公里的城市级概览也不卡
    放大 (z≥17) 才铺小路/步道      → 街道级细节
  要素表**按 cls 升序排**, 于是每个 LOD 的要素区间是**连续前缀** —— 零冗余,
  同一条路不会为了多档而被存多份。

★ **为什么点表要"首点绝对 + 后续增量"**
    首点 i4 微度(相对全局 min)  → 精度 0.11 m, 范围 ±0.1° 够用
    后续 i2 微度(相对前一点)    → 4 B/点, 相邻 OSM 节点通常 <100 m (增量 10 万微度以内)
    比"每点 i4 绝对"省一半, 比"全部绝对 i4"省 50%。
    ⚠ 相邻点距若 >32767 微度(3.6 km) 会溢出 —— 打包时**硬断言**报错, 不静默截断。

用法
----
  # 道路 → 矢量包 (带缓存, 重复跑很快)
  python3 tools/map_vec.py roads --bbox 25.36,118.95,25.50,119.10 \\
      --out dist/map/putian_road.ltm
  # 等高线 (terrarium DEM → marching squares)
  python3 tools/map_vec.py contour --bbox 25.36,118.95,25.50,119.10 --interval 20 \\
      --out dist/map/putian_contour.ltm
  # 自检: 反向解析 + 用**固件字库**渲一张样张 (样张 = 真机所见)
  python3 tools/map_vec.py preview --in build/map/putian_road.ltm \\
      --center 25.4335,119.0110 --zoom 14 --out docs/screenshots/矢量样张.png
  python3 tools/map_vec.py info --in build/map/putian_road.ltm
"""
import argparse
import json
import math
import os
import struct
import sys
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from map_build import (CACHE, UA, OVERPASS, fetch_roads, lonlat_to_world,
                       m_per_px, fw_fonts, fw_text_w, fw_text_px)

MAGIC = b"LTM1"
# v2: 要素 bbox 由 i2 扩成 i4
# v3: 头部 8 字节填充位启用 → **粗网格空间索引** (off_idx @56, flags @60)
#     为什么需要: 实测 z16 每帧要解 **9307** 个要素表项, 只为了画 **31** 条 (300:1 浪费)。
#     所有主流离线格式都有这一层 (MBTiles 瓦片金字塔 / PMTiles 目录 / Mapsforge 的
#     per-zoom index blocks / Organic Maps 的 bucket 网格 / OsmAnd 的 tile 索引)。
#     索引布局见 build_grid_index()。
VER = 3
LAYER_ROAD = 1
LAYER_CONTOUR = 2
LAYER_POI = 3          # 地标 (单点要素 + 名字)

# ---------------- 道路等级 → cls (0 最大) ----------------
CLS_MAP = {
    "motorway": 0, "motorway_link": 0,
    "trunk": 0, "trunk_link": 0,
    "primary": 1, "primary_link": 1,
    "secondary": 2, "secondary_link": 2,
    "tertiary": 3, "tertiary_link": 3,
    "unclassified": 4,
    "residential": 5, "living_street": 5,
    "service": 6,
    # ★ track = 农林/林区便道 —— 山区极多, 归 6 (和 service 同档, z15 起画)
    "track": 6,
    # ★ 步道/骑行道 --- 数量最多、最"无关机动车", 归 7 (z17 才画);
    #   省级编译默认 --maxcls 6 把它们排除, 免得每块体积翻几倍。
    "footway": 7, "path": 7, "steps": 7, "cycleway": 7, "bridleway": 7,
}
CLS_OTHER = 7
CLS_NAME = {0: "高速/国道", 1: "省道", 2: "次干道", 3: "县道/主干",
            4: "乡道", 5: "居住区路", 6: "内部路", 7: "步道/小径"}
CLS_NAME_CONTOUR = {0: "整百米(计曲线)", 1: "整四十米(半距)", 2: "其余(首曲线)"}
CLS_NAME_POI = {0: "重要地标(市/区/镇/政府/医院/车站)", 1: "公共设施(学校/宗教/场馆)",
                2: "其他(古迹/酒店/景点)"}

# ---------------- 地标等级 (0 最重要) ----------------
# 只收"人话能当地标"的: 行政区 / 政府 / 医院 / 车站 / 学校 / 宗教 / 场馆 / 古迹。
# ⛔ 不收 shop/restaurant/cafe 之类 —— OSM 在国内本来就稀, 收了也全是空白,
#    反而把真正的地标挤没了。
POI_RULES = [
    (0, "place",   {"city", "town", "county", "state", "province"}),
    (0, "amenity", {"townhall", "hospital", "university", "college",
                    "bus_station", "ferry_terminal"}),
    (0, "railway", {"station"}),
    (1, "place",   {"village", "suburb", "neighbourhood", "quarter"}),
    (1, "amenity", {"school", "police", "fire_station", "marketplace",
                    "library", "place_of_worship", "kindergarten"}),
    (1, "tourism", {"attraction", "museum", "viewpoint", "theme_park", "zoo"}),
    (2, "historic", None),          # historic=* 任意值
    (2, "tourism", {"hotel", "hostel", "guest_house"}),
]


def poi_cls(tags):
    """OSM tags → 0/1/2; 不是地标返回 None"""
    best = None
    for cls, key, vals in POI_RULES:
        v = tags.get(key)
        if v is None:
            continue
        if vals is None or v in vals:
            if best is None or cls < best:
                best = cls
    return best


# 地标 LOD: 缩略只标大地点, 放大才补小设施 (复用与道路同一套 LOD 机制)
LODS_POI = [
    dict(zmin=0,  cls_max=0, w=1, label=True),    # 市/区/镇/政府/医院/车站
    dict(zmin=13, cls_max=1, w=1, label=True),    # + 学校/宗教/场馆
    dict(zmin=15, cls_max=2, w=1, label=True),    # + 古迹/酒店
]

# ---------------- LOD 分档 (zmin 升序) ----------------
# cls_max = 该档**画到哪一等级为止** (含); w = 线宽 px; label = 是否标路名
# 分档目标: **每屏要画的要素条数大致恒定** —— 缩得越小, 可见范围越大,
# 所以等级门槛必须收得更紧, 否则缩略视图反而比近景更慢。
LODS = [
    dict(zmin=0,  cls_max=1, w=2, label=True),    # z≤12 省市级概览: 高速/国道/省道
    dict(zmin=13, cls_max=2, w=2, label=True),    # z13    + 次干道
    dict(zmin=14, cls_max=3, w=2, label=True),    # z14    + 县道
    dict(zmin=15, cls_max=6, w=1, label=True),    # z15-16 + 居住区路/内部路
    dict(zmin=17, cls_max=7, w=1, label=True),    # z17+   + 步道/小径
]

# 等高线用**自己一套** LOD —— 语义正好相反: 缩放越小只画**越粗的等值线**
#   cls 0 = 每 100 m 一根 (计曲线) · cls 1 = 每 50 m (半距) · cls 2 = 每 20 m (首曲线)
#   ⚠ 文件里存了 LOD 表, 所以设备端不需要硬编码等级含义, 读表即可 (自描述)。
LODS_CONTOUR = [
    dict(zmin=0,  cls_max=0, w=1, label=False),   # 缩略: 只看 100 m 大台阶
    dict(zmin=13, cls_max=1, w=1, label=False),   # 中景: + 50 m
    dict(zmin=15, cls_max=2, w=1, label=False),   # 近景: + 20 m
]


def load_roads_lods(path=None):
    """返回道路 LOD 表 (zmin 升序)。

    ⚠ **必须是连续前缀**: 文件里的要素表按 cls 升序排, 每档取 "cls<=cls_max" 的
      前缀区间才零冗余。所以 cls_max 必须**单调不减**, 否则会出现空洞档。
    path 给定时从 JSON 覆盖 (便于调档不碰代码), 支持两种写法:
      [ {zmin,cls_max,w,label}, ... ]        或者      {"roads":[...]}
    """
    if path:
        with open(path, "r", encoding="utf-8") as fp:
            j = json.load(fp)
        if isinstance(j, dict):
            j = j.get("roads") or next(iter(j.values()))
        lods = [dict(zmin=int(L["zmin"]), cls_max=int(L["cls_max"]),
                     w=int(L.get("w", 1)), label=bool(L.get("label", True))) for L in j]
        lods.sort(key=lambda d: d["zmin"])
        prev = -1
        for L in lods:                       # 前缀性硬校验
            if L["cls_max"] < prev:
                raise SystemExit("LOD 表 cls_max 必须单调不减 (前缀性), 出错档 zmin=%d" % L["zmin"])
            prev = L["cls_max"]
        return lods
    import copy
    return copy.deepcopy(LODS)


# ==================================================================
# 几何
# ==================================================================
def fetch_pois(bbox, timeout=180):
    """拉地标 (place / amenity / tourism / historic / railway=station)。

    ⚠ 与 fetch_roads 一样**必须带 User-Agent**, 否则 Overpass 直接 406。
    ⚠ 面要素用 `out center` 拿中心点 —— 1bpp 掌机画不了面轮廓, 标个点就够。
    """
    s, w, n, e = bbox
    # ⛔ 别写 `node["amenity"]`(不带值) —— 那会把 bbox 内**所有** amenity 全拉回来,
    #   Overpass 直接 504 Gateway Timeout (实测)。必须按具体值用正则筛。
    AMEN = ("^(hospital|school|university|college|kindergarten|police|fire_station|"
            "bus_station|ferry_terminal|townhall|marketplace|library|place_of_worship)$")
    TOUR = "^(attraction|museum|viewpoint|theme_park|zoo|hotel|hostel|guest_house)$"
    q = ('[out:json][timeout:%d];('
         'node["place"](%f,%f,%f,%f);'
         'node["amenity"~"%s"](%f,%f,%f,%f);'
         'way["amenity"~"^(hospital|school|university|college|townhall|marketplace)$"](%f,%f,%f,%f);'
         'node["tourism"~"%s"](%f,%f,%f,%f);'
         'node["historic"](%f,%f,%f,%f);'
         'node["railway"="station"](%f,%f,%f,%f);'
         ');out center tags;'
         % ((timeout, s, w, n, e, AMEN, s, w, n, e, s, w, n, e,
             TOUR, s, w, n, e, s, w, n, e, s, w, n, e)))
    os.makedirs(CACHE, exist_ok=True)
    key = os.path.join(CACHE, "poi_%.4f_%.4f_%.4f_%.4f.json" % bbox)
    if os.path.exists(key):
        d = json.load(open(key, encoding="utf-8"))
        print("  用缓存 %s (%d 条)" % (os.path.relpath(key, ROOT), len(d.get("elements", []))))
        return d.get("elements", [])
    print("  拉 Overpass 地标 ...")
    req = urllib.request.Request(OVERPASS, data=urllib.parse.urlencode({"data": q}).encode(),
                                 headers={"User-Agent": UA,
                                          "Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=timeout + 30) as r:
        raw = r.read()
    d = json.loads(raw.decode("utf-8"))
    json.dump(d, open(key, "w", encoding="utf-8"))
    print("  ✓ %d 条, %.1f KB" % (len(d.get("elements", [])), len(raw) / 1024.0))
    return d.get("elements", [])


def dp_simplify(pts, tol):
    """Douglas-Peucker。pts = [(x, y), ...] (度)。tol 单位与坐标一致。
    迭代实现 (不用递归) —— 一条长路几千个点, 递归会爆栈。"""
    n = len(pts)
    if n < 3:
        return pts
    keep = bytearray(n)
    keep[0] = keep[n - 1] = 1
    stack = [(0, n - 1)]
    t2 = tol * tol
    while stack:
        i, j = stack.pop()
        if j <= i + 1:
            continue
        x0, y0 = pts[i]
        x1, y1 = pts[j]
        dx, dy = x1 - x0, y1 - y0
        d2 = dx * dx + dy * dy
        best, bi = -1.0, -1
        for k in range(i + 1, j):
            x, y = pts[k]
            if d2 <= 0.0:
                dd = (x - x0) ** 2 + (y - y0) ** 2
            else:
                t = ((x - x0) * dx + (y - y0) * dy) / d2
                if t < 0.0:
                    t = 0.0
                elif t > 1.0:
                    t = 1.0
                px, py = x0 + t * dx, y0 + t * dy
                dd = (x - px) ** 2 + (y - py) ** 2
            if dd > best:
                best, bi = dd, k
        if best > t2 and bi > 0:
            keep[bi] = 1
            stack.append((i, bi))
            stack.append((bi, j))
    return [pts[k] for k in range(n) if keep[k]]


def _clip_seg(x0, y0, x1, y1, bb):
    """Liang-Barsky 线段裁剪。返回裁剪后的 (x0,y0,x1,y1) 或 None。"""
    xmin, ymin, xmax, ymax = bb
    dx, dy = x1 - x0, y1 - y0
    t0, t1 = 0.0, 1.0
    for p, q in ((-dx, x0 - xmin), (dx, xmax - x0), (-dy, y0 - ymin), (dy, ymax - y0)):
        if p == 0.0:
            if q < 0.0:
                return None
        else:
            r = q / p
            if p < 0.0:
                if r > t1:
                    return None
                if r > t0:
                    t0 = r
            else:
                if r < t0:
                    return None
                if r < t1:
                    t1 = r
    return (x0 + t0 * dx, y0 + t0 * dy, x0 + t1 * dx, y0 + t1 * dy)


def clip_polyline(pts, bb, eps=1e-11):
    """把折线裁进 bbox。出界段断开 → 一条路可能被切成多条子折线。

    ⚠ 为什么必须裁: Overpass 的 `out geom` 给的是 **way 的完整几何**, 不是 bbox 内的部分。
      一条跨行政区的国道能把数据范围拖出几十分之一度, 于是要素 bbox(i2, 1e-5 度) 溢出,
      而且白存几十公里屏外数据。
    """
    out = []
    cur = []
    for i in range(len(pts) - 1):
        x0, y0 = pts[i]
        x1, y1 = pts[i + 1]
        r = _clip_seg(x0, y0, x1, y1, bb)
        if r is None:
            if len(cur) >= 2:
                out.append(cur)
            cur = []
            continue
        cx0, cy0, cx1, cy1 = r
        if cur and abs(cur[-1][0] - cx0) < eps and abs(cur[-1][1] - cy0) < eps:
            cur.append((cx1, cy1))
        else:
            if len(cur) >= 2:
                out.append(cur)
            cur = [(cx0, cy0), (cx1, cy1)]
    if len(cur) >= 2:
        out.append(cur)
    return out


# ==================================================================
# 打点前的两道几何处理 (对 pack_ltm / map_prov.pack_tile 通用)
# ==================================================================
# i2 增量上限 = ±32767 微度 ≈ ±3.6 km。缩略层大抽稀后, 一条笔直国道相邻两个
# 保留点可以拉开十几公里 —— 直接溢出。**插共线点**是唯一正确解:
# 插进去的点都在原线段上, Bresenham 走同一条线, 渲染结果与原样**逐像素一致**。
MAXD_E6 = 30000            # 稍微留余量 (0.030° ≈ 3.33 km)


def densify_e6(pts, maxd=MAXD_E6):
    """pts=[(lon_e6,lat_e6),...] 整数微度; 把相邻点距 >maxd 的段插满共线点。"""
    if len(pts) < 2:
        return pts
    out = [pts[0]]
    plon, plat = pts[0]
    for lon, lat in pts[1:]:
        dlon, dlat = lon - plon, lat - plat
        m = max(abs(dlon), abs(dlat))
        if m > maxd:
            k = (m + maxd - 1) // maxd
            for j in range(1, k):
                out.append((plon + (dlon * j) // k, plat + (dlat * j) // k))
        out.append((lon, lat))
        plon, plat = lon, lat
    return out


def densify_deg(pts, maxd=MAXD_E6):
    """度坐标版: 先转微度, 处理完再转回 (内部往返取整, 与直接打包等价)。"""
    e6 = densify_e6([(int(round(x * 1e6)), int(round(y * 1e6))) for x, y in pts], maxd)
    return [(x / 1e6, y / 1e6) for x, y in e6]


def fit_budget(pts, budget=255, tol0=6e-6):
    """把折线压到 budget 点以内 (纯 DP, 不管 i2 增量)。

    ⚠ 为什么不能像原来那样**均匀抽样**: `pts[::step]` 会把一段急弯的细节直接抹掉,
      地图上表现为"道路被拉直/抄近道"。改用**加大 DP 容差**去压 —— 它总是优先
      丢掉最"不影响形状"的点。
    """
    if len(pts) <= budget:
        return pts
    tol = tol0
    for _ in range(14):
        out = dp_simplify(pts, tol)
        if len(out) <= budget:
            return out
        tol *= 1.7
    step = max(1, (len(pts) + budget - 1) // budget)      # 兜底
    return pts[::step]


def _need_e6(pp, maxd=MAXD_E6):
    """插满共线点后的点数 (精确值, 不是估计)。"""
    n = 1
    plon, plat = pp[0]
    for lon, lat in pp[1:]:
        m = max(abs(lon - plon), abs(lat - plat))
        n += 1 if m <= maxd else (m + maxd - 1) // maxd
        plon, plat = lon, lat
    return n


def pack_ready(pts_e6, budget=255, tol0_e6=10, maxd=MAXD_E6):
    """µdeg 点串 → 「可直接打点」的块列表 (每块插满共线点后仍 ≤ budget 点)。

    ★★ 两条约束必须**同时**满足, 这是本项目最容易写错的地方:
        ① npts 字段是 u8          ⇒ 最终点数 ≤ 255
        ② 相邻点增量是 i2 (±32767) ⇒ 每段 ≤ 3.3 km, 长的段要插共线点
      踩过的坑: 先把点数 DP 压到 255, **再**插点 ⇒ 插完顶穿 255,
      `struct.pack` 直接抛 `'B' format requires 0 <= number <= 255`。
      正确做法是把"插点开销"一起算进预算 —— 见 _need_e6。
      ⚠ 插点开销 ≈ 折线总长/3.3km, 与保留点数无关 ⇒ 一条超长的路 (合并同名路后
        可能出现 800 km 的国道) 天然放不进 255 点。这时**拆成多条同名要素**,
        不能截断 (截断 = 地图上少一截路)。
    """
    if len(pts_e6) < 2:
        return [pts_e6]
    # dp_simplify 与坐标单位无关 ⇒ 直接在 µdeg 上跑, tol 也用 µdeg
    tol = tol0_e6
    cur = pts_e6
    for _ in range(24):
        if _need_e6(cur, maxd) <= budget:
            d = densify_e6(cur, maxd)
            if len(d) <= budget:
                return [d]
        tol = tol * 1.8
        cur = dp_simplify(pts_e6, tol)
    # 兜底: 按 budget 点一段切开 (只在极端长路上触发)
    d = densify_e6(cur, maxd)
    return [d[i:i + budget] for i in range(0, len(d), budget)] or [pts_e6[:budget]]


def _chain_segments(segs, strict, maxpts=4096):
    """贪心首尾相接: 把一堆折线拼成尽量少的条数。

    strict=True  —— 只在**共享端点恰好只连着 2 条线**时才接 (标准路网溶解规则)。
                    无名路必须用这个: 端点上有 3+ 条线就是真路口, 硬接会画出一条
                    不存在的路 (把两条不相干的乡道连成一条直线)。
    strict=False —— 同名路随意接 (同名的就是同一条路, 路口分叉也无所谓)。
    """
    from collections import defaultdict
    n = len(segs)
    if n <= 1:
        return [list(s) for s in segs]

    def ek(p):
        return (round(p[0], 7), round(p[1], 7))

    ep = defaultdict(list)                    # 端点 → [(段下标, 0=首 / 1=尾)]
    for i, s in enumerate(segs):
        ep[ek(s[0])].append((i, 0))
        ep[ek(s[-1])].append((i, 1))

    out = []
    used = [False] * n
    for i0 in range(n):
        if used[i0]:
            continue
        used[i0] = True
        chain = list(segs[i0])
        grew = True
        while grew and len(chain) < maxpts:
            grew = False
            for end in (1, 0):                # 先接尾, 再接首
                k = ek(chain[-1] if end else chain[0])
                if strict and len(ep[k]) != 2:
                    continue                  # 真路口 / 断头 —— 不接
                cand = None
                for (j, e) in ep[k]:
                    if not used[j]:
                        cand = (j, e)
                        break
                if cand is None:
                    continue
                j, e = cand
                used[j] = True
                s = segs[j]
                if end:
                    # 让 j 的**首点**贴住 chain 尾
                    chain.extend(s[1:] if e == 0 else list(reversed(s))[1:])
                else:
                    # 让 j 的**尾点**贴住 chain 首
                    chain = (s[:-1] if e == 1 else list(reversed(s))[:-1]) + chain
                grew = True
                break
        out.append(chain)
    return out


def dissolve_ways(feats, maxpts=4096):
    """把 way 合并成长折线 —— 概览层的**决定性**优化, 见下面 voluminous 的说明。

    为什么必须有这一步
    ------------------
    要素表是 **24 B/条**, 而 OSM 把一条国道打成几百段 (路口、桥、限速段各一条 way)。
    示例省 cls<=1 实测 **46,065 条 way** ⇒ 光要素表就 1.1 MB, 而概览层总共只给几百 KB。
    折线**点**反而是小头 (平均 2.3 点/条)。所以不合并, 怎么调容差都压不下来。

    两级策略 (实测数据得出)
    --------------------------------
      ① 同名同级的直接串起来 (同名 ⇒ 同一条路, 路口分叉也没关系)
      ② 无名的走 **strict**: 只在"共享端点恰好连 2 条"时才接。
         第一版只做 ①, 46065 条只降到 24172 —— 说明**多数干道压根没名字**,
         不处理无名的等于没优化。
    """
    named = {}
    unnamed = {}
    for f in feats:
        nm = f.get("name")
        if nm:
            named.setdefault((f["cls"], nm), []).append(f["pts"])
        else:
            unnamed.setdefault(f["cls"], []).append(f["pts"])

    out = []
    for (cls, nm), segs in named.items():
        for ch in _chain_segments(segs, strict=False, maxpts=maxpts):
            out.append(dict(cls=cls, pts=ch, name=nm))
    for cls, segs in unnamed.items():
        for ch in _chain_segments(segs, strict=True, maxpts=maxpts):
            out.append(dict(cls=cls, pts=ch, name=None))
    return out


# ==================================================================
# 打包 / 解包
# ==================================================================
#                         magic  ver lay nlod  minlat minlon maxlat maxlon  ← 8
#                         off_lod off_feat off_pt off_str len_str n_feat n_pt crc  ← 8
# ⚠⚠ 跨语言陷阱: 4 字节定长串 —— Python 写 "4s", Lua 写 "c4"。
#    两边**不是同一个写法**: Python 的 "c4" = 4 个独立的 c (占 4 项!), Lua 的 "c4" = 定长串。
#    踩过一次: Python 侧写 "c4" → pack 报 "expected 19 items"。
HDR_FMT = "<4sBBH" + "iiii" + "IIIIIIII" + "xxxxxxxx"      # 4+1+1+2+16+32+8 = 64 B, 16 值
HDR_LEN = 64
LOD_FMT = "<BBHIIII"                        # 1+1+2+4+4+4+4 = 20 B, 7 值
LOD_LEN = 20
# ★ v2: 要素 bbox 从 i2 扩成 **i4** (1e-5 度, 相对文件 min)。
#   为什么必须改: i2 只有 ±32767 ⇒ **单文件跨度上限 0.3277°**; 而示例省有 4.9°,
#   连"省级概览"这一个文件都装不下 (更别说将来加城市)。i4 范围 ±21474° ✓。
#   代价: 要素表 16 → 24 字节 (示例 3419 条 ≈ +27 KB), 换来"任意大小都能装一个文件"。
FEAT_FMT = "<BBHI" + "iiii"                 # 1+1+2+4+16 = 24 B, 8 值
FEAT_LEN = 24


def crc32(buf):
    import zlib
    return zlib.crc32(buf) & 0xFFFFFFFF


def build_grid_index(fbb, span_w, span_h, origin=(0, 0), ncells_max=64, max_span=3):
    """粗网格空间索引 —— 设备端"只解视口那几格"的依据。

    为什么非要有它（2026-10-05 实测）
    ---------------------------------
    没有索引时 `draw_layer` 必须**逐个 unpack** 当前 LOD 的每个要素表项再判 bbox:
      z16: 扫 9307 条只画 31 条 (300:1) · z10: 扫 13110 条只画 594 条 (22:1)。
    Lua 里一次 `string.unpack` ≈ 0.5~1 µs ⇒ 每帧白烧 5~9 ms。

    怎么做的（**关键设计：只存"最小格"**）
    --------------------------------------
    一个要素的 bbox 可能跨若干格。常见做法是"每格都登记一条"——但那样查询时同一个
    要素会被取到多次, 还得去重 (Lua 侧要一张 nfeat 大小的去重表, 几千要素就是几百 KB)。
    这里换一条更省的路: **要素只登记在它 bbox 的"最小格"(cx0,cy0) 里**, 查询时把格区间
    **向外扩 max_span-1 格**。数学上等价, 而且天然不会重复取到同一个要素 ✓
    代价是索引里每个要素只占 **1 条 u16**, 不是 1.5~3 条。

    fbb   = [(bx0,by0,bx1,by1), ...] 单位 **1e-5 度**, 相对**本文件 min**
    span  = (文件宽, 文件高) 同单位
    只登记"跨格数 ≤ max_span"的要素; 更长的 (比如溶解后的整条国道) 进 **global 表**,
    每帧都扫 —— 它们本来就几乎总是可见, 而且数量很少。

    返回 (bytes, meta)；meta 里 nx/ny 为 0 表示"这个文件不做索引"(要素太多, u16 装不下)。
    """
    n = len(fbb)
    if n < 24 or n > 0xFFFE:
        return b"", dict(nx=0, ny=0, n=0, g=0)
    # 每格目标 ~3 条要素。
    # ⚠ 格数别太小: "最小格 + 查询外扩 2 格" 的固有开销在小格数下会被放大 ——
    #   实测 16×16 时 z16 只能过滤掉 81% (7564→1400), 50×50 时能过滤 93% (7564→507)。
    #   索引本身约占文件 6~7%, 值。
    ax = int((n / 3.0) ** 0.5 + 0.5)
    ax = ncells_max if ax > ncells_max else (2 if ax < 2 else ax)
    nx = ny = ax
    cw = max(1, -(-span_w // nx))          # ceil
    ch = max(1, -(-span_h // ny))

    cells = [[] for _ in range(nx * ny)]
    gid = []
    for i, (bx0, by0, bx1, by1) in enumerate(fbb):
        cx0 = min(nx - 1, max(0, bx0 // cw))
        cx1 = min(nx - 1, max(0, bx1 // cw))
        cy0 = min(ny - 1, max(0, by0 // ch))
        cy1 = min(ny - 1, max(0, by1 // ch))
        if (cx1 - cx0) >= max_span or (cy1 - cy0) >= max_span:
            gid.append(i)
            continue
        cells[cy0 * nx + cx0].append(i)    # ★ 只进最小格

    start = [0] * (nx * ny + 1)
    fid = []
    for c in range(nx * ny):
        start[c] = len(fid)
        fid.extend(cells[c])
    start[nx * ny] = len(fid)

    # ---- 段布局 (小端, 设备端按固定偏移解) ----
    #   u8 iver | u8 pad | u16 nx | u16 ny
    #   i4 ox, oy, cw, ch           (微度; ox/oy 是网格原点 = 文件 min)
    #   u32 nsid | u32 gcount
    #   u32 cell_start[nx*ny+1]
    #   u16 fid[nsid]
    #   u16 gid[gcount]
    ox = origin[0]                     # 网格原点 (微度) —— 恒等于文件 min, 存下来便于自检
    oy = origin[1]
    out = bytearray()
    out += struct.pack("<BBHH", 1, 0, nx, ny)
    out += struct.pack("<iiii", ox, oy, cw * 10, ch * 10)     # 1e-5 度 → 微度
    out += struct.pack("<II", len(fid), len(gid))
    out += struct.pack("<%dI" % (nx * ny + 1), *start)
    if fid:
        out += struct.pack("<%dH" % len(fid), *fid)
    if gid:
        out += struct.pack("<%dH" % len(gid), *gid)
    return bytes(out), dict(nx=nx, ny=ny, n=len(fid), g=len(gid))


def pack_ltm(feats, bbox, layer=LAYER_ROAD, lods=None):
    """feats = [ {cls:int, pts:[(lon,lat),...], name:str|None}, ... ] → bytes"""
    if lods is None:
        lods = LODS
    min_lon, min_lat, max_lon, max_lat = bbox
    min_lon_e6 = int(round(min_lon * 1e6))
    min_lat_e6 = int(round(min_lat * 1e6))

    feats = [f for f in feats if len(f["pts"]) >= 1]     # 地标是**单点**要素 (npts=1)
    # ★ 按 cls 升序 —— 于是每个 LOD(前缀) 的要素区间天然连续、零冗余
    feats.sort(key=lambda f: (f["cls"], -len(f["pts"])))

    # ---- 字符串表 (同名共用一份) ----
    strtab = bytearray()
    smap = {}

    def sintern(s):
        if not s:
            return 0xFFFF
        v = smap.get(s)
        if v is not None:
            return v
        off = len(strtab)
        strtab.extend(s.encode("utf-8"))
        strtab.append(0)
        if off > 0xFFFE:
            raise SystemExit("字符串表超 64KB, 需要扩展 nameoff 字段")
        smap[s] = off
        return off

    # ---- 点表 + 要素表 ----
    ptbl = bytearray()
    fents = []
    fcls = []                      # 与 fents 一一对应 (LOD 前缀区间要用)
    fend = []                      # 每条要素点表的**结束偏移** (LOD 的 p1 要用)
    fbb = []                       # 每条要素的 bbox (1e-5 度, 相对文件 min) —— 建索引用
    for f in feats:
        # pack_ready 同时解决"点数 ≤ 255"和"段长 ≤ 3.3 km"两件事;
        # 超长的路会被拆成多条**同名**要素 (宁可多一条, 不能丢一截路)。
        for ch in pack_ready([(int(round(x * 1e6)), int(round(y * 1e6)))
                              for x, y in f["pts"]]):
            npts = len(ch)
            poff = len(ptbl)
            lon0, lat0 = ch[0]
            ptbl += struct.pack("<ii", lat0 - min_lat_e6, lon0 - min_lon_e6)
            plat, plon = lat0, lon0
            for lon, lat in ch[1:]:
                dlat, dlon = lat - plat, lon - plon
                if not (-32768 <= dlat <= 32767 and -32768 <= dlon <= 32767):
                    raise SystemExit(
                        "点增量溢出 i2: 段长 %.2f km (name=%s) —— pack_ready 没兜住"
                        % (max(abs(dlat), abs(dlon)) / 1e6 * 111.0, f.get("name")))
                ptbl += struct.pack("<hh", dlat, dlon)
                plat, plon = lat, lon
            lons = [p[0] for p in ch]
            lats = [p[1] for p in ch]
            bx0 = int(round((min(lons) - min_lon_e6) / 10.0))     # → 1e-5 度
            by0 = int(round((min(lats) - min_lat_e6) / 10.0))
            bx1 = int(round((max(lons) - min_lon_e6) / 10.0))
            by1 = int(round((max(lats) - min_lat_e6) / 10.0))
            for v in (bx0, by0, bx1, by1):      # i4 范围 ±21474°, 基本不会溢出
                if not (-2147483648 <= v <= 2147483647):
                    raise SystemExit("要素 bbox 溢出 i4")
            fents.append(struct.pack(FEAT_FMT, f["cls"], npts, sintern(f.get("name")),
                                     poff, bx0, by0, bx1, by1))
            fcls.append(f["cls"])
            fend.append(len(ptbl))
            fbb.append((bx0, by0, bx1, by1))

    # ---- LOD 目录 ----
    lod_ents = []
    for L in lods:
        sel = [i for i, c in enumerate(fcls) if c <= L["cls_max"]]
        if not sel:
            lod_ents.append((L, 0, 0, 0, 0))
            continue
        f0, f1 = sel[0], sel[-1] + 1
        p0 = struct.unpack_from("<I", fents[f0], 4)[0]
        p1 = fend[f1 - 1]                     # 已算好, 别再拿 feats 反推 (条数会错位)
        lod_ents.append((L, f0, f1, p0, p1))

    off_lod = HDR_LEN
    off_feat = off_lod + LOD_LEN * len(lods)
    off_pt = off_feat + FEAT_LEN * len(fents)
    off_str = off_pt + len(ptbl)

    lod_blob = bytearray()
    for L, f0, f1, p0, p1 in lod_ents:
        lod_blob += struct.pack(LOD_FMT, L["cls_max"], L["w"], L["zmin"], f0, f1, p0, p1)

    head = struct.pack(HDR_FMT, MAGIC, VER, layer, len(lods),
                       min_lat_e6, min_lon_e6,
                       int(round(max_lat * 1e6)), int(round(max_lon * 1e6)),
                       off_lod, off_feat, off_pt, off_str,
                       len(strtab), len(fents), len(ptbl) // 4, 0)
    # ---- 空间索引 (v3): 追加在字符串表之后, 偏移写在头部 56..59 (原填充位) ----
    idx, imeta = build_grid_index(
        fbb,
        (int(round(max_lon * 1e6)) - min_lon_e6) // 10,
        (int(round(max_lat * 1e6)) - min_lat_e6) // 10,
        origin=(min_lon_e6, min_lat_e6))
    off_idx = (off_str + len(strtab)) if idx else 0
    body = (bytes(head) + bytes(lod_blob) + b"".join(fents) + bytes(ptbl)
            + bytes(strtab) + idx)
    # ★ crc32 = **头部之后全部载荷**的校验和 (不含头部自己) —— 无自引用, 设备端
    #   只需 `crc32(file:sub(65))` 就能核, 不必先抠掉 crc 字段再算。
    crc = crc32(body[HDR_LEN:])
    body = body[:52] + struct.pack("<I", crc) + struct.pack("<I", off_idx) + body[60:]
    return body, crc


def parse_ltm(raw):
    """反向解析 (自检用)。返回 dict。"""
    if len(raw) < HDR_LEN or raw[:4] != MAGIC:
        raise SystemExit("不是 LTM1 文件")
    (magic, ver, layer, nlod, minlat, minlon, maxlat, maxlon,
     off_lod, off_feat, off_pt, off_str, len_str, nfeat, npt, crc) = \
        struct.unpack_from(HDR_FMT, raw, 0)
    lods = []
    for k in range(nlod):
        cmax, w, zmin, f0, f1, p0, p1 = struct.unpack_from(LOD_FMT, raw, off_lod + k * LOD_LEN)
        lods.append(dict(cls_max=cmax, w=w, zmin=zmin, f0=f0, f1=f1, p0=p0, p1=p1))
    feats = []
    for i in range(nfeat):
        cls, npts, noff, poff, bx0, by0, bx1, by1 = \
            struct.unpack_from(FEAT_FMT, raw, off_feat + i * FEAT_LEN)
        name = None
        if noff != 0xFFFF:
            e = raw.index(b"\0", off_str + noff)
            name = raw[off_str + noff:e].decode("utf-8")
        lat, lon = struct.unpack_from("<ii", raw, off_pt + poff)
        pts = [(lon / 1e6 + minlon / 1e6, lat / 1e6 + minlat / 1e6)]
        clat, clon = lat, lon
        for k in range(npts - 1):
            dl, dn = struct.unpack_from("<hh", raw, off_pt + poff + 8 + k * 4)
            clat += dl
            clon += dn
            pts.append((clon / 1e6 + minlon / 1e6, clat / 1e6 + minlat / 1e6))
        feats.append(dict(cls=cls, npts=npts, name=name, pts=pts,
                          bbox=(bx0, by0, bx1, by1)))
    r = dict(ver=ver, layer=layer, nlod=nlod, nfeat=nfeat, npt=npt, crc=crc,
             bbox=(minlon / 1e6, minlat / 1e6, maxlon / 1e6, maxlat / 1e6),
             lods=lods, feats=feats, str_len=len_str,
             off_idx=struct.unpack_from("<I", raw, 56)[0],
             file_crc=crc32(raw[HDR_LEN:]))
    return r


def parse_index(raw, d):
    """反解 v3 的网格索引 (自检用)。返回 dict 或 None。"""
    o = d.get("off_idx") or 0
    if not o or o >= len(raw):
        return None
    iver, _pad, nx, ny = struct.unpack_from("<BBHH", raw, o)
    ox, oy, cw, ch = struct.unpack_from("<iiii", raw, o + 6)
    nsid, gcount = struct.unpack_from("<II", raw, o + 22)
    p = o + 30
    start = struct.unpack_from("<%dI" % (nx * ny + 1), raw, p)
    p += 4 * (nx * ny + 1)
    fid = struct.unpack_from("<%dH" % nsid, raw, p) if nsid else ()
    p += 2 * nsid
    gid = struct.unpack_from("<%dH" % gcount, raw, p) if gcount else ()
    return dict(iver=iver, nx=nx, ny=ny, ox=ox, oy=oy, cw=cw, ch=ch,
                nsid=nsid, gcount=gcount, start=start, fid=fid, gid=gid)


# ==================================================================
# 设备端同款渲染 (预览 == 真机)
# ==================================================================
def _hline(plot, xa, xb, y, clip):
    if xb < xa:
        xa, xb = xb, xa
    cx0, cy0, cx1, cy1 = clip
    if xa < cx0:
        xa = cx0
    if xb > cx1:
        xb = cx1
    if xa > xb or y < cy0 or y > cy1:
        return
    for x in range(xa, xb + 1):
        plot(x, y)


def _vline(plot, x, ya, yb, clip):
    if yb < ya:
        ya, yb = yb, ya
    cx0, cy0, cx1, cy1 = clip
    if ya < cy0:
        ya = cy0
    if yb > cy1:
        yb = cy1
    if ya > yb or x < cx0 or x > cx1:
        return
    for y in range(ya, yb + 1):
        plot(x, y)


def draw_seg(plot, x0, y0, x1, y1, clip=(0, 0, 399, 299), w=1):
    """Bresenham + **同 y 连续段合并成一次 hline** (设备端 Lua 逐行同算法)。

    ★ 为什么必须合并: 设备端每个 app.hline 都是一次 Lua↔C 跨界调用。一条 400 px 的水平路
      逐点 app.px 要 400 次调用; 合并后**只 1 次**。这是"不卡顿"的关键。
    ★ w=2 的粗线: 按线方向沿**法向**偏移 1 px 再画一遍 —— 不能一律 +1 y,
      那样竖直的线根本不增粗 (只是被拉长)。
    """
    dx = x1 - x0
    dy = y1 - y0
    adx = dx if dx >= 0 else -dx
    ady = dy if dy >= 0 else -dy
    sx = 1 if dx >= 0 else -1
    sy = 1 if dy >= 0 else -1
    if adx >= ady:
        ox, oy = 0, 1                    # 近水平 → 向下加粗
    else:
        ox, oy = 1, 0                    # 近垂直 → 向右加粗
    for k in range(w):
        if adx >= ady:
            d = 2 * ady - adx
            x = x0
            y = y0
            run = x0
            while True:
                if x == x1:
                    break
                if d > 0:
                    _hline(plot, run, x, y + oy * k, clip)
                    run = x + sx
                    y += sy
                    d -= 2 * adx
                d += 2 * ady
                x += sx
            _hline(plot, run, x1, y + oy * k, clip)
        else:
            d = 2 * adx - ady
            x = x0
            y = y0
            run = y0
            while True:
                if y == y1:
                    break
                if d > 0:
                    _vline(plot, x + ox * k, run, y, clip)
                    run = y + sy
                    x += sx
                    d -= 2 * ady
                d += 2 * adx
                y += sy
            _vline(plot, x + ox * k, run, y1, clip)


def pick_lod(lods, z):
    """与 map.lua 同规则: 取最后一个 zmin<=z 的档"""
    sel = 0
    for i, L in enumerate(lods):
        if z >= L["zmin"]:
            sel = i
    return sel


def label_anchor(scr, min_len, W=400, H=300, top=24):
    """路名锚点 = 折线**按弧长**的中点 (只统计落在屏内的段)。

    ⚠ 不能用"最长单段的中点": OSM 的 way 被切成大量短段 (实测中位 **34 m**),
      在 z11 (69 m/px) 下 34 m 不到 1 px, 于是"最长段 ≥40px"的判据永远不成立 →
      缩略视图一个路名都标不出来。按**整条路的可见总长**判据 + 弧长取中点才稳。
    返回 (可见总长px, mx, my) 或 None。
    """
    seg = []
    tot = 0.0
    for i in range(len(scr) - 1):
        x0, y0 = scr[i]
        x1, y1 = scr[i + 1]
        if max(x0, x1) < 2 or min(x0, x1) > W - 2:
            continue
        if max(y0, y1) < top + 2 or min(y0, y1) > H - 2:
            continue
        s = math.hypot(x1 - x0, y1 - y0)
        seg.append((s, x0, y0, x1, y1))
        tot += s
    if tot < min_len or not seg:
        return None
    half = tot / 2.0
    acc = 0.0
    for s, x0, y0, x1, y1 in seg:
        if acc + s >= half:
            t = (half - acc) / s if s > 1e-9 else 0.0
            return tot, x0 + t * (x1 - x0), y0 + t * (y1 - y0)
        acc += s
    s, x0, y0, x1, y1 = seg[-1]
    return tot, (x0 + x1) / 2.0, (y0 + y1) / 2.0


def render_preview(raws, center, zoom, out, W=400, H=300, top=24,
                   labels=True, max_labels=14, min_len=26):
    """设备端渲染的 Python 复刻: 同一投影、同一 LOD 选择、同一画线、同一固件字库。

    raws = [.ltm 文件内容, ...] —— **先画等高线(底), 再画道路(上), 最后标路名**,
    与设备端 map.lua 的叠放顺序完全一致; 这样样张就是真机所见, 不是"艺术效果图"。
    """
    from PIL import Image
    ds = [parse_ltm(r) for r in raws]
    ds.sort(key=lambda d: 0 if d["layer"] == LAYER_CONTOUR else 1)   # 等高线垫底

    lat, lon = center
    kx = 0.711105 * (2 ** zoom)                  # 每度经度占多少像素 (墨卡托局部常数)
    ky = kx / math.cos(math.radians(lat))
    kxe = kx / 1e6
    kye = ky / 1e6
    clon = int(round(lon * 1e6))
    clat = int(round(lat * 1e6))
    ox = W / 2.0
    mh = H - top                                 # 地图区高度
    oy = top + mh / 2.0
    mpp = m_per_px(lat, zoom)

    def to_screen(lon_e6, lat_e6):
        return ox + (lon_e6 - clon) * kxe, oy - (lat_e6 - clat) * kye

    img = Image.new("1", (W, H), 1)                    # 1 = 白
    px = img.load()

    def plot(x, y):
        if 0 <= x < W and 0 <= y < H:
            px[x, y] = 0

    # 视口 (世界) 范围, 微度
    halfw = (W / 2.0) * 1e6 / kx
    halfh = (mh / 2.0) * 1e6 / ky
    vx0, vx1 = clon - halfw, clon + halfw
    vy0, vy1 = clat - halfh, clat + halfh

    named = []
    stat = []
    for d in ds:
        minlon_e6 = int(round(d["bbox"][0] * 1e6))
        minlat_e6 = int(round(d["bbox"][1] * 1e6))
        li = pick_lod(d["lods"], zoom)
        L = d["lods"][li]
        drawn = 0
        for f in d["feats"][L["f0"]:L["f1"]]:
            bx0, by0, bx1, by1 = f["bbox"]              # 单位 1e-5 度, 相对 min
            fx0 = minlon_e6 + bx0 * 10
            fx1 = minlon_e6 + bx1 * 10
            fy0 = minlat_e6 + by0 * 10
            fy1 = minlat_e6 + by1 * 10
            if fx1 < vx0 or fx0 > vx1 or fy1 < vy0 or fy0 > vy1:
                continue
            drawn += 1
            scr = [to_screen(int(round(p[0] * 1e6)), int(round(p[1] * 1e6))) for p in f["pts"]]
            # ★ 屏幕空间抽稀: 与 map.lua 的 MINSTEP 逐值同规则 (点距 < 3 px 不落笔,
            #   落笔时从"上次落笔点"直连到当前点 → 线不断)。样张必须同算法, 否则会骗人。
            ax, ay = scr[0]
            lx, ly = ax, ay
            for i in range(1, len(scr)):
                qx, qy = scr[i]
                if abs(qx - lx) + abs(qy - ly) >= 3:
                    draw_seg(plot, int(round(lx)), int(round(ly)),
                             int(round(qx)), int(round(qy)), (0, top, W - 1, H - 1), L["w"])
                    lx, ly = qx, qy
                ax, ay = qx, qy
            if abs(lx - ax) + abs(ly - ay) > 0:
                draw_seg(plot, int(round(lx)), int(round(ly)),
                         int(round(ax)), int(round(ay)), (0, top, W - 1, H - 1), L["w"])
            if f["name"]:
                named.append((f["name"], f["cls"], scr))
        stat.append("%s LOD%d(cls<=%d,w=%d) %d/%d条"
                    % ("等高" if d["layer"] == LAYER_CONTOUR else "道路",
                       li, L["cls_max"], L["w"], drawn, L["f1"] - L["f0"]))

    # ---- 路名 (按**弧长**取中点; 同名只留可见总长最长的一条) ----
    placed = 0
    if labels and named:
        best = {}
        for name, cls, scr in named:
            a = label_anchor(scr, min_len)
            if a is None:
                continue
            tot, mx, my = a
            if name not in best or tot > best[name][0]:
                best[name] = (tot, cls, mx, my)
        order = sorted(best.items(), key=lambda kv: (kv[1][1], -kv[1][0]))
        used = []
        for name, (tot, cls, mx, my) in order:
            if placed >= max_labels:
                break
            tw = fw_text_w(name)
            if not (tw / 2 + 3 <= mx <= W - tw / 2 - 3 and top + 11 <= my <= H - 11):
                continue
            if any(abs(mx - ux) < tw / 2 + 26 and abs(my - uy) < 17 for ux, uy in used):
                continue
            used.append((mx, my))
            placed += 1
            tx, ty = int(mx - tw / 2), int(my - 8)
            # 白底 1px 描边 → 压在路上也读得清
            for yy in range(ty - 1, ty + 17):
                for xx in range(tx - 1, tx + tw + 1):
                    if 0 <= xx < W and top <= yy < H:
                        px[xx, yy] = 1
            fw_text_px(px, tx, ty, name)

    img.save(out)
    print("  ✓ %s  %dx%d 1bpp | %s | 标名 %d | %.2f m/px"
          % (out, W, H, " ; ".join(stat), placed, mpp))
    return out


# ==================================================================
# 等高线 (terrarium DEM → marching squares)
# ==================================================================
# 角点: a=左上 b=右上 c=右下 d=左下;  边: 0=上(a-b) 1=右(b-c) 2=下(d-c) 3=左(a-d)
# bit0..3 = a/b/c/d 高于当前等值面。表值 = 该格要出的 (起点边, 终点边) 对。
MSTABLE = {
    1: ((3, 0),), 2: ((0, 1),), 3: ((3, 1),), 4: ((1, 2),),
    5: ((3, 0), (1, 2)),          # 鞍点 ×2
    6: ((0, 2),), 7: ((2, 3),), 8: ((2, 3),), 9: ((0, 2),),
    10: ((3, 0), (1, 2)),         # 鞍点 ×2
    11: ((0, 1),), 12: ((1, 2),), 13: ((3, 1),), 14: ((3, 0),),
}


def build_contours(elev, dem_info, bbox, interval, min_len=2):
    """等高线: terrarium DEM → marching squares → 折线 (经纬度)。

    实现要点
    --------
    ★ 逐格**先算高程跨度**, 只在 (lo, hi] 区间内的等值面上做 marching squares ——
      一块 1000×1000 的 DEM × 14 个等值面若全量扫是 1400 万次 Python 循环 (分钟级);
      按跨度剪枝后绝大多数格子只命中 0~1 个等值面, 快 10 倍以上。
    ★ 相邻格在同一条边上算出的是**同一个交点**, 所以按"交点像素坐标"哈希就能把
      线段首尾相接成长折线 (等高线是连续曲线, 不接就是满地碎线头)。
    """
    z, tx0, ty0 = dem_info
    Hn = len(elev)
    Wn = len(elev[0])
    n_ = 256.0 * (2 ** z)

    def px2lon(x):
        return (tx0 * 256 + x) / n_ * 360.0 - 180.0

    def px2lat(y):
        t = math.pi * (1 - 2 * (ty0 * 256 + y) / n_)
        return math.degrees(math.atan(math.sinh(t)))

    lo_all = min(min(r) for r in elev)
    hi_all = max(max(r) for r in elev)
    k_first = int(math.floor(lo_all / interval)) + 1
    k_last = int(math.floor(hi_all / interval))

    segs_by_lev = {}
    floor = math.floor
    for iy in range(Hn - 1):
        r0 = elev[iy]
        r1 = elev[iy + 1]
        for ix in range(Wn - 1):
            a = r0[ix]
            b = r0[ix + 1]
            c = r1[ix + 1]
            d = r1[ix]
            clo = a
            if b < clo:
                clo = b
            if c < clo:
                clo = c
            if d < clo:
                clo = d
            chi = a
            if b > chi:
                chi = b
            if c > chi:
                chi = c
            if d > chi:
                chi = d
            if chi <= clo:
                continue
            k0 = int(floor(clo / interval))
            k1 = int(floor(chi / interval))
            if k0 < k_first - 1:
                k0 = k_first - 1
            if k1 > k_last:
                k1 = k_last
            for k in range(k0, k1 + 1):
                lev = k * interval
                if lev <= clo or lev > chi:
                    continue
                idx = 0
                if a > lev:
                    idx |= 1
                if b > lev:
                    idx |= 2
                if c > lev:
                    idx |= 4
                if d > lev:
                    idx |= 8
                pairs = MSTABLE.get(idx)
                if not pairs:
                    continue
                x1f = ix + 1.0
                y1f = iy + 1.0
                for (ea, eb_) in pairs:
                    pts = []
                    for e in (ea, eb_):
                        if e == 0:
                            t = (lev - a) / (b - a) if b != a else 0.5
                            pts.append((ix + t, iy))
                        elif e == 1:
                            t = (lev - b) / (c - b) if c != b else 0.5
                            pts.append((x1f, iy + t))
                        elif e == 2:
                            t = (lev - d) / (c - d) if c != d else 0.5
                            pts.append((ix + t, y1f))
                        else:
                            t = (lev - a) / (d - a) if d != a else 0.5
                            pts.append((ix, iy + t))
                    segs_by_lev.setdefault(lev, []).append((pts[0], pts[1]))

    feats = []
    for lev, segs in sorted(segs_by_lev.items()):
        # 端点哈希 → 首尾相接
        ends = {}
        for k, (p, q) in enumerate(segs):
            ends.setdefault(_ek(p), []).append(k)
            ends.setdefault(_ek(q), []).append(k)
        used = [False] * len(segs)
        for s0 in range(len(segs)):
            if used[s0]:
                continue
            used[s0] = True
            chain = [segs[s0][0], segs[s0][1]]
            # 向前 (从尾端接)
            while True:
                nxt = _pop_unused(ends, _ek(chain[-1]), used, segs)
                if nxt is None:
                    break
                chain.append(nxt)
            # 向后 (从头端接, 结果用 reverse 插到前面)
            while True:
                nxt = _pop_unused(ends, _ek(chain[0]), used, segs)
                if nxt is None:
                    break
                chain.insert(0, nxt)
            if len(chain) < min_len:
                continue
            pts = [(px2lon(x), px2lat(y)) for x, y in chain]
            pts = dp_simplify(pts, 1e-6)
            if len(pts) < 2:
                continue
            # ★ cls 按等值线**粗细等级**分 (等高距 20 m 时):
            #     整百米 = 计曲线(cls0) · 整四十米 = 半距(cls1) · 其余首曲线(cls2)
            #   于是 LOD 机制直接复用 —— 缩略视图只画 100 m 计曲线, 放大才补 20 m 首曲线。
            #   ⚠ 别用 "整五十" 分档: 等高距 20 m 时 20k%50==0 与 20k%100==0 等价 (都要求 k%5==0),
            #     中间那一档会**整档空掉**。
            lv = int(round(lev))
            if lv % 100 == 0:
                cls = 0
            elif lv % 40 == 0:
                cls = 1
            else:
                cls = 2
            feats.append(dict(cls=cls, pts=pts, name=None))
    return feats


def _ek(p):
    """端点键 (像素坐标四舍五入到 1e-4 px —— 同一格边上的交点由同一算式得出, 必相同)"""
    return (round(p[0], 4), round(p[1], 4))


def _pop_unused(ends, key, used, segs):
    for k in ends.get(key, ()):
        if used[k]:
            continue
        used[k] = True
        p, q = segs[k]
        return q if _ek(p) == key else p
    return None


# ==================================================================
# main
# ==================================================================
def cmd_roads(a):
    bbox = tuple(float(x) for x in a.bbox.split(","))
    s, w, n, e = bbox
    clipbb = (w, s, e, n)
    els = fetch_roads(bbox, 'way["highway"]')
    print("  Overpass 返回 %d 条 way" % len(els))
    raw_pts = 0
    clipped_pts = 0
    feats = []
    for el in els:
        g = el.get("geometry") or []
        if len(g) < 2:
            continue
        t = el.get("tags", {})
        hw = t.get("highway", "")
        if hw in ("proposed", "construction", "razed", "abandoned"):
            continue
        cls = CLS_MAP.get(hw, CLS_OTHER)
        pts = [(p["lon"], p["lat"]) for p in g]
        raw_pts += len(pts)
        for sub in clip_polyline(pts, clipbb):
            clipped_pts += len(sub)
            sub = dp_simplify(sub, a.tol)
            feats.append(dict(cls=cls, pts=sub, name=t.get("name")))
    print("  裁剪+简化: %d 点 → %d 点 (含出界剔除), 折线 %d 条 (原 %d 条 way)"
          % (raw_pts, clipped_pts, len(feats), len(els)))
    blob, crc = pack_ltm(feats, clipbb, LAYER_ROAD)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, "wb").write(blob)
    print("  ✓ %s  %.1f KB  crc=%08X" % (a.out, len(blob) / 1024.0, crc))
    cmd_info(argparse.Namespace(**{"in": a.out}))


def cmd_poi(a):
    bbox = tuple(float(x) for x in a.bbox.split(","))
    s, w, n, e = bbox
    clipbb = (w, s, e, n)
    els = fetch_pois(bbox)
    print("  Overpass 返回 %d 条" % len(els))
    feats, noname = [], 0
    for el in els:
        t = el.get("tags", {})
        cls = poi_cls(t)
        if cls is None:
            continue
        lat, lon = el.get("lat"), el.get("lon")
        if lat is None and el.get("center"):
            lat, lon = el["center"]["lat"], el["center"]["lon"]
        if lat is None or lon is None:
            continue
        if not (s <= lat <= n and w <= lon <= e):
            continue                                   # 裁到范围
        nm = t.get("name")
        if not nm:
            noname += 1
            continue                                   # 没名字的点标不出来, 直接丢
        feats.append(dict(cls=cls, pts=[(lon, lat)], name=nm))
    print("  入选 %d 个地标 (丢掉无名 %d 个)" % (len(feats), noname))
    blob, crc = pack_ltm(feats, clipbb, LAYER_POI, lods=LODS_POI)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, "wb").write(blob)
    print("  ✓ %s  %.1f KB  crc=%08X" % (a.out, len(blob) / 1024.0, crc))
    cmd_info(argparse.Namespace(**{"in": a.out}))


def cmd_info(a):
    path = a.__dict__["in"]
    if isinstance(path, list):
        path = path[0]
    raw = open(path, "rb").read()
    d = parse_ltm(raw)
    print("  --- %s (%.1f KB, crc=%08X) ---" % (path, len(raw) / 1024.0, d["file_crc"]))
    bb = d["bbox"]
    print("  layer=%d  bbox=%.4f,%.4f ~ %.4f,%.4f  (%.1f × %.1f km)" % (
        d["layer"], bb[0], bb[1], bb[2], bb[3],
        (bb[2] - bb[0]) * 100.55, (bb[3] - bb[1]) * 111.32))
    print("  要素 %d 条 / 点 %d 个 / 字符串表 %.1f KB" % (d["nfeat"], d["npt"], d["str_len"] / 1024.0))
    from collections import Counter
    cnt = Counter(f["cls"] for f in d["feats"])
    nm = sum(1 for f in d["feats"] if f["name"])
    if d["layer"] == LAYER_CONTOUR: names = CLS_NAME_CONTOUR
    elif d["layer"] == LAYER_POI:   names = CLS_NAME_POI
    else:                            names = CLS_NAME
    print("  有路名 %d 条" % nm)
    for c in sorted(cnt):
        print("    cls%d %-14s %5d 条" % (c, names.get(c, "?"), cnt[c]))
    for i, L in enumerate(d["lods"]):
        print("    LOD%d  zmin>=%-2d cls<=%d  要素 [%d,%d) = %d 条  点表 [%d,%d)" % (
            i, L["zmin"], L["cls_max"], L["f0"], L["f1"], L["f1"] - L["f0"],
            L["p0"], L["p1"]))


def cmd_preview(a):
    raws = [open(p, "rb").read() for p in a.input]
    center = tuple(float(x) for x in a.center.split(","))
    render_preview(raws, center, a.zoom, a.out, labels=not a.no_labels,
                   max_labels=a.max_labels)


def cmd_contour(a):
    from map_build import fetch_dem
    bbox = tuple(float(x) for x in a.bbox.split(","))
    s, w, n, e = bbox
    clipbb = (w, s, e, n)
    elev, info = fetch_dem(bbox, a.demz)
    print("  DEM %d×%d, 高程 %.0f..%.0f m" % (
        len(elev[0]), len(elev),
        min(min(r) for r in elev), max(max(r) for r in elev)))
    raw = build_contours(elev, info, bbox, a.interval)
    npts0 = sum(len(f["pts"]) for f in raw)
    # ★ 裁剪 (DEM 瓦片覆盖范围大于请求 bbox) + 抽稀 + 去碎线
    tol = a.tol
    out = []
    for f in raw:
        for sub in clip_polyline(f["pts"], clipbb):
            ln = _deg_len(sub)
            if ln < a.minlen:
                continue
            sub = dp_simplify(sub, tol)
            if len(sub) < 2 or _deg_len(sub) < a.minlen:
                continue
            out.append(dict(cls=f["cls"], pts=sub, name=None))
    print("  marching squares → %d 条 / %d 点; 裁剪+抽稀+去碎 → %d 条 / %d 点 (长度 >=%.0f m)"
          % (len(raw), npts0, len(out), sum(len(f["pts"]) for f in out), a.minlen))
    blob, crc = pack_ltm(out, clipbb, LAYER_CONTOUR, lods=LODS_CONTOUR)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, "wb").write(blob)
    print("  ✓ %s  %.1f KB  crc=%08X" % (a.out, len(blob) / 1024.0, crc))
    cmd_info(argparse.Namespace(**{"in": a.out}))


def _deg_len(pts):
    """折线长度 (米), 按 25.4°N 的经纬度换算"""
    t = 0.0
    for i in range(len(pts) - 1):
        dx = (pts[i + 1][0] - pts[i][0]) * 100550.0
        dy = (pts[i + 1][1] - pts[i][1]) * 111320.0
        t += math.hypot(dx, dy)
    return t


def main():
    ap = argparse.ArgumentParser(description="LinTOS 离线矢量地图编译器")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("roads")
    p.add_argument("--bbox", default="25.36,118.95,25.50,119.10")
    p.add_argument("--out", default="dist/map/putian_road.ltm")
    p.add_argument("--tol", type=float, default=2e-6, help="Douglas-Peucker 容差(度), 默认 2e-6≈0.22m")
    p.set_defaults(fn=cmd_roads)

    p = sub.add_parser("contour")
    p.add_argument("--bbox", default="25.36,118.95,25.50,119.10")
    p.add_argument("--out", default="dist/map/putian_contour.ltm")
    p.add_argument("--interval", type=float, default=20.0, help="等高距(米)")
    p.add_argument("--tol", type=float, default=2e-5, help="抽稀容差(度), 默认 2e-5≈2.2m")
    p.add_argument("--minlen", type=float, default=150.0, help="丢弃短于该长度(米)的碎线")
    p.add_argument("--demz", type=int, default=13)
    p.set_defaults(fn=cmd_contour)

    p = sub.add_parser("poi")
    p.add_argument("--bbox", default="25.36,118.95,25.50,119.10")
    p.add_argument("--out", default="dist/map/putian_poi.ltm")
    p.set_defaults(fn=cmd_poi)

    p = sub.add_parser("preview")
    p.add_argument("--in", dest="input", action="append", required=True,
                   help="可给多次: 先给的垫底 (等高线), 后给的在上 (道路)")
    p.add_argument("--center", default="25.4335,119.0110")
    p.add_argument("--zoom", type=int, default=14)
    p.add_argument("--out", default="/tmp/vec_preview.png")
    p.add_argument("--no-labels", action="store_true")
    p.add_argument("--max-labels", type=int, default=14)
    p.set_defaults(fn=cmd_preview)

    p = sub.add_parser("info")
    p.add_argument("--in", dest="input", required=True)
    p.set_defaults(fn=cmd_info)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
