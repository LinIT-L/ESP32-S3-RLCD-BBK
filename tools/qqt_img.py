#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""qqt_img.py — 解析 QQ堂(QQTang v5.2) 客户端的 `.img` 资源格式 (DIMG)。

为什么写它
----------
用户要求"深度参考这个使用它的 UI"。解出来的客户端里 UI 图有 505 张，但**全是自有格式**
`QQF\\x1aDIMG`，没有任何常规图片（全树只有 4 个 bmp/gif）。所以先把格式吃透。

★ 格式已破 (2026-10-03)
--------------------
每帧 = **[w×h 个 RGB565 小端]** + **[w×h 个附加字节]** 两块分开存, 不是"3 字节交错"。

  判据链 (每一步都有反例被排除):
    ① 平坦色块在数据里是 `84 00 84 00 …` → 是 **RGB565 常量 0x0084**, 不是 RGB888 乱码;
    ② 头里 f[14] 行字节数 = 帧宽×2 (44→88) → 彩色部分按 **2 字节/像素** 逐行排;
    ③ 先按"3 字节交错"读 → 出来是**严格 1 像素棋盘**, 正是"2 字节像素按 3 字节跨读"的干涉纹;
    ④ 两块之和 = 帧数 × w × h × 3, 与文件长度严丝合缝;
    ⑤ 拼成总览图后图标全部清晰可辨 (会员/普通按钮、道具、吉祥物、播放条 `>>>`/`测试中`)。

⚠ **第三个字节不要当显示 alpha**: 拿它合成到白底会把整张图洗成"水印级"浅灰
  (形状都在但看不清)。默认忽略, 需要时用 `--alpha` 打开。

⛔ 仍有 **609/908 未解**: 它们的体量比值不是整数倍 (如 17.02 / 2.706 / 4.0),
   是**压缩变体** (大概率行级 RLE)。下一步应从 `python23.dll` + `boost_python.dll` 的
   调用侧, 或反编译 `res/uiRes/ui*.pyc` 找到解压入口 —— 不要再靠长度猜。

用法
----
    python3 tools/qqt_img.py list <UI资源根目录>   # 资源清单(名称+尺寸+帧数) ← 现在最有用的功能
    python3 tools/qqt_img.py info  <file.img>      # 打印头部字段
    python3 tools/qqt_img.py png   <file.img> -o out.png [--frame N]   # ⚠ 像素解释未验证
