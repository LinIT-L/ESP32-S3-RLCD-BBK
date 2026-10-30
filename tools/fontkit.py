#!/usr/bin/env python3
"""fontkit.py — LinTOS 主机端统一字体转换工具

一条流水线：多种字体源格式 → 两种设备端正式格式

  BDF / TTF / OTF / PCF / HZK / 旧 .fnt
      ├─→ .lbf        阅读器统一字体格式（1bit + Unicode 映射 + 按需读取）
      └─→ UI 16px bin 系统 UI 字库（保持 ZH1FNT01 旧格式与旧字符索引顺序）

设计原则
  * 只跑在主机端，不把字体解析依赖放进固件
  * 相同输入 → 字节级相同输出（deterministic）
  * 不覆盖源文件；输出前先写 .tmp 再原子 rename
  * 任何"猜"都不做：认不出的格式直接报错，不生成半成品

用法
  # 1) 源字体转 LBF（阅读器用）
  python3 tools/fontkit.py lbf sources/ark12.bdf --out out/ark12.lbf --name "方舟像素12"

  # 2) 批量转（配置驱动）
  python3 tools/fontkit.py batch tools/fonts_manifest.json --outdir /tmp/lbf

  # 3) 生成 UI 16px 字库（严格按旧 bin 的字符表与顺序）
  python3 tools/fontkit.py ui16 sources/zhengge16.bdf \
      --index-from components/ui/fonts/font_zh16.bin \
      --out /tmp/font_zh16_new.bin --report /tmp/ui16_report.json

  # 4) 旧 .fnt 迁移（用户 TF 卡数据，只读源文件）
  python3 tools/fontkit.py fnt2lbf /sdcard/fonts/xxx.fnt --out /tmp/xxx.lbf

  # 5) 查看任意字体信息
  python3 tools/fontkit.py info sources/ark12.bdf
  python3 tools/fontkit.py info out/ark12.lbf
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import struct
import sys
import zlib
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Tuple

# --------------------------------------------------------------------------
# LBF 格式常量（必须与 components/engines/book_reader/lbf.h 保持一致）
# --------------------------------------------------------------------------

LBF_MAGIC = b"LBF1"
LBF_VER_MAJOR = 1
LBF_VER_MINOR = 0
LBF_HEADER_SIZE = 96

# flags
LBF_FLAG_PROPORTIONAL = 1 << 0   # 比例字体（字宽不等）
LBF_FLAG_HAS_ASCII = 1 << 1      # 含 ASCII 字形
LBF_FLAG_CJK = 1 << 2            # 含 CJK 字形

# src_format
SRC_BDF, SRC_TTF, SRC_OTF, SRC_PCF, SRC_HZK, SRC_FNT, SRC_UI_BIN = range(7)
SRC_FORMAT_NAME = {
    SRC_BDF: "bdf", SRC_TTF: "ttf", SRC_OTF: "otf",
    SRC_PCF: "pcf", SRC_HZK: "hzk", SRC_FNT: "fnt", SRC_UI_BIN: "ui_bin",
}

# 头部：magic/ver/header_size/flags | w/h/baseline/lineh | 7×u32 | name[32] | src/resv | pad
_LBF_HDR = struct.Struct("<4sHHII BBbB IIIIIII 32s BB 14x")
# 索引项：offset(u32) width(u8) height(u8) xoff(i8) yoff(i8)
_LBF_IDX = struct.Struct("<IBBbb")

assert _LBF_HDR.size == LBF_HEADER_SIZE, _LBF_HDR.size
assert _LBF_IDX.size == 8

UI_BIN_MAGIC = b"ZH1FNT01"


# --------------------------------------------------------------------------
# 通用数据结构
# --------------------------------------------------------------------------

@dataclass
class Glyph:
    """单个字形。bitmap 为行优先 1bpp，每行 (width+7)//8 字节，MSB 在左，1=墨点。"""
    width: int
    height: int
    xoff: int = 0
    yoff: int = 0
    bitmap: bytes = b""

    @property
    def stride(self) -> int:
        return (self.width + 7) // 8

    def get(self, x: int, y: int) -> int:
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return 0
        return (self.bitmap[y * self.stride + (x >> 3)] >> (7 - (x & 7))) & 1

    def ink(self) -> int:
        return sum(bin(b).count("1") for b in self.bitmap)


@dataclass
class Font:
    """解析后的字体：码点 → 字形，外加全局度量。"""
    name: str
    native_w: int
    native_h: int
    baseline: int
    line_height: int
    glyphs: Dict[int, Glyph] = field(default_factory=dict)
    src_format: int = SRC_BDF
    source_path: str = ""
    license: str = ""
    proportional: bool = False

    def add(self, cp: int, g: Glyph) -> None:
        self.glyphs[cp] = g

    def coverage(self) -> Dict[str, int]:
        asc = sum(1 for cp in self.glyphs if cp < 0x80)
        cjk = sum(1 for cp in self.glyphs if 0x4E00 <= cp <= 0x9FFF)
        return {"total": len(self.glyphs), "ascii": asc, "cjk": cjk}


class FontError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# BDF 解析
# --------------------------------------------------------------------------

