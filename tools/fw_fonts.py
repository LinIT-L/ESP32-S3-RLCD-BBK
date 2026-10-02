#!/usr/bin/env python3
"""fw_fonts.py — 直接加载**固件真实字模**, 供 lua_sim.py 做像素级精确渲染。

为什么需要它: 以前模拟器用 macOS 系统 TTF (Menlo / Hiragino) 近似固件字体,
结果截图与设备实际显示有差异, 无法用来判定"UI 是否一模一样"。
本模块改为读取固件自己的字模数据:

  components/ui/fonts/font8x16_zh.h   ASCII 8x16 点阵 (95 字符, 每字 16 字节)
  components/ui/fonts/font_zh.bin     中文 24x24 (每字 72 字节, 3 字节/行)
  components/ui/fonts/font_zh16.bin   中文 16x16 (每字 32 字节, 2 字节/行)
    布局: [8B magic "ZH1FNT01"][u32 count][字符表 count*3 UTF-8][字形 count*per]
    ⚠ ASCII 字形也存在 font_zh.bin 里, 键为 b"\\x00\\x00" + 字符 (对应 font_zh_find_ascii)

于是 1bpp 帧缓冲上的每一个点都与设备一致 (LCD 抖色/反色除外)。
"""
import os
import re
import struct

_HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FONT_DIR = os.path.normpath(os.path.join(_HERE, "..", "components", "ui", "fonts"))


