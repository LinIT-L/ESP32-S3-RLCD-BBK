#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""qqt_bw.py — 彩色原图 → 1bpp 单色屏 的**高质量**转换器 (QQ堂素材专用)。

为什么不用"全局分位数阈值"
--------------------------
第一版用的是 `thresh_ratio(rows, target)`：取一个灰度分位数当阈值, 让墨点占比≈target。
问题 (用户在真机上一眼看出来的"太粗糙")：
  · 这是**硬阈值**, 中间调全被压成纯黑或纯白 → 立体感/渐变全丢, 图标糊成一块块黑疙瘩;
  · 阈值一变, 整张图的墨量整体涨落 → 同一批图标有的很黑有的很白, 风格不统一;
  · 缩放用最近邻: **缩小**(如 45×36 → 24×24) 会丢线并产生锯齿噪点, 是"粗糙"的主因之一。

这里换成三步:
  ① **重采样**: 放大/缩小都用 PIL 的 LANCZOS (高质量滤波器), 缩小时先做面积平均, 不产生锯齿;
  ② **色调曲线**: 把原图的 [暗部, 亮部] 拉伸到满量程, 再按需要加 gamma;
  ③ **8×8 有序抖动 (Bayer / halftone)**: 用一个 64 级阈值矩阵去和灰度比,
     再乘一个增益系数把**平均墨量**压到目标值。
     结果: 灰阶被表达成**规则的点阵疏密**(像报纸印刷), 而不是随机噪点或大色块 ——
     在 1bpp 反射屏上这是最"精细"的表达方式, 也最接近原版那种网点质感。