def load_bdf(path: str) -> Font:
    """解析 BDF（Adobe Bitmap Distribution Format）。原生位图，不做任何缩放。"""
    if path.endswith(".gz"):
        raw = gzip.open(path, "rb").read().decode("latin-1")
    else:
        raw = open(path, "r", encoding="latin-1").read()

    # 老 X11 中文 BDF（ISAS gb24st / 部分 HBF 系）的 ENCODING 是 **GB2312 区位码**
    # （0x2121 起，加 0x8080 才是 EUC 双字节），不是 Unicode。直接当码点用会
    # 只能匹配到极少数巧合相等的字 —— 必须先按 GB2312 解码。
    head = raw[:3000].upper()
    gb_mode = ("CHARSET_REGISTRY" in head and "GB2312" in head) or \
              ("FONT " in head and "GB2312" in head and "UNICODE" not in head)

    font = Font(name=os.path.basename(path), native_w=0, native_h=0,
                baseline=0, line_height=0, src_format=SRC_BDF, source_path=path)
    font.gb2312_encoded = gb_mode

    bbox = None
    ascent = descent = None
    cur: Optional[dict] = None
    bitmap_rows: List[str] = []
    in_bitmap = False

    for line in raw.splitlines():
        if in_bitmap:
            if line.startswith("ENDCHAR"):
                in_bitmap = False
                if cur is not None and bitmap_rows:
                    _bdf_emit(font, cur, bitmap_rows, gb_mode)
                cur = None
                bitmap_rows = []
            else:
                bitmap_rows.append(line.strip())
            continue

        if line.startswith("FONTBOUNDINGBOX"):
            p = line.split()
            bbox = (int(p[1]), int(p[2]), int(p[3]), int(p[4]))
        elif line.startswith("FONT_ASCENT"):
            ascent = int(line.split()[1])
        elif line.startswith("FONT_DESCENT"):
            descent = int(line.split()[1])
        elif line.startswith("FAMILY_NAME"):
            m = re.search(r'"(.*)"', line)
            if m:
                font.name = m.group(1)
        elif line.startswith("COPYRIGHT"):
            m = re.search(r'"(.*)"', line)
            if m:
                font.license = (font.license + " " + m.group(1)).strip()
        elif line.startswith("LICENSE"):
            m = re.search(r'"(.*)"', line)
            if m:
                font.license = (font.license + " " + m.group(1)).strip()
        elif line.startswith("STARTCHAR"):
            cur = {"name": line.split(" ", 1)[1] if " " in line else ""}
        elif line.startswith("ENCODING") and cur is not None:
            cur["enc"] = int(line.split()[1])
        elif line.startswith("DWIDTH") and cur is not None:
            cur["dwidth"] = int(line.split()[1])
        elif line.startswith("BBX") and cur is not None:
            p = line.split()
            cur["bbx"] = (int(p[1]), int(p[2]), int(p[3]), int(p[4]))
        elif line.startswith("BITMAP") and cur is not None:
            in_bitmap = True
            bitmap_rows = []

    if not font.glyphs:
        raise FontError(f"{path}: 未解析到任何字形（不是 BDF？）")

    if bbox:
        font.native_w, font.native_h = bbox[0], bbox[1]
    else:
        g0 = next(iter(font.glyphs.values()))
        font.native_w, font.native_h = g0.width, g0.height
    font.baseline = ascent if ascent is not None else font.native_h - 2
    font.line_height = (ascent or font.native_h) + (descent or 0)

    # 判断是否比例字体
    widths = {g.width for g in font.glyphs.values() if g.width > 0}
    font.proportional = len(widths) > 1 or font.native_w <= 0
    return font


def _bdf_emit(font: Font, cur: dict, rows: List[str],
              gb_mode: bool = False) -> None:
    cp = cur.get("enc", -1)
    if cp < 0:
        return
    if gb_mode:
        # ENCODING 是 GB2312 区位（0x2121 = 区 0x21 位 0x21），加 0x8080 成 EUC 再解码
        hi = (cp >> 8) & 0xFF
        lo = cp & 0xFF
        try:
            if hi == 0:
                ch = chr(lo)                       # 单字节 ASCII
            else:
                ch = bytes([hi + 0x80, lo + 0x80]).decode("gb2312")
        except (UnicodeDecodeError, ValueError):
            return
        cp = ord(ch)
    if cp > 0x10FFFF:
        return
    bbx = cur.get("bbx", (font.native_w or 16, font.native_h or 16, 0, 0))
    w, h, xoff, yoff = bbx
    if w <= 0 or h <= 0:
        return
    stride = (w + 7) // 8
    buf = bytearray()
    for r in range(h):
        hexrow = rows[r] if r < len(rows) else ""
        try:
            data = bytes.fromhex(hexrow) if hexrow else b""
        except ValueError:
            data = b""
        data = (data + b"\x00" * stride)[:stride]
        buf += data
    font.add(cp, Glyph(w, h, xoff, yoff, bytes(buf)))


