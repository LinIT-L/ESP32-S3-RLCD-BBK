#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""map_build.py — LinTOS 离线地图工具链 (莆田, 1bpp 400x300)

设计要点
--------
★ 数据源 = **矢量**(OSM Overpass), 不是栅格瓦片。
  栅格在 z18 全球是几百 TB 量级(Cardputer 项目实测), 矢量只存折线点 —— 莆田全境道路
  也就几 MB, 而且**任意缩放**都清楚。
★ 两条数据通路(已实测可通):
   ① 道路: Overpass API `way["highway"](bbox); out geom;` → 含 `name`(路名)
   ② 高程: AWS terrain tiles `elevation-tiles-prod/terrarium/{z}/{x}/{y}.png`
          高程 = R*256 + G + B/256 - 32768 (米)
★ 输出 = 1bpp。本项目屏是 **ST7305 反射式 400x300, 1 位=墨**。
  线用硬阈值画(细线不抖), 面(水/绿地)用 4x4 Bayer 抖动区分层次。

用法
----
  # 拉数据(带缓存) + 出预览图
  python3 tools/map_build.py --fetch  --bbox 25.40,118.98,25.47,119.06
  python3 tools/map_build.py --preview --center 25.4335,119.0110 --zoom 16 --out /tmp/pd.png
  python3 tools/map_build.py --tiles  --center 25.4335,119.0110 --zoom 16   # 生成瓦片
