#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""qqt_spr.py — 程序化绘制《QQ堂》掌机版的人物与怪物精灵。

为什么不用原版素材直接缩
------------------------
原版是 3D 渲染的大图，缩到 24px 二值化只会得到黑疙瘩（用户原话"人物也非常丑"）。
这里改成**先用 PIL 在 4 倍尺寸上画**（96×96，圆角/圆头/大眼睛都好画），
再用 LANCZOS 缩到 24×24 → 自动得到抗锯齿边缘，阈一下就是干净的 1bpp 精灵。
这就是"低分辨率手绘"的老办法：**在高分辨率上画，缩下来**。

产出 `spr.bmp`（一张图集，12 帧横排 @24px）：
  0..7  人物:  index = dir*2 + frame    (dir: 0上 1下 2左 3右; frame: 0/1 两个走路姿态)
  8..11 怪物:  0近战冲锋 1元素师 2引爆者 3BOSS
"""
from PIL import Image, ImageDraw

S = 4                 # 超采样倍数
PX = 24               # 最终单帧尺寸
BIG = PX * S          # 绘制尺寸
BLACK = 0
WHITE = 255


def _new():
    im = Image.new("L", (BIG, BIG), WHITE)
    return im, ImageDraw.Draw(im)


def _hero(direction, frame):
    """人物: 大头 + 小身 + 手脚。方向 0上 1下 2左 3右。

    ⛔ 关键: **头和身子之间必须留白色间隙**, 手脚也要和身子分开 ——
       全部涂成一坨黑的话 24px 下就是个"带耳朵的黑方块"(第一版就是这么丑的)。
    """
    im, d = _new()
    cx = BIG // 2
    # 耳朵 (贴着头)
    d.ellipse([cx - 32, 16, cx - 18, 30], fill=BLACK)
    d.ellipse([cx + 18, 16, cx + 32, 30], fill=BLACK)
    # 头 (实心黑圆, 直径 44/96 ≈ 11px)
    d.ellipse([cx - 22, 6, cx + 22, 50], fill=BLACK)
    # 身子 (窄一点, 与头留白)
    d.rounded_rectangle([cx - 13, 58, cx + 13, 82], radius=7, fill=BLACK)
    # 手 (小球, 和身子脱开)
    d.ellipse([cx - 32, 58, cx - 18, 74], fill=BLACK)
    d.ellipse([cx + 18, 58, cx + 32, 74], fill=BLACK)
    # 脚
    if frame == 0:
        d.rounded_rectangle([cx - 13, 86, cx - 2, 96], radius=3, fill=BLACK)
        d.rounded_rectangle([cx + 2, 86, cx + 13, 96], radius=3, fill=BLACK)
    else:
        d.rounded_rectangle([cx - 17, 86, cx - 5, 95], radius=3, fill=BLACK)
        d.rounded_rectangle([cx + 5, 87, cx + 17, 96], radius=3, fill=BLACK)
    if direction != 0:                       # 0=上 = 背对, 不画脸
        ex = 0
        if direction == 2: ex = -6
        elif direction == 3: ex = 6
        # 眼白
        d.ellipse([cx - 17 + ex, 18, cx - 3 + ex, 34], fill=WHITE)
        d.ellipse([cx + 3 + ex, 18, cx + 17 + ex, 34], fill=WHITE)
        # 瞳孔
        d.ellipse([cx - 14 + ex, 22, cx - 6 + ex, 31], fill=BLACK)
        d.ellipse([cx + 6 + ex, 22, cx + 14 + ex, 31], fill=BLACK)
        # 嘴 (白弧)
        d.ellipse([cx - 8, 36, cx + 8, 48], fill=WHITE)
        d.rectangle([cx - 8, 36, cx + 8, 41], fill=BLACK)
    else:
        d.rectangle([cx - 2, 0, cx + 2, 10], fill=BLACK)     # 背对: 呆毛
    return im


def _mon(kind):
    """怪物: 4 种, 形状各自不同, 一眼能分。"""
    im, d = _new()
    cx = BIG // 2
    if kind == 0:                            # 近战冲锋: 尖角球
        d.ellipse([cx - 28, 22, cx + 28, 78], fill=BLACK)
        d.polygon([(cx - 22, 30), (cx - 12, 4), (cx - 2, 28)], fill=BLACK)
        d.polygon([(cx + 2, 28), (cx + 12, 4), (cx + 22, 30)], fill=BLACK)
        d.ellipse([cx - 18, 38, cx - 4, 52], fill=WHITE)
        d.ellipse([cx + 4, 38, cx + 18, 52], fill=WHITE)
        d.ellipse([cx - 13, 42, cx - 8, 49], fill=BLACK)
        d.ellipse([cx + 8, 42, cx + 13, 49], fill=BLACK)
        d.rectangle([cx - 12, 60, cx + 12, 65], fill=WHITE)
    elif kind == 1:                          # 元素师: 悬浮环 + 单眼
        d.ellipse([cx - 18, 8, cx + 18, 44], fill=BLACK)
        d.ellipse([cx - 8, 18, cx + 8, 34], fill=WHITE)
        d.ellipse([cx - 4, 22, cx + 4, 30], fill=BLACK)
        d.ellipse([cx - 30, 50, cx + 30, 70], outline=BLACK, width=6)
        d.ellipse([cx - 10, 52, cx + 10, 68], fill=BLACK)
    elif kind == 2:                          # 引爆者: 方壳 + 十字
        d.rounded_rectangle([cx - 26, 24, cx + 26, 76], radius=8, fill=BLACK)
        d.rectangle([cx - 3, 30, cx + 3, 70], fill=WHITE)
        d.rectangle([cx - 20, 47, cx + 20, 53], fill=WHITE)
        d.rectangle([cx - 8, 8, cx + 8, 24], fill=BLACK)
    else:                                    # BOSS: 大块 + 王冠
        d.ellipse([cx - 34, 26, cx + 34, 86], fill=BLACK)
        d.polygon([(cx - 28, 30), (cx - 20, 2), (cx - 8, 22)], fill=BLACK)
        d.polygon([(cx - 8, 22), (cx, 0), (cx + 8, 22)], fill=BLACK)
        d.polygon([(cx + 8, 22), (cx + 20, 2), (cx + 28, 30)], fill=BLACK)
        d.ellipse([cx - 22, 48, cx - 6, 64], fill=WHITE)
        d.ellipse([cx + 6, 48, cx + 22, 64], fill=WHITE)
        d.ellipse([cx - 16, 52, cx - 10, 60], fill=BLACK)
        d.ellipse([cx + 10, 52, cx + 16, 60], fill=BLACK)
        d.rectangle([cx - 14, 70, cx + 14, 76], fill=WHITE)
    return im


def frame_bits(im, px=PX, thresh=128):
    """超采样图 → px×px 的 0/1 墨点矩阵 (1 = 墨)。"""
    small = im.resize((px, px), Image.LANCZOS)
    data = list(small.getdata())
    return [[1 if data[y * px + x] < thresh else 0 for x in range(px)] for y in range(px)]


N_HERO, N_MON = 8, 4


def build_frames():
    """返回 [24×24 的 0/1 矩阵] × 12 (前 8 人物, 后 4 怪物)。"""
    out = []
    for di in range(4):                      # 0上 1下 2左 3右
        for fr in range(2):
            out.append(frame_bits(_hero(di, fr)))
    for k in range(N_MON):
        out.append(frame_bits(_mon(k)))
    return out


if __name__ == "__main__":
    fr = build_frames()
    for i, m in enumerate(fr):
        print("--- frame", i)
        for row in m:
            print("  " + "".join("#" if v else "." for v in row))