def load_pcf(path: str, keep: Optional[set] = None) -> Font:
    """PCF（X11 编译后的原生点阵）。**零插值、零二值化、零基线重算。**

    为什么必须这么走：文泉驿这类老中文字体，官方同时发布「点阵 PCF」和
    「轮廓 TTF」。TTF 是 PCF 的矢量化产物，从 TTF 反推点阵要经过光栅化 +
    二值化，必然产生多余像素 / 丢笔画 —— 那不是转换损失可以调掉的，是方向错了。
    直接从 PCF 取位图才是原始字形。

    实现要点：
      · PCF 只在**原生 px** 提供位图 —— FreeType 对其他尺寸直接 `invalid pixel size`，
        所以「能加载成功的尺寸」就是原生尺寸，不需要猜。
      · 用 `anchor='ls'`（左-基线）把字形画到 px×px 画布，输出的位图与 PCF
        内嵌数据逐位相同（FreeType 对位图字体不做任何重采样）。
      · yoff 按 `ascent - top - height` 反算，保证 _fit_cell 摆回原位。
    """
    from PIL import Image, ImageDraw, ImageFont

    f = None
    px = 0
    for cand in range(9, 49):
        try:
            ff = ImageFont.truetype(path, cand)
            if ff.getbbox("永") or ff.getbbox("A") or ff.getbbox("0"):
                f, px = ff, cand
                break
        except Exception:
            continue
    if f is None:
        raise FontError(f"{path}: FreeType 无法以任何尺寸加载该 PCF（不是位图字体？）")

    asc, desc = f.getmetrics()
    font = Font(name=os.path.basename(path), native_w=px, native_h=px,
                baseline=asc, line_height=asc + desc,
                src_format=SRC_PCF, source_path=path)
    font.native_layout = True      # 字形自带正确位置，build_ui_bin 不做整体基线校正
    font.pcf_ascent = asc

    pad = px // 2
    cv_w = px + pad * 2
    cps = sorted(keep) if keep else range(0x20, 0x10000)
    for cp in cps:
        if cp > 0x10FFFF:
            continue
        try:
            ch = chr(cp)
            img = Image.new("1", (cv_w, px * 2), 0)
            d = ImageDraw.Draw(img)
            d.text((pad, px + asc), ch, font=f, fill=1, anchor="ls")
        except Exception:
            continue
        bb = img.getbbox()
        if not bb:
            continue                      # 该码点在 PCF 里没有字形（或本来就是空白）
        x0, y0, x1, y1 = bb
        w, h = x1 - x0, y1 - y0
        if w <= 0 or h <= 0 or w > px * 2:
            continue
        top = y0 - px                     # 相对基线行的顶行
        stride = (w + 7) // 8
        buf = bytearray(stride * h)
        pxl = img.load()
        for yy in range(h):
            for xx in range(w):
                if pxl[x0 + xx, y0 + yy]:
                    buf[yy * stride + (xx >> 3)] |= 0x80 >> (xx & 7)
        font.add(cp, Glyph(w, h, x0 - pad, asc - top - h, bytes(buf)))

    if not font.glyphs:
        raise FontError(f"{path}: PCF 里没有取到任何字形")
    font.proportional = len({g.width for g in font.glyphs.values()}) > 1
    return font



def load_sfnt(path: str, target_px: Optional[int] = None,
              keep: Optional[set] = None) -> Font:
    """解析 TTF/OTF。

    先看是否内嵌位图（EBDT/EBLC 或 CBDT/CBLC）——那是原生点阵，直接用。
    没有内嵌位图时退回 FreeType 轮廓渲染，在原生字号上渲染后按 128 阈值二值化，
    并在报告里明确标注 "rasterized"（非原生点阵）。
    """
    try:
        from fontTools.ttLib import TTFont
    except ImportError as e:
        raise FontError("需要 fontTools：pip install fonttools") from e

    tt = TTFont(path, lazy=True, fontNumber=0)
    is_otf = "CFF " in tt or "CFF2" in tt
    src = SRC_OTF if is_otf else SRC_TTF

    name = _sfnt_name(tt) or os.path.basename(path)
    upem = tt["head"].unitsPerEm

    has_bitmap = "EBDT" in tt or "CBDT" in tt
    if has_bitmap and target_px is None:
        target_px = _sfnt_bitmap_ppem(tt)

    font = Font(name=name, native_w=0, native_h=0, baseline=0, line_height=0,
                src_format=src, source_path=path)

    if has_bitmap:
        font.glyphs.update(_sfnt_bitmap_glyphs(tt, font))
        font.native_w = int(target_px or font.native_w or 16)
        font.native_h = font.native_w
        font.baseline = int(round(font.native_h * 0.8))
        font.line_height = font.native_h
        if not font.glyphs:
            raise FontError(f"{path}: 内嵌位图表为空")
        return font

    # 轮廓字体：必须在原生字号渲染，否则破坏像素网格
    if target_px is None:
        raise FontError(
            f"{path}: 轮廓字体（无内嵌位图），必须用 --size 指定原生字号")
    font.glyphs.update(_rasterize_sfnt(tt, target_px, upem, font, keep))
    font.native_w = target_px
    font.native_h = target_px
    font.baseline = int(round(target_px * tt["hhea"].ascent / upem))
    font.line_height = int(round(
        target_px * (tt["hhea"].ascent - tt["hhea"].descent) / upem))
    return font