"""
import math
from PIL import Image

# ---------------------------------------------------------------- 8×8 Bayer
_BAYER8 = [
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
]
# 归一化到 (0,1)：用 (v+0.5)/64 让阈值分布在格子中心, 避免 0 导致纯白区被误判成墨
_BAYER_F = [[(v + 0.5) / 64.0 for v in row] for row in _BAYER8]


# ---------------------------------------------------------------- 基本转换
def rows_to_img(rows):
    h = len(rows)
    w = len(rows[0]) if h else 0
    im = Image.new("L", (w, h))
    im.putdata([int(v) for r in rows for v in r])
    return im


def img_to_rows(im):
    w, h = im.size
    px = list(im.getdata())
    return [px[y * w:(y + 1) * w] for y in range(h)]


def resample(rows, dw, dh, box=None):
    """LANCZOS 重采样 (先裁剪 box 再缩放)。放大缩小都走它, 不要用最近邻。"""
    im = rows_to_img(rows)
    if box:
        im = im.crop(box)
    return img_to_rows(im.resize((dw, dh), Image.LANCZOS))


def fit(rows, dw, dh, box=None):
    """**等比**铺满 dw×dh 再居中裁切 —— 原版背景 150×130 是 1.15:1 的方图,
    画面 4:3; 直接拉伸会把圆的东西压扁。"""
    im = rows_to_img(rows)
    if box:
        im = im.crop(box)
    sw, sh = im.size
    s = max(dw / float(sw), dh / float(sh))
    mw, mh = int(sw * s + 0.5), int(sh * s + 0.5)
    im = im.resize((mw, mh), Image.LANCZOS)
    x0 = max(0, (mw - dw) // 2)
    y0 = max(0, (mh - dh) // 2)
    return img_to_rows(im.crop((x0, y0, x0 + dw, y0 + dh)))


def stretch(rows, lo_pct=0.05, hi_pct=0.95, gamma=1.0):
    """把 [lo_pct, hi_pct] 分位拉伸到满量程 0..255。γ<1 提亮, γ>1 压暗。"""
    vals = sorted(v for r in rows for v in r)
    if not vals:
        return rows
    lo = vals[min(len(vals) - 1, int(lo_pct * len(vals)))]
    hi = vals[min(len(vals) - 1, int(hi_pct * len(vals)))]
    if hi <= lo:
        hi = lo + 1
    inv = 1.0 / (hi - lo)
    out = []
    for r in rows:
        rr = []
        for v in r:
            t = (v - lo) * inv
            t = 0.0 if t < 0 else (1.0 if t > 1 else t)
            if gamma != 1.0:
                t = t ** gamma
            rr.append(int(t * 255))
        out.append(rr)
    return out


def dither(rows, ink=0.22, gamma=1.0):
    """有序抖动 → 0/1 墨点。墨点占比 ≈ ink (按平均暗度等比缩放, 保留相对层次)。

    ink 就是"这张图在屏幕上该有多少黑"。0.10 = 极淡纹理, 0.30 = 明显的画面,
    0.55+ = 主体很重。**不要**用它表达"对比度", 对比度交给 stretch 的 gamma。"""
    h = len(rows)
    w = len(rows[0]) if h else 0
    # 每像素的"暗度" d ∈ [0,1]
    dark = [[1.0 - (v / 255.0) for v in r] for r in rows]
    if gamma != 1.0:
        dark = [[d ** gamma for d in r] for r in dark]
    m = sum(v for r in dark for v in r) / float(w * h) if w * h else 0.0
    if m <= 1e-6:
        return [[0] * w for _ in range(h)]
    k = ink / m
    out = []
    for y in range(h):
        row = []
        for x in range(w):
            d = dark[y][x] * k
            if d > 1.0:
                d = 1.0
            row.append(1 if d > _BAYER_F[y & 7][x & 7] else 0)
        out.append(row)
    return out


def hard_threshold(rows, ink=0.30):
    """硬阈值: 最暗的 ink 比例 → 墨。小尺寸图标(≤40px)用这个, **不要抖动**。

    为什么图标不能抖动: 抖动是靠"点阵疏密"表达灰阶, 需要足够的像素面积才成"纹样";
    24~40px 的图标抖完只剩噪声, 用户看到的就是"一团粗糙的黑疙瘩"。
    硬阈值给出**边界干净**的实心形状 + 内部留白(高光/镂空), 这才是 1bpp 图标该有的样子。
    背景这类"照片"反过来 —— 必须抖动, 不然整片糊成黑或白。
    """
    vals = sorted((1.0 - v / 255.0) for r in rows for v in r)
    n = len(vals)
    if not n:
        return []
    q = vals[min(n - 1, int((1.0 - ink) * n))]
    return [[1 if (1.0 - v / 255.0) >= q else 0 for v in r] for r in rows]


def to_bits(rows, dw, dh, ink=0.22, gamma=1.0, lo=0.05, hi=0.95, box=None,
            outline=0, invert=False, use_dither=True):
    """标准流程: 重采样 → (可选反相) → 色调拉伸 → 抖动/硬阈值 → (可选描边)。

    invert=True: 原图是"黑底亮主体"(QQ堂的图标/表情/世界图标全是这种) →
                 先把灰度反相, 让主体变成暗的, 再当"墨"处理。
    use_dither=False: 走硬阈值 (图标/表情用) —— 见 hard_threshold 的说明。
    ⚠ 形参不能叫 dither —— 会和下面同名的 dither() 函数撞名 → 'bool' object is not callable。
    outline=1:   给墨团外圈补 1px 墨线, 缩小后边缘发虚时能把形状"框住"。
    """
    g = fit(rows, dw, dh, box) if box else resample(rows, dw, dh)
    if invert:
        g = [[255 - v for v in r] for r in g]
    g = stretch(g, lo, hi)
    if use_dither:
        bits = dither(g, ink=ink, gamma=gamma)
    else:
        bits = hard_threshold(g, ink=ink)
    if outline > 0:
        bits = add_outline(bits, outline)
    return bits


def add_outline(bits, t=1):
    """给墨团外圈加 t 圈墨 (只在原本是白的地方加, 不向内吞)。"""
    h = len(bits)
    w = len(bits[0]) if h else 0
    cur = [row[:] for row in bits]
    for _ in range(t):
        nxt = [row[:] for row in cur]
        for y in range(h):
            for x in range(w):
                if cur[y][x]:
                    continue
                n = 0
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < h and 0 <= xx < w and cur[yy][xx]:
                        n += 1
                if n >= 1:
                    nxt[y][x] = 1
        cur = nxt
    return cur


def _sobel(rows):
    """梯度幅值 (0..255)。用来做"线描"：只画轮廓与内部暗纹，不整块填黑。"""
    h = len(rows)
    w = len(rows[0]) if h else 0
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            a = rows[y - 1][x] if y > 0 else rows[y][x]
            b = rows[y + 1][x] if y + 1 < h else rows[y][x]
            c = rows[y][x - 1] if x > 0 else rows[y][x]
            d = rows[y][x + 1] if x + 1 < w else rows[y][x]
            gx = d - c
            gy = b - a
            m = abs(gx) + abs(gy)
            out[y][x] = 255 if m > 255 else m
    return out


def to_line(rows, dw, dh, ink=0.14, lo=0.05, hi=0.95, box=None, invert=False, thick=1):
    """**线描**转换：输出"白底 + 细黑线"的图，而不是实心黑块。

    什么时候用: 图标 / 表情 / 世界图标这类"3D 渲染的小物件"。
    实心硬阈值在 24~48px 上只会得到一个黑疙瘩（用户原话："太黑了、太糊了"）；
    线描保留轮廓 + 内部暗纹，在单色屏上"一眼看清是什么"，墨量只有 0.10~0.18。
    做法: LANCZOS 重采样 → 拉伸 → **Sobel 梯度** → 按目标墨量取分位 → 可选加粗 1 圈。
    """
    g = fit(rows, dw, dh, box) if box else resample(rows, dw, dh)
    if invert:
        g = [[255 - v for v in r] for r in g]
    g = stretch(g, lo, hi)
    e = _sobel(g)
    vals = sorted(v for r in e for v in r)
    n = len(vals)
    if not n:
        return []
    q = vals[min(n - 1, int((1.0 - ink) * n))]
    bits = [[1 if v >= q else 0 for v in r] for r in e]
    if thick > 0:
        bits = add_outline(bits, thick)     # 加粗轮廓, 避免虚线感
    return bits


def denoise(bits, t=1):
    """去掉孤立墨点 (t 圈内邻居都没墨就删掉) —— 抖动在平坦区会掉单点, 清掉更干净。"""
    h = len(bits)
    w = len(bits[0]) if h else 0
    out = [row[:] for row in bits]
    for y in range(h):
        for x in range(w):
            if not bits[y][x]:
                continue
            n = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dy == 0 and dx == 0:
                        continue
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < h and 0 <= xx < w and bits[yy][xx]:
                        n += 1
            if n < t:
                out[y][x] = 0
    return out