"""
import argparse, json, math, os, sys, urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "cache_map")
UA = "LinTOS-mapbuilder/1.0 (personal offline map project)"

# ---- 路网等级 → 画法。1bpp 只有黑白, 靠"线宽 + 虚实 + 双线"区分等级 ----
# w = 线宽(px), dy = 虚实(0=实线), dbl = 双线(主干道画两条细线更像路)
ROAD = {
    "motorway":       dict(w=3, dbl=True,  lvl=0, label=True),
    "trunk":          dict(w=3, dbl=True,  lvl=0, label=True),
    "primary":        dict(w=2, dbl=False, lvl=1, label=True),
    "secondary":      dict(w=2, dbl=False, lvl=2, label=True),
    "tertiary":       dict(w=1, dbl=False, lvl=3, label=True),
    "unclassified":   dict(w=1, dbl=False, lvl=4, label=False),
    "residential":    dict(w=1, dbl=False, lvl=4, label=False),
    "service":        dict(w=1, dbl=False, lvl=5, label=False),
    "living_street":  dict(w=1, dbl=False, lvl=4, label=False),
    "pedestrian":     dict(w=1, dbl=False, lvl=5, label=False),
    "footway":        dict(w=1, dbl=False, lvl=6, label=False),
    "path":           dict(w=1, dbl=False, lvl=6, label=False),
    "cycleway":       dict(w=1, dbl=False, lvl=6, label=False),
    "track":          dict(w=1, dbl=False, lvl=6, label=False),
    "steps":          dict(w=1, dbl=False, lvl=6, label=False),
}
DEFAULT_ROAD = dict(w=1, dbl=False, lvl=6, label=False)


# ================= Web Mercator =================
def lonlat_to_world(lon, lat, z):
    """经纬度 → Web Mercator 世界像素坐标 (zoom z, 256px/tile 的常规约定)"""
    n = 256.0 * (2 ** z)
    x = (lon + 180.0) / 360.0 * n
    s = math.sin(math.radians(lat))
    y = (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n
    return x, y


def m_per_px(lat, z):
    """该纬度、该 zoom 下, 1 像素多少米"""
    return 156543.03392 * math.cos(math.radians(lat)) / (2 ** z)


# ================= 数据拉取 =================
OVERPASS = "https://overpass-api.de/api/interpreter"


def fetch_roads(bbox, tag_filter='way["highway"]', timeout=180):
    """bbox = (south, west, north, east)。返回 Overpass JSON 的 elements 列表。"""
    s, w, n, e = bbox
    q = ('[out:json][timeout:%d];%s(%f,%f,%f,%f);out geom;'
         % (timeout, tag_filter, s, w, n, e))
    os.makedirs(CACHE, exist_ok=True)
    key = os.path.join(CACHE, "roads_%.4f_%.4f_%.4f_%.4f.json" % bbox)
    if os.path.exists(key):
        d = json.load(open(key, encoding="utf-8"))
        print("  用缓存 %s (%d 条)" % (os.path.relpath(key, ROOT), len(d.get("elements", []))))
        return d.get("elements", [])
    print("  拉 Overpass: %s ..." % tag_filter)
    req = urllib.request.Request(OVERPASS, data=urllib.parse.urlencode({"data": q}).encode(),
                                 headers={"User-Agent": UA,
                                          "Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=timeout + 30) as r:
        raw = r.read()
    d = json.loads(raw.decode("utf-8"))
    json.dump(d, open(key, "w", encoding="utf-8"))
    print("  ✓ %d 条, %.1f KB" % (len(d.get("elements", [])), len(raw) / 1024.0))
    return d.get("elements", [])


def fetch_dem(bbox, z=13):
    """下 terrarium 高程瓦片, 拼成 (高程数组, 覆盖 bbox 的经纬度范围)。"""
    from PIL import Image
    import io
    s, w, n, e = bbox
    xs, ys = [], []
    x0, y0 = lonlat_to_world(w, n, z)
    x1, y1 = lonlat_to_world(e, s, z)
    tx0, ty0 = int(x0 // 256), int(y0 // 256)
    tx1, ty1 = int(x1 // 256), int(y1 // 256)
    W, H = (tx1 - tx0 + 1) * 256, (ty1 - ty0 + 1) * 256
    big = Image.new("RGB", (W, H))
    got = 0
    os.makedirs(CACHE, exist_ok=True)
    for ty in range(ty0, ty1 + 1):
        for tx in range(tx0, tx1 + 1):
            url = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/%d/%d/%d.png" % (z, tx, ty)
            f = os.path.join(CACHE, "dem_%d_%d_%d.png" % (z, tx, ty))
            try:
                if os.path.exists(f):
                    im = Image.open(f)
                else:
                    req = urllib.request.Request(url, headers={"User-Agent": UA})
                    with urllib.request.urlopen(req, timeout=60) as r:
                        b = r.read()
                    open(f, "wb").write(b)
                    im = Image.open(io.BytesIO(b))
                big.paste(im.convert("RGB"), ((tx - tx0) * 256, (ty - ty0) * 256))
                got += 1
            except Exception as ex:
                print("    ! DEM 瓦片 %d/%d/%d 失败: %s" % (z, tx, ty, ex))
    print("  ✓ DEM %d 张 (%d×%d px, z%d)" % (got, W, H, z))
    # 高程数组 (纯 python 列表; 本项目环境没装 numpy)
    px = big.load()
    elev = [[px[x, y][0] * 256 + px[x, y][1] + px[x, y][2] / 256.0 - 32768
             for x in range(W)] for y in range(H)]
    return elev, (z, tx0, ty0)


# ================= 固件字库 (样张=真机) =================
_FW = None


def fw_fonts():
    """惰性加载固件字模。**路名用固件字库渲染**而不是系统 TTF ——
    真机上 Lua 只能显示固件的点阵字 (`app.mix` = 汉字 16×16 + ASCII 8×16),
    用系统字体画的样张跟真机对不上。这里直接读 font_zh16.bin / font8x16_zh.h,
    于是生成的瓦片里文字**已经烧好**, 设备端只需 blit, 零文字渲染开销。"""
    global _FW
    if _FW is None:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        from fw_fonts import FwFonts
        _FW = FwFonts()
    return _FW


def fw_text_w(s):
    fw = fw_fonts()
    w = 0
    for ch in s:
        w += 16 if (ord(ch) > 0x2E80 and fw.has_zh16(ch)) else 8
    return w


def fw_text_px(px, x, y, s):
    """在灰度图 (0=墨 255=纸) 上画一行文字, 与固件 app.mix 逐点一致。
    宽度规则同 app.mix: 汉字 16 宽, ASCII 8 宽 (字库缺字也留 16 宽, 与固件"找不到就不画"一致)。
    ⚠ px 是 PIL PixelAccess, 只能 px[x,y]=v 下标赋值, **不能当函数调**。"""
    fw = fw_fonts()
    cx = x
    for ch in s:
        if ord(ch) > 0x2E80:
            if fw.has_zh16(ch):
                for r in range(16):
                    for c in range(16):
                        if fw.zh16_bit(ch, r, c):
                            px[cx + c, y + r] = 0
            cx += 16
        else:
            if 32 <= ord(ch) <= 126:
                for r in range(16):
                    for c in range(8):
                        if fw.ascii8x16_bit(ch, r, c, rows=16):
                            px[cx + c, y + r] = 0
            cx += 8
    return cx


# ================= 画线 (纯 PIL, 1bpp) =================
BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def draw_polyline(d, pts, w=1, dash=0):
    """画折线。d = PIL ImageDraw (1 模式, 但是 8 位灰度更像方便: 用 255=白 0=黑)。"""
    if len(pts) < 2:
        return
    acc = 0
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        seg = math.hypot(x1 - x0, y1 - y0)
        if dash:
            # 简单虚实: 按累计长度决定这一段画不画
            on = int((acc + seg) / dash) % 2 == 0
            acc += seg
            if not on:
                continue
        if w <= 1:
            d.line([(x0, y0), (x1, y1)], fill=0, width=1)
        else:
            d.line([(x0, y0), (x1, y1)], fill=0, width=w)


def render(roads, center, zoom, out, W=400, H=300, dashes=None,
           elev=None, dem_info=None, contours=None, labels=True):
    from PIL import Image, ImageDraw, ImageFont
    lat, lon = center
    mpp = m_per_px(lat, zoom)                 # 米/像素
    cx, cy = lonlat_to_world(lon, lat, zoom)

    img = Image.new("L", (W, H), 255)         # 255=白(纸)
    d = ImageDraw.Draw(img)

    def w2s(px, py):
        """世界像素(zoom z) → 屏幕像素"""
        return px - (cx - W / 2.0), py - (cy - H / 2.0)

    # ---------- ① 等高线(可选, 画在路下面) ----------
    if elev is not None and dem_info and contours:
        z, tx0, ty0 = dem_info
        base_x = tx0 * 256.0
        base_y = ty0 * 256.0
        for lev, dw in contours:
            for y in range(1, len(elev) - 1):
                for x in range(1, len(elev[0]) - 1):
                    a = elev[y][x]
                    if abs(a - lev) <= 0:      # 简化: 只打"恰好跨过"的点(后续换 marching squares)
                        sx, sy = w2s(base_x + x, base_y + y)
                        if 0 <= sx < W and 0 <= sy < H:
                            d.point((sx, sy), fill=0)

    # ---------- ② 道路 ----------
    placed = 0
    named = []
    for el in roads:
        g = el.get("geometry") or []
        if len(g) < 2:
            continue
        t = el.get("tags", {})
        hw = t.get("highway", "")
        st = ROAD.get(hw, DEFAULT_ROAD)
        pts = [w2s(*lonlat_to_world(p["lon"], p["lat"], zoom)) for p in g]
        # 视口裁剪
        if not any(-50 <= x <= W + 50 and -50 <= y <= H + 50 for x, y in pts):
            continue
        draw_polyline(d, [(round(x), round(y)) for x, y in pts], w=st["w"])
        placed += 1
        if st["label"] and t.get("name"):
            named.append((t["name"], st["lvl"], pts))

    # ---------- ③ 路名 (用固件字库, 样张=真机) ----------
    if labels and named:
        px = img.load()
        # ★ 同名道路去重: OSM 里一条路常被切成很多 way, 同一个 name 会出现十几段。
        #   只保留**最长的那段**作为标注锚点, 否则屏幕上会出现 4 个"荔园南路"。
        best = {}
        for name, lvl, pts in named:
            ln = 0.0
            for i in range(len(pts) - 1):
                ln += math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
            if name not in best or ln > best[name][1]:
                best[name] = (lvl, ln, pts)
        uniq = [(nm, v[0], v[2]) for nm, v in best.items()]

        used = []
        uniq.sort(key=lambda z: (z[1], -len(z[2])))     # 先标等级高的, 同级先标长的
        for name, lvl, pts in uniq:
            if len(used) > 16:
                break
            i = len(pts) // 2                            # 取折线中点
            x, y = pts[i]
            tw, th = fw_text_w(name), 16
            # 屏幕内 + 留 3px 边
            if not (tw / 2 + 3 <= x <= W - tw / 2 - 3 and th / 2 + 3 <= y <= H - th / 2 - 3):
                continue
            # 与已标注的不重叠 (16px 字更大, 间距也要够)
            if any(abs(x - ux) < tw / 2 + 42 and abs(y - uy) < 18 for ux, uy in used):
                continue
            used.append((x, y))
            tx, ty = int(x - tw / 2), int(y - th / 2)
            # 白底 (压在路线上也看得清) + 固件字库黑字
            for yy in range(ty - 2, ty + th + 2):
                for xx in range(tx - 2, tx + tw + 2):
                    if 0 <= xx < W and 0 <= yy < H:
                        px[xx, yy] = 255
            fw_text_px(px, tx, ty, name)

    # ---------- ④ 出 1bpp ----------
    bw = img.point(lambda v: 0 if v < 128 else 255, "1")
    bw.save(out)
    print("  ✓ %s  %dx%d  1bpp  画了 %d 条路, 标了 %d 个路名  (%.1f m/px)"
          % (out, W, H, placed, min(len(used), len(named)), mpp))
    return out


# ================= main =================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--bbox", default="25.38,118.97,25.48,119.06")
    ap.add_argument("--center", default="25.4335,119.0110")
    ap.add_argument("--zoom", type=int, default=16)
    ap.add_argument("--out", default="/tmp/map_preview.png")
    ap.add_argument("--labels", action="store_true", default=True)
    a = ap.parse_args()

    bbox = tuple(float(x) for x in a.bbox.split(","))
    center = tuple(float(x) for x in a.center.split(","))
    roads = fetch_roads(bbox)
    render(roads, center, a.zoom, a.out)


if __name__ == "__main__":
    main()