def _sfnt_name(tt) -> str:
    try:
        for rec in tt["name"].names:
            if rec.nameID == 4:
                try:
                    return rec.toUnicode()
                except Exception:
                    pass
    except Exception:
        pass
    return ""


def _sfnt_bitmap_ppem(tt) -> int:
    try:
        strike = tt["EBLC"].strikes[0] if "EBLC" in tt else tt["CBLC"].strikes[0]
        return int(strike.bitmapSizeTable.ppemY)
    except Exception:
        return 16


def _sfnt_bitmap_glyphs(tt, font: Font) -> Dict[int, Glyph]:
    """从 EBDT/CBDT 抽原生位图。1bit 与 8bit 灰度都处理（灰度按 128 二值化）。"""
    out: Dict[int, Glyph] = {}
    table = tt["EBDT"] if "EBDT" in tt else tt["CBDT"]
    cmap = tt.getBestCmap()
    if not cmap:
        return out

    # EBDT: per-glyph index 需要 EBLC 的 sizeTable 定位，fontTools 有 bitmaps 属性
    strike = tt["EBLC"].strikes[0] if "EBLC" in tt else tt["CBLC"].strikes[0]
    try:
        bitmap_data = strike.bitmapSizeTable
        index_sub = strike.indexSubTables[0]
    except Exception:
        return out

    for cp, gname in cmap.items():
        try:
            gid = tt.getGlyphID(gname)
            sub = None
            for s in strike.indexSubTables:
                if s.firstGlyphIndex <= gid <= s.lastGlyphIndex:
                    sub = s
                    break
            if sub is None:
                continue
            entry = sub.names.get(gid) if hasattr(sub, "names") else None
            if entry is None:
                metrics, data = table.strikeData[0][gid]
            else:
                metrics, data = table.strikeData[0][entry]
            w = metrics.width
            h = metrics.height
            rows = getattr(data, "imageData", b"")
            stride = (w + 7) // 8
            if len(rows) < stride * h:
                continue
            out[cp] = Glyph(w, h, metrics.horiBearingX & 0xFF if False else 0,
                            metrics.horiBearingY, bytes(rows[:stride * h]))
        except Exception:
            continue
    return out


def _rasterize_sfnt(tt, px: int, upem: int, font: Font,
                    keep: Optional[set] = None) -> Dict[int, Glyph]:
    """用 PIL/FreeType 在原生字号渲染轮廓，128 阈值二值化。

    像素字体的轮廓本来就是「每像素一个方格」，在 em=px 上渲染即与像素网格对齐；
    这里额外留一圈画布边距，避免字形贴边被裁。
    """
    from PIL import Image, ImageDraw, ImageFont

    path = font.source_path
    pil_font = ImageFont.truetype(path, px)

    out: Dict[int, Glyph] = {}
    cmap = tt.getBestCmap() or {}
    ascent_units = tt["hhea"].ascent
    descent_units = tt["hhea"].descent
    ascent = int(round(px * ascent_units / upem))
    descent = int(round(-descent_units * px / upem))

    M = px                      # 画布边距
    canvas = px + 2 * M
    baseline_y = M + ascent     # 基线在画布中的 y

    cps = sorted(keep) if keep else list(cmap.keys())
    for cp in cps:
        if cp > 0xFFFF or cp not in cmap:
            continue
        ch = chr(cp)
        try:
            img = Image.new("L", (canvas, canvas), 0)
            d = ImageDraw.Draw(img)
            d.text((M, baseline_y), ch, fill=255, font=pil_font, anchor="ls")
        except Exception:
            continue
        bbox = img.getbbox()
        if not bbox:
            continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        if w <= 0 or h <= 0 or w > px + 2 or h > px + 2:
            continue
        stride = (w + 7) // 8
        buf = bytearray(stride * h)
        pix = img.load()
        for yy in range(h):
            for xx in range(w):
                if pix[x0 + xx, y0 + yy] >= 96:
                    buf[yy * stride + (xx >> 3)] |= 0x80 >> (xx & 7)
        # BDF 语义：xoff = 左边相对原点；yoff = 底边相对基线（向上为正）
        out[cp] = Glyph(w, h, x0 - M, baseline_y - y1, bytes(buf))
    return out


# --------------------------------------------------------------------------
# HZK 解析（GB2312 94×94 定长字模）
# --------------------------------------------------------------------------