class FwFonts:
    """固件字模只读索引。所有 bit 查询都是 (char, row, col) → bool。"""

    def __init__(self, font_dir=DEFAULT_FONT_DIR):
        self.dir = font_dir
        self.ascii8x16 = self._load_ascii8x16(os.path.join(font_dir, "font8x16_zh.h"))
        self._zh24_chars, self._zh24_glyphs, self._zh24_bpr = self._load_bin(
            os.path.join(font_dir, "font_zh.bin"), 72, 3)
        self._zh16_chars, self._zh16_glyphs, self._zh16_bpr = self._load_bin(
            os.path.join(font_dir, "font_zh16.bin"), 32, 2)
        # UTF-8 键 → 字形 (注意: glyphs 是 bytes, 必须切片, [i] 会得到 int)
        self._z24 = {}
        self._z24_ascii = {}
        n = len(self._zh24_chars) // 3
        for i in range(n):
            k = self._zh24_chars[i * 3:i * 3 + 3]
            g = self._zh24_glyphs[i * 72:(i + 1) * 72]
            self._z24[k] = g
            if k[0] == 0 and k[1] == 0:          # font_zh_find_ascii 的存储约定
                self._z24_ascii[k[2]] = g
        self._z16 = {}
        n16 = len(self._zh16_chars) // 3
        for i in range(n16):
            k = self._zh16_chars[i * 3:i * 3 + 3]
            self._z16[k] = self._zh16_glyphs[i * 32:(i + 1) * 32]

    # ---------- 载入 ----------
    @staticmethod
    def _load_ascii8x16(path):
        """解析 font8x16_zh.h 里的字形行 (95 行 × 16 字节)。
        注意: 不能整体用 `\\{...\\}` 正则 —— '{' / '}' 两个字符的**行尾注释**里
        本身带花括号, 会把匹配截断 (实测少 1 个字形)。逐行取 16 个 0x?? 更稳。"""
        out = []
        for line in open(path, encoding="utf-8"):
            if not line.lstrip().startswith("{"):
                continue
            nums = re.findall(r"0x([0-9A-Fa-f]{1,2})\b", line.split("/*")[0])
            if len(nums) == 16:
                out.append(bytes(int(v, 16) for v in nums))
        if len(out) < 95:
            raise RuntimeError("font8x16_zh.h 解析失败: 只得到 %d 个字形" % len(out))
        return out[:95]

    @staticmethod
    def _load_bin(path, per, bpr):
        d = open(path, "rb").read()
        if d[:8] != b"ZH1FNT01":
            raise RuntimeError("%s: magic 不对" % path)
        cnt = struct.unpack("<I", d[8:12])[0]
        chars = d[12:12 + cnt * 3]
        glyphs = d[12 + cnt * 3:12 + cnt * 3 + cnt * per]
        if len(glyphs) != cnt * per:
            raise RuntimeError("%s: 字形区长度不符" % path)
        return chars, glyphs, bpr

    def _glyph(self, table, bpr, key):
        g = table.get(key)
        return (g, bpr) if g else (None, bpr)

    # ---------- 位查询 ----------
    @staticmethod
    def _bit(data, bpr, row, col):
        b = data[row * bpr + (col >> 3)]
        return bool(b & (0x80 >> (col & 7)))

    def ascii8x16_bit(self, ch, row, col, rows=16):
        """ASCII 8×16 (font_ascii.c 的 FONT8X12 真身, 名字是历史遗留)。
        rows 参数用于模拟 draw_ascii_small 只取前 12 行的行为。"""
        if row >= rows:
            return False
        c = ord(ch) if isinstance(ch, str) else ch
        idx = (c - 0x20) if (0x20 <= c <= 0x7E) else (ord('?') - 0x20)
        return self._bit(self.ascii8x16[idx], 1, row, col)

    def zh24_bit(self, ch, row, col):
        """中文(或 ASCII) 24×24 字形, 来自 font_zh.bin。"""
        key = ch.encode("utf-8") if isinstance(ch, str) else ch
        g = self._z24.get(key)
        if g is None:
            return False
        return self._bit(g, self._zh24_bpr, row, col)

    def zh24_ascii_bit(self, ch, row, col_src24):
        """draw_ascii() 用的 ASCII 字形: font_zh 里的 24×24 大写风格 ASCII。"""
        g = self._z24_ascii.get(ord(ch) if isinstance(ch, str) else ch)
        if g is None:
            return False
        return self._bit(g, self._zh24_bpr, row, col_src24)

    def has_zh24_ascii(self, ch):
        return (ord(ch) if isinstance(ch, str) else ch) in self._z24_ascii

    def zh16_bit(self, ch, row, col):
        """中文 16×16 字形, 来自 font_zh16.bin。"""
        key = ch.encode("utf-8") if isinstance(ch, str) else ch
        g = self._z16.get(key)
        if g is None:
            return False
        return self._bit(g, self._zh16_bpr, row, col)

    def has_zh16(self, ch):
        return (ch.encode("utf-8") if isinstance(ch, str) else ch) in self._z16

    def has_zh24(self, ch):
        return (ch.encode("utf-8") if isinstance(ch, str) else ch) in self._z24

    # ---------- 覆盖率查询 (供 lua_check 静态检查"这个字设备上画得出来吗") ----------
    def chars16(self):
        """font_zh16 覆盖的字符集合 (app.zh / app.mix 走这套)。"""
        return {k.decode("utf-8") for k in self._z16 if k[0] != 0}

    def chars24(self):
        """font_zh 覆盖的字符集合 (app.zh24 / app.zhs / app.zhsb 走这套)。"""
        return {k.decode("utf-8") for k in self._z24 if k[0] != 0}

    # ---------- 统计 ----------
    def stats(self):
        return dict(ascii=len(self.ascii8x16), zh24=len(self._z24),
                    zh24_ascii=len(self._z24_ascii), zh16=len(self._z16))


if __name__ == "__main__":
    f = FwFonts()
    print("字模目录:", f.dir)
    print("统计:", f.stats())
    # 自检: 渲染 "A" 看形状
    for r in range(16):
        print("".join("#" if f.ascii8x16_bit("A", r, c) else "." for c in range(8)))
    print("--- 中 16 ---")
    for r in range(16):
        print("".join("#" if f.zh16_bit("中", r, c) else "." for c in range(16)))