"""
import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sprite_sheet import write_png


def parse(path):
    d = open(path, "rb").read()
    if d[:8] != b"QQF\x1aDIMG":
        return None
    if len(d) < 68:
        return None
    f = struct.unpack_from("<15I", d, 8)
    w, h, n, bpp = f[6], f[7], f[2], f[1]
    fw, fh = f[12], f[13]
    body = len(d) - 68
    # ★★ 2026-10-03 关键修正: 每帧尺寸用 **f[12]×f[13]**(帧), 不是 f[6]×f[7](画布)。
    #   两者经常不同(画布带留白): 例如 face/faces/000.img 画布 37×33 而帧 37×31。
    #   我先前一律用画布尺寸 → 470 个文件被误判成"压缩变体"; 实际上它们**根本没压缩**,
    #   payload 开头就是 RGB565 像素 (ff ff = 白)。
    #   正确口径: data = N 帧 × (fw×fh×bpB), 之后**可能**还有一小段尾部(实测 28/56/112/140 B,
    #   内容未定, 直接跳过即可)。
    fw = fw or w
    fh = fh or h
    best = None
    for (W, H) in ((fw, fh), (w, h)):
        for B in (3, 4, 2):
            if not W or not H or not n:
                continue
            total = W * H * B * n
            rest = body - total
            if 0 <= rest <= 2048 and (best is None or rest < best[3]):
                best = (W, H, B, rest)
    exact = best is not None
    trailer = best[3] if best else 0
    return {
        "file": path, "fields": f, "w": w, "h": h, "frames": n, "bpp": bpp,
        "fw": fw, "fh": fh, "rowbytes": f[14],
        "dw": best[0] if best else 0, "dh": best[1] if best else 0,
        "bppb": best[2] if best else 0,
        "payload": body, "expect": (best[0] * best[1] * best[2] * n if best else None),
        "trailer": trailer, "exact": exact, "data": d,
    }


def decode_frame(im, idx=0):
    """第 idx 帧 → rows[list[list[int]]] 灰度 (0..255)。

    ★★ 2026-10-03 定案 (先前两版都错, 记录判据免得再绕):
      每帧 = **[dw×dh 个 RGB565 小端]** + **[dw×dh 个附加字节]**, 两块**分开存**;
      dw/dh 取头里的 **f[12]/f[13]**(帧尺寸), **不是 f[6]/f[7]**(画布尺寸) —— 画布常带留白
      (例: face/faces/000.img 画布 37×33 但帧 37×31)。一律用画布尺寸会把 470 个文件
      误判成"压缩变体"(其实它们根本没压缩, payload 开头就是 RGB565 的 `ff ff` = 白)。

      曾经错过两次的坑:
        ① 按 RGB888 逐行读 → 出雪花 (其实平坦色是 `84 00…` = RGB565 常量 0x0084);
        ② 按"3 字节交错"读 → 出**严格 1 像素棋盘**, 那是 2 字节像素被跨 3 字节读的干涉纹;
           正确解是"先整块彩色、再整块附加字节"。
      ⚠ 第 3 个字节**别当显示 alpha**: 拿它合成到白底会把整图洗成水印级浅灰。
    """
    if not im["exact"]:
        return None
    w, h, B = im["dw"], im["dh"], im["bppb"]
    step = w * h * B
    base = 68 + idx * step
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            i0 = y * w + x
            if B == 2:
                o = base + i0 * 2
                v = im["data"][o] | (im["data"][o + 1] << 8)
                lum = int(0.299 * (((v >> 11) & 0x1F) * 255 // 31)
                          + 0.587 * (((v >> 5) & 0x3F) * 255 // 63)
                          + 0.114 * ((v & 0x1F) * 255 // 31))
            elif B == 4:
                o = base + i0 * 4
                lum = int(0.299 * im["data"][o + 2] + 0.587 * im["data"][o + 1]
                          + 0.114 * im["data"][o])
            else:                                    # B == 3
                o = base + i0 * 2
                v = im["data"][o] | (im["data"][o + 1] << 8)
                r = ((v >> 11) & 0x1F) * 255 // 31
                g = ((v >> 5) & 0x3F) * 255 // 63
                b = (v & 0x1F) * 255 // 31
                lum = int(0.299 * r + 0.587 * g + 0.114 * b)
                if im.get("use_alpha"):
                    al = im["data"][base + w * h * 2 + i0]
                    lum = 255 - (255 - lum) * al // 255
            row.append(lum)
        rows.append(row)
    return rows


def cmd_list(root, pat):
    files = sorted(glob.glob(os.path.join(root, "**", pat), recursive=True))
    exact = 0
    for p in files:
        im = parse(p)
        if not im:
            continue
        if im["exact"]:
            exact += 1
        print("%-56s %3dx%-3d %d帧 %7dB %s"
              % (os.path.relpath(p, root), im["w"], im["h"], im["frames"], im["payload"],
                 "" if im["exact"] else "(头字段不自洽: 未解)"))
    print("\n共 %d 个 .img, 其中 %d 个可直接解码" % (len(files), exact))


def write_png_rgb(path, w, h, rgb):
    """纯标准库 RGB PNG (写颜色版, 便于人眼核对素材)。rgb = bytearray(w*h*3)。"""
    import zlib
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def decode_frame_rgb(im, idx=0):
    """第 idx 帧 → bytearray(w*h*3) 的 RGB。口径与 decode_frame 完全一致 (dw/dh/bppb)。"""
    if not im["exact"]:
        return None
    w, h, B = im["dw"], im["dh"], im["bppb"]
    base = 68 + idx * (w * h * B)
    out = bytearray(w * h * 3)
    for i in range(w * h):
        if B == 2 or B == 3:
            o = base + i * 2
            v = im["data"][o] | (im["data"][o + 1] << 8)
            r = ((v >> 11) & 0x1F) * 255 // 31
            g = ((v >> 5) & 0x3F) * 255 // 63
            b = (v & 0x1F) * 255 // 31
            if B == 3 and im.get("use_alpha"):
                al = im["data"][base + w * h * 2 + i]
                r = 255 - (255 - r) * al // 255
                g = 255 - (255 - g) * al // 255
                b = 255 - (255 - b) * al // 255
        else:
            o = base + i * 4
            b, g, r = im["data"][o], im["data"][o + 1], im["data"][o + 2]
        out[i * 3], out[i * 3 + 1], out[i * 3 + 2] = r, g, b
    return out


def cmd_extract(root, outdir, want_rgb, use_alpha=False, all_frames=False):
    files = sorted(glob.glob(os.path.join(root, "**", "*.img"), recursive=True))
    os.makedirs(outdir, exist_ok=True)
    ok = fail = 0
    man = []
    for p in files:
        im = parse(p)
        if im:
            im["use_alpha"] = use_alpha
        base_rel = os.path.relpath(p, root).replace("/", "__")[:-4]
        if not im or not im["exact"]:
            fail += 1
            man.append((os.path.relpath(p, root), "跳过", im and im["w"], im and im["h"],
                        im and im["frames"], im and im["bpp"]))
            continue
        nf = im["frames"] if all_frames else 1
        for fi in range(nf):
            rel = base_rel + ((".f%d" % fi) if all_frames else "") + ".png"
            path = os.path.join(outdir, rel)
            if want_rgb:
                write_png_rgb(path, im["dw"], im["dh"], decode_frame_rgb(im, fi))
            else:
                rows = decode_frame(im, fi)
                W, H = im["dw"], im["dh"]
                b = bytearray(W * H)
                for y in range(H):
                    for x in range(W):
                        b[y * W + x] = rows[y][x]
                write_png(path, W, H, b)
            ok += 1
        man.append((os.path.relpath(p, root), "已出图", im["dw"], im["dh"], im["frames"],
                    "%dB/px" % im["bppb"]))
    with open(os.path.join(outdir, "_manifest.tsv"), "w", encoding="utf-8") as f:
        f.write("原始文件\t状态\t宽\t高\t帧数\t像素格式\n")
        for r in man:
            f.write("\t".join(str(x) for x in r) + "\n")
    print("导出 %d 张图, 源文件 %d 个 (跳过 %d 个) → %s" % (ok, len(files), fail, outdir))
    print("清单: %s/_manifest.tsv" % outdir)
    return ok, fail



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["list", "info", "png", "extract"])
    ap.add_argument("path", nargs="?", default=".")
    ap.add_argument("-o", "--out", default="/tmp/qqt.png")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--gray", action="store_true", help="extract 时输出灰度而不是彩色")
    ap.add_argument("--allframes", action="store_true", help="每一帧都导出 (name.f0.png …)")
    ap.add_argument("--alpha", action="store_true", help="把第 3 字节当显示 alpha 合成到白底 (默认否, 会洗淡)")
    a = ap.parse_args()

    if a.cmd == "list":
        cmd_list(a.path, "*.img")
        return
    if a.cmd == "extract":
        cmd_extract(a.path, a.out, not a.gray, a.alpha, a.allframes)
        return
    im = parse(a.path)
    if im:
        im['use_alpha'] = a.alpha
    if not im:
        print("不是 DIMG: %s" % a.path)
        sys.exit(1)
    if a.cmd == "info":
        print("file      %s" % a.path)
        print("magic     QQF\\x1aDIMG")
        for i, v in enumerate(im["fields"]):
            if v > 0xFFFFFF:
                continue
            print("  f[%2d] = %d" % (i, v))
        print("画布 f[6]f[7] = %dx%d  (关卡图里这两个字段可能是哨兵值 0xCCCCCCCC, 不可信)"
              % (im["w"], im["h"]))
        print("帧   f[12]f[13] = %dx%d  ← 数据按这个切" % (im["fw"], im["fh"]))
        print("帧数 %d   行字节 %d   实际切分 %dx%d %dB/px"
              % (im["frames"], im["rowbytes"], im["dw"], im["dh"], im["bppb"]))
        print("负荷      %d B, 期望 %s B, %s"
              % (im["payload"], im["expect"], "吻合" if im["exact"] else "**不吻合(字段语义不同)**"))
        return
    rows = decode_frame(im, a.frame)
    if rows is None:
        print("该文件头部字段不自洽, 拒绝硬解 (猜出来的图是错的)")
        sys.exit(2)
    W, H = im["dw"] * a.scale, im["dh"] * a.scale
    buf = bytearray(W * H)
    for y in range(im["dh"]):
        for x in range(im["dw"]):
            v = rows[y][x]
            for dy in range(a.scale):
                for dx in range(a.scale):
                    buf[(y * a.scale + dy) * W + x * a.scale + dx] = v
    write_png(a.out, W, H, buf)
    print("→ %s (%dx%d 帧%d)" % (a.out, W, H, a.frame))


if __name__ == "__main__":
    main()