def load_hzk(path: str, cell: int) -> Font:
    """HZK16 / HZK24x 系列：GB2312 区位码定长点阵，无文件头。

    结构：区 0xA1-0xF7、位 0xA1-0xFE，按 (区-0xA1)*94 + (位-0xA1) 顺序排列。
    每字 = cell × ceil(cell/8) 字节，行优先，MSB 在左。
    """
    data = open(path, "rb").read()
    per = cell * ((cell + 7) // 8)
    if len(data) % per != 0:
        # HZK24 常见 94 区 × 94 位 × 72B = 636,192；实际 600,048 = 8334 字
        raise FontError(
            f"{path}: 大小 {len(data)} 不是 {per} 的整数倍，可能不是 HZK{cell}")

    font = Font(name=os.path.basename(path), native_w=cell, native_h=cell,
                baseline=cell - (2 if cell <= 16 else 4), line_height=cell,
                src_format=SRC_HZK, source_path=path)

    # GB2312 → Unicode 表（复用项目内 gb2312_uni.c 的映射思路，这里用 Python 内置 codec）
    idx = 0
    stride = (cell + 7) // 8
    for qu in range(0xA1, 0xF8):
        for wei in range(0xA1, 0xFF):
            if idx + per > len(data):
                break
            raw = data[idx:idx + per]
            idx += per
            try:
                ch = bytes([qu, wei]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(ch) != 1:
                continue
            # HZK 每行 stride 字节，1=墨点
            font.add(ord(ch), Glyph(cell, cell, 0, 0, raw))
    return font


# --------------------------------------------------------------------------
# 旧 .fnt 解析（BK16FNT1）与 UI bin 解析
# --------------------------------------------------------------------------

def load_old_fnt(path: str) -> Font:
    """旧阅读器 .fnt（BK16FNT1）：GB2312 94×94 偏移表 + 可选 RLE。

    布局见备份前的 font_book.c。这里按实际文件判定：
      magic[8] "BK16FNT1" | u16 cell | u16 flags | ... 之后是偏移表与数据。
    """
    data = open(path, "rb").read()
    if data[:8] != b"BK16FNT1":
        raise FontError(f"{path}: 不是 BK16FNT1 旧字库")
    cell = struct.unpack("<H", data[8:10])[0]
    if cell not in (12, 16, 24, 32):
        raise FontError(f"{path}: 异常 cell={cell}")
    raise FontError(
        f"{path}: 旧 .fnt 的偏移表布局在备份的 font_book.c 中按版本不同，"
        "本工具拒绝猜测；请先用 `fontkit.py fntdump` 导出结构后再迁移")


def load_ui_bin(path: str) -> Tuple[bytes, int, List[bytes], int]:
    """读 ZH1FNT01 字库，返回 (raw, count, chars, glyph_off)。"""
    d = open(path, "rb").read()
    if d[:8] != UI_BIN_MAGIC:
        raise FontError(f"{path}: 不是 ZH1FNT01 字库")
    n = struct.unpack("<I", d[8:12])[0]
    chars = [d[12 + i * 3: 12 + i * 3 + 3] for i in range(n)]
    goff = 12 + n * 3
    return d, n, chars, goff


# --------------------------------------------------------------------------
# 写出 LBF
# --------------------------------------------------------------------------

def _s8(v: int) -> int:
    """夹到 int8 范围。xoff/yoff 是有符号字节，不能用 & 0xFF 转无符号。"""
    if v > 127:
        return 127
    if v < -128:
        return -128
    return v


def write_lbf(font: Font, out_path: str, font_id: Optional[int] = None) -> dict:
    cps = sorted(font.glyphs.keys())
    if not cps:
        raise FontError("没有字形可写")

    glyph_blobs = bytearray()
    idx_entries = bytearray()
    for cp in cps:
        g = font.glyphs[cp]
        off = len(glyph_blobs)
        glyph_blobs += g.bitmap
        pos = len(idx_entries)
        idx_entries += _LBF_IDX.pack(off, min(g.width, 255), min(g.height, 255),
                                     _s8(g.xoff), _s8(g.yoff))
        if pos != off and False:
            pass

    cmap = b"".join(struct.pack("<I", cp) for cp in cps)

    cmap_off = LBF_HEADER_SIZE
    idx_off = cmap_off + len(cmap)
    data_off = idx_off + len(idx_entries)
    file_size = data_off + len(glyph_blobs)

    flags = 0
    if font.proportional:
        flags |= LBF_FLAG_PROPORTIONAL
    if any(cp < 0x80 for cp in cps):
        flags |= LBF_FLAG_HAS_ASCII
    if any(0x4E00 <= cp <= 0x9FFF for cp in cps):
        flags |= LBF_FLAG_CJK

    if font_id is None:
        font_id = zlib.crc32(font.name.encode("utf-8")) & 0xFFFFFFFF

    name_bytes = font.name.encode("utf-8")[:31]

    header = _LBF_HDR.pack(
        LBF_MAGIC, LBF_VER_MAJOR, LBF_VER_MINOR, LBF_HEADER_SIZE, flags,
        font.native_w & 0xFF, font.native_h & 0xFF,
        font.baseline & 0xFF, font.line_height & 0xFF,
        len(cps), cmap_off, idx_off, data_off, len(glyph_blobs), file_size, 0,
        name_bytes, font.src_format, 0,
    )
    body = header + cmap + bytes(idx_entries) + bytes(glyph_blobs)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    body = body[:44] + struct.pack("<I", crc) + body[48:]

    tmp = out_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(body)
    os.replace(tmp, out_path)

    return {
        "out": out_path, "glyphs": len(cps), "size": len(body),
        "native": f"{font.native_w}x{font.native_h}",
        "baseline": font.baseline, "line_height": font.line_height,
        "crc32": crc, "bytes_per_glyph": round(len(glyph_blobs) / len(cps), 2),
        "ascii": sum(1 for cp in cps if cp < 0x80),
        "cjk": sum(1 for cp in cps if 0x4E00 <= cp <= 0x9FFF),
    }


def read_lbf(path: str) -> dict:
    d = open(path, "rb").read()
    if len(d) < LBF_HEADER_SIZE:
        raise FontError(f"{path}: 文件过短")
    (magic, vM, vm, hsize, flags, nw, nh, base, lh, count,
     cmap_off, idx_off, data_off, data_len, fsize, crc,
     name, src, resv) = _LBF_HDR.unpack_from(d, 0)
    if magic != LBF_MAGIC:
        raise FontError(f"{path}: magic={magic!r} 不是 LBF")
    body = d[:44] + b"\x00\x00\x00\x00" + d[48:]
    real_crc = zlib.crc32(body) & 0xFFFFFFFF
    return {
        "version": f"{vM}.{vm}", "header_size": hsize, "flags": flags,
        "native_w": nw, "native_h": nh, "baseline": base, "line_height": lh,
        "glyph_count": count, "cmap_off": cmap_off, "idx_off": idx_off,
        "data_off": data_off, "data_len": data_len, "file_size": fsize,
        "actual_size": len(d), "name": name.rstrip(b"\x00").decode("utf-8", "replace"),
        "src_format": SRC_FORMAT_NAME.get(src, src),
        "crc32_stored": f"{crc:08X}", "crc32_calc": f"{real_crc:08X}",
        "crc_ok": crc == real_crc,
    }


# --------------------------------------------------------------------------
# UI 16px 字库生成（严格按旧索引）
# --------------------------------------------------------------------------

def build_ui_bin(font: Font, index_bin: str, out_path: str,
                 cell: int = 16, strict: bool = True) -> dict:
    """按旧字库的字符表与顺序生成新 16×16 字库，格式保持 ZH1FNT01。

    对每个旧字符，从新字体取字形；若新字体缺字，沿用旧字库字形（不允许空白掩盖），
    并在报告里列出——绝不静默减少字符。

    垂直对齐：不同字体声明的 ascent 不可靠（同一字体不同重制版能差 1~2 行），
    因此这里先按字形公式摆一遍，再统计**墨迹底部行的众数**，整体平移使众数对齐到
    旧字库实测的基线行（14）。这样任何字体都能与旧字库落在同一基线上。
    """
    old_raw, n, chars, goff = load_ui_bin(index_bin)
    old_per = cell * ((cell + 7) // 8)
    if len(old_raw) != 12 + n * 3 + n * old_per:
        raise FontError(f"{index_bin}: 大小与 {cell}px 布局不符 "
                        f"(期望 {12 + n*3 + n*old_per}, 实际 {len(old_raw)})")

    def old_glyph(i: int) -> bytes:
        o = goff + i * old_per
        return old_raw[o:o + old_per]

    # ---- 判断源字体是否「原生满格」----
    # HZK / 已排好的点阵 ROM 本身就是 cell×cell 满格布局（墨迹直接顶到第 0 行和第
    # cell-1 行），它自带正确垂直位置，**不能**再做基线重排——重排会把顶部或底部
    # 整行裁掉。只有轮廓渲染出来的紧致字形（宽高可变、带 xoff/yoff）才需要重排。
    def is_full_cell(g: "Glyph") -> bool:
        # 只看宽高和水平偏移：字形本身就是 cell 行满格位图，垂直位置已由位图内容决定。
        # yoff 是「字形左边/底边相对基线」的标注（BDF 常见 -2、HZK 为 0），
        # 对满格位图没有意义，不能拿它当重排依据。
        return (g.width == cell and g.height == cell
                and g.xoff == 0
                and len(g.bitmap) == cell * bpr)

    bpr = (cell + 7) // 8
    full_all = bool(font.glyphs) and all(is_full_cell(g) for g in font.glyphs.values())

    # ---- 第一遍：算出每个字的原始摆放，并统计墨迹底部 ----
    # 原生点阵（PCF / HZK 系）：字形自带正确位置与基线，**不做任何整体校正**。
    # 从轮廓渲染来的字形（TTF/OTF）才需要按墨迹底部众数对齐到旧库基线。
    native_layout = bool(getattr(font, "native_layout", False))
    BASELINE = int(round(font.baseline)) if (native_layout and font.baseline) \
               else int(round(cell * 14 / 16))
    placed: List[Optional[bytes]] = []
    bottoms: List[int] = []
    missing: List[str] = []
    fallback_used: List[str] = []

    for i, cb in enumerate(chars):
        try:
            ch = cb.decode("utf-8")
            cp = ord(ch)
        except Exception:
            placed.append(None)
            fallback_used.append(f"#{i}:<非法UTF8>")
            continue
        g = font.glyphs.get(cp)
        if g is None:
            placed.append(None)
            missing.append(f"U+{cp:04X} {ch}")
            continue
        buf = bytes(g.bitmap) if (full_all and is_full_cell(g)) \
              else _fit_cell(g, cell, 0, BASELINE)
        nz = [r for r in range(cell) if any(buf[r * bpr:(r + 1) * bpr])]
        if nz:
            bottoms.append(nz[-1])
        placed.append(buf)

    from collections import Counter
    mode_bottom = Counter(bottoms).most_common(1)[0][0] if bottoms else BASELINE
    delta = 0 if (full_all or native_layout) else (BASELINE - mode_bottom)

    # ---- 第二遍：带整体校正量重新摆放（满格字体 delta 恒为 0，原样输出）----
    glyphs_out = bytearray()
    for i, cb in enumerate(chars):
        p = placed[i]
        if p is None:
            glyphs_out += old_glyph(i)
            continue
        cp = ord(cb.decode("utf-8"))
        g = font.glyphs[cp]
        if full_all and is_full_cell(g):
            glyphs_out += bytes(g.bitmap)
        else:
            glyphs_out += _fit_cell(g, cell, delta, BASELINE)

    body = UI_BIN_MAGIC + struct.pack("<I", n) + b"".join(chars) + bytes(glyphs_out)
    assert len(body) == 12 + n * 3 + n * old_per, len(body)

    tmp = out_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(body)
    os.replace(tmp, out_path)

    diff = 0
    for i in range(n):
        o = goff + i * old_per
        newo = 12 + n * 3 + i * old_per
        if body[newo:newo + old_per] != old_raw[o:o + old_per]:
            diff += 1

    return {
        "out": out_path, "count": n, "size": len(body),
        "old_size": len(old_raw),
        "from_new_font": n - len(missing) - len(fallback_used),
        "missing_kept_old": len(missing),
        "missing_list": missing[:200],
        "invalid_utf8_kept_old": len(fallback_used),
        "baseline_mode_bottom": mode_bottom,
        "baseline_delta": delta,
        "glyph_diff_vs_old": diff,
        "sha256": hashlib.sha256(body).hexdigest(),
        "old_sha256": hashlib.sha256(old_raw).hexdigest(),
        "index_identical": True,
    }


def _fit_cell(g: Glyph, cell: int = 16, delta: int = 0,
              baseline: int = 14) -> bytes:
    """把 BDF 字形放进 16×16 网格：按基线对齐、不缩放、只做整数平移。

    基线位置取自旧字库实测：文泉驿 16px 的墨迹落在第 0–14 行，即基线在第 14 行。
    BDF 的 BBX yoff 是"字形左下角相对基线"的偏移（向上为正），因此：
        字形顶部行 = baseline16 - (yoff + height)
    delta 为整体垂直校正量（跨字体对齐用）。
    """
    stride = (cell + 7) // 8
    buf = bytearray(cell * stride)
    dx = (cell - g.width) // 2
    dy = baseline - (g.yoff + g.height) + delta
    for y in range(g.height):
        ty = y + dy
        if ty < 0 or ty >= cell:
            continue
        row_off = ty * stride
        for x in range(g.width):
            if not g.get(x, y):
                continue
            tx = x + dx
            if tx < 0 or tx >= cell:
                continue
            buf[row_off + (tx >> 3)] |= 0x80 >> (tx & 7)
    return bytes(buf)


# --------------------------------------------------------------------------
# 载入分派
# --------------------------------------------------------------------------

def sniff_format(path: str) -> str:
    """按文件头判断真实格式，不只看扩展名。"""
    with open(path, "rb") as f:
        head = f.read(8)
    if head[:4] in (b"\x00\x01\x00\x00", b"true", b"OTTO", b"ttcf"):
        return "otf" if head[:4] == b"OTTO" else "ttf"
    if head[:8] == b"ZH1FNT01":
        return "ui_bin"
    if head[:8] == b"BK16FNT1":
        return "fnt"
    if head[:4] == LBF_MAGIC:
        return "lbf"
    if head[:2] == b"\x1f\x8b":
        return "bdf"
    try:
        with open(path, "r", encoding="latin-1") as f:
            first = f.readline()
        if first.startswith("STARTFONT"):
            return "bdf"
    except Exception:
        pass
    if head[:4] == b"\x01fcp":
        return "pcf"
    return "unknown"


def load_any(path: str, size: Optional[int] = None,
             hzk_cell: Optional[int] = None,
             keep: Optional[set] = None) -> Font:
    # 显式给了 HZK 字身尺寸就直接走 HZK（该格式无文件头，无法靠嗅探判定）
    if hzk_cell:
        return load_hzk(path, hzk_cell)
    fmt = sniff_format(path)
    if fmt == "bdf":
        return load_bdf(path)
    if fmt == "pcf":
        return load_pcf(path, keep)
    if fmt in ("ttf", "otf"):
        return load_sfnt(path, size, keep)
    if fmt == "hzk":
        return load_hzk(path, hzk_cell or 16)
    if fmt == "fnt":
        return load_old_fnt(path)
    raise FontError(f"{path}: 无法识别格式（head 检测为 {fmt}）")


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _fmt_info(font: Font) -> str:
    cov = font.coverage()
    return (f"name={font.name}\n"
            f"src_format={SRC_FORMAT_NAME.get(font.src_format)}\n"
            f"native={font.native_w}x{font.native_h} baseline={font.baseline} "
            f"line_height={font.line_height}\n"
            f"glyphs={cov['total']} ascii={cov['ascii']} cjk={cov['cjk']}\n"
            f"license={font.license or '(未声明)'}")


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(prog="fontkit", description="LinTOS 字体转换工具")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="查看字体信息")
    p.add_argument("src")
    p.add_argument("--size", type=int)
    p.add_argument("--hzk-cell", type=int)

    p = sub.add_parser("lbf", help="源字体 → LBF")
    p.add_argument("src")
    p.add_argument("--out", required=True)
    p.add_argument("--name")
    p.add_argument("--size", type=int, help="轮廓字体必须给原生字号")
    p.add_argument("--hzk-cell", type=int)
    p.add_argument("--charset", help="限定字符集：gb2312 / text:文件 / hex:文件")

    p = sub.add_parser("ui16", help="生成 UI 字库（ZH1FNT01，按旧索引）")
    p.add_argument("src")
    p.add_argument("--index-from", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--size", type=int)
    p.add_argument("--cell", type=int, default=16, help="字格尺寸 16 或 24")
    p.add_argument("--hzk-cell", type=int, help="HZK 原始字模尺寸（如 24）")
    p.add_argument("--report")

    p = sub.add_parser("verify", help="校验 LBF 文件")
    p.add_argument("lbf")

    p = sub.add_parser("batch", help="按 manifest 批量转换")
    p.add_argument("manifest")
    p.add_argument("--outdir", required=True)

    args = ap.parse_args(argv)

    try:
        if args.cmd == "info":
            font = load_any(args.src, args.size, args.hzk_cell)
            print(_fmt_info(font))
            return 0

        if args.cmd == "lbf":
            keep = _charset_set(args.charset) if args.charset else None
            font = load_any(args.src, args.size, args.hzk_cell, keep)
            if args.name:
                font.name = args.name
            if keep is not None:
                font.glyphs = {cp: g for cp, g in font.glyphs.items() if cp in keep}
            st = write_lbf(font, args.out)
            print(json.dumps(st, ensure_ascii=False, indent=1))
            return 0

        if args.cmd == "verify":
            print(json.dumps(read_lbf(args.lbf), ensure_ascii=False, indent=1))
            return 0

        if args.cmd == "ui16":
            # 先读出旧字库的字符表, 当作 keep 集合 —— PCF 有 3 万字,
            # 只取目标字库真正要的那一批, 避免逐码点扫 6 万次。
            _old_raw, _n, _chars, _go = load_ui_bin(args.index_from)
            _keep = set()
            for _cb in _chars:
                try:
                    _keep.add(ord(_cb.decode("utf-8")))
                except Exception:
                    pass
            font = load_any(args.src, args.size, args.hzk_cell, _keep)
            st = build_ui_bin(font, args.index_from, args.out, args.cell)
            if args.report:
                with open(args.report, "w") as f:
                    json.dump(st, f, ensure_ascii=False, indent=1)
            print(json.dumps({k: v for k, v in st.items() if k != "missing_list"},
                             ensure_ascii=False, indent=1))
            if st["missing_kept_old"]:
                print(f"⚠ 缺字 {st['missing_kept_old']} 个，已沿用旧字形："
                      f"{st['missing_list'][:10]}", file=sys.stderr)
            return 0

        if args.cmd == "batch":
            man = json.load(open(args.manifest))
            os.makedirs(args.outdir, exist_ok=True)
            results = []
            for item in man["fonts"]:
                src = item["src"]
                if not os.path.exists(src):
                    results.append({"id": item["id"], "status": "missing_source"})
                    continue
                try:
                    font = load_any(src, item.get("size"), item.get("hzk_cell"))
                    font.name = item.get("name", font.name)
                    out = os.path.join(args.outdir, item["id"] + ".lbf")
                    st = write_lbf(font, out)
                    st.update({"id": item["id"], "status": "ok"})
                    results.append(st)
                except Exception as e:
                    results.append({"id": item["id"], "status": "error",
                                    "error": str(e)})
            json.dump(results, sys.stdout, ensure_ascii=False, indent=1)
            return 0
    except FontError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    return 1


def _charset_set(spec: str) -> set:
    keep: set = set()
    if spec == "gb2312":
        for hi in range(0xA1, 0xF8):
            for lo in range(0xA1, 0xFF):
                try:
                    keep.add(ord(bytes([hi, lo]).decode("gb2312")))
                except Exception:
                    pass
        # ★★★ 2026-10-30 修: GB2312 **标准本身包含 ASCII**（单字节 0x20-0x7E），
        #   这里原来只收双字节区 ⇒ 转换出来的 LBF 一个 ASCII 都没有。
        #   后果: unifont16 / chill7 / wqy12-16 / ark12_otf 这几款里，
        #   英文数字在阅读器上只能回落到内置 FONT8X12，中英混排风格直接割裂
        #   （像素字体的英文设计完全用不上）。
        #   实测证据: 修前这 8 个 LBF 的 U+0020-007E 覆盖率都是 0，
        #   而 GB2312 汉字 6763 + 符号 682 = 7445 与 unifont16 的 7445 字完全吻合
        #   —— 说明它们就是按 gb2312 转的，只是把 ASCII 漏在外面。
        for c in range(0x20, 0x7F):
            keep.add(c)
    elif spec.startswith("text:"):
        txt = open(spec[5:], encoding="utf-8").read()
        keep = {ord(c) for c in txt}
    elif spec.startswith("hex:"):
        for line in open(spec[4:]):
            line = line.strip()
            if line:
                keep.add(int(line, 16))
    else:
        raise FontError(f"未知 charset: {spec}")
    return keep


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
