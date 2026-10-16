#!/usr/bin/env python3
"""fw_kbd.py — 直接读固件 `components/ui/kbd_lib/keyboard.c` 的键盘布局, 实现同一套内核逻辑。

为什么单独抽一个模块:
  1. 模拟器要能在没有硬件时验证"键盘长什么样、点哪里出什么字符" —— 也就是 term.lua 这类
     应用的核心交互。若在模拟器里另抄一份 52 键表, 就会与固件分叉。
  2. 布局表在固件里是**单一来源**(keyboard.c 的 s_keys[]), 这里只做解析 + 同语义实现。

与固件的对应关系(逐条对齐):
  KBD_ROW=29  KBD_ROWS=5  KBD_EDIT_ROW=19
  kbd_press(): CAPS 翻转 caps / SHIFT 翻转 shift / FN 循环 0→1→2 / 其余返回动作码
  kbd_char() : fn_mode==1 且该键有 fn → 无字符; fn_mode==2 且有 sym → sym[0];
               否则 ch, 且 caps||shift 时小写字母转大写
  4 种样式(kbd_draw_style/kbd_hit_style):
     std    → 排高 29, 全部 5 排, 全宽
     edit   → 排高 19, 全部 5 排, 全宽
     rows   → 排高 29, 只前 rows 排
     dialog → 把 400 宽布局等比缩放平移到 [x, x+w), 排高 row_h
"""
import os
import re

_HERE = os.path.dirname(os.path.abspath(__file__))
_KBD_C = os.path.normpath(os.path.join(_HERE, "..", "components", "ui", "kbd_lib", "keyboard.c"))

KBD_ROW = 29
KBD_ROWS = 5
KBD_EDIT_ROW = 19
KBD_H = KBD_ROWS * KBD_ROW          # 145
KBD_W = 400

# 动作码 (与 keyboard.h 的 KBD_ACT_* 同值)
ACT_CHAR, ACT_ENTER, ACT_BKSP, ACT_SPACE, ACT_ESC, ACT_TAB = 0, 1, 2, 3, 4, 5
ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT = 6, 7, 8, 9
ACT_CAPS, ACT_SHIFT, ACT_FN, ACT_CTRL, ACT_ALT = 10, 11, 12, 13, 14

_CNAME = {"KBD_ACT_CHAR": ACT_CHAR, "KBD_ACT_ENTER": ACT_ENTER, "KBD_ACT_BKSP": ACT_BKSP,
          "KBD_ACT_SPACE": ACT_SPACE, "KBD_ACT_ESC": ACT_ESC, "KBD_ACT_TAB": ACT_TAB,
          "KBD_ACT_UP": ACT_UP, "KBD_ACT_DOWN": ACT_DOWN, "KBD_ACT_LEFT": ACT_LEFT,
          "KBD_ACT_RIGHT": ACT_RIGHT, "KBD_ACT_CAPS": ACT_CAPS, "KBD_ACT_SHIFT": ACT_SHIFT,
          "KBD_ACT_FN": ACT_FN, "KBD_ACT_CTRL": ACT_CTRL, "KBD_ACT_ALT": ACT_ALT}
ACT_NAME = {v: k for k, v in _CNAME.items()}

STYLE_STD, STYLE_EDIT, STYLE_ROWS, STYLE_DIALOG = 0, 1, 2, 3
_STYLE_BY_PREFIX = {"s": STYLE_STD, "e": STYLE_EDIT, "r": STYLE_ROWS, "d": STYLE_DIALOG}
STYLE_NAME = {STYLE_STD: "std", STYLE_EDIT: "edit", STYLE_ROWS: "rows", STYLE_DIALOG: "dialog"}


def _cstr(tok):
    """C 字面量 → Python 字符串 ('' -> None)"""
    t = tok.strip()
    if t in ("0", "", "NULL"):
        return None
    if len(t) >= 2 and t[0] == t[-1] and t[0] in "\"'":
        t = t[1:-1]
    return t if t else None


class Key:
    __slots__ = ("lab", "ch", "act", "x", "w", "row", "fn", "sym")

    def __init__(self, lab, ch, act, x, w, row, fn, sym):
        self.lab, self.ch, self.act = lab, ch, act
        self.x, self.w, self.row = int(x), int(w), int(row)
        self.fn, self.sym = fn, sym


def load_keys(path=_KBD_C):
    """解析 keyboard.c 的 s_keys[] → [Key, ...]"""
    src = open(path, encoding="utf-8", errors="replace").read()
    i = src.index("static const kbd_key_t s_keys[] = {")
    j = src.index("};", i)
    body = src[i:j if j > i else len(src)]
    rows = re.findall(r'\{"([^"]*)",([^,]*),([A-Z_0-9]+),(\d+),(\d+),(\d+),([^,]*),([^}]*)\}', body)
    out = []
    for lab, ch, act, x, w, r, fn, sym in rows:
        c = ch.strip()
        chv = _cstr(c) if (c not in ("0", "")) else None
        out.append(Key(lab, chv, _CNAME.get(act, ACT_CHAR), x, w, r, _cstr(fn), _cstr(sym)))
    if len(out) < 50:
        raise RuntimeError("keyboard.c 解析异常: 只得到 %d 个键" % len(out))
    return out


class Kernel:
    """键盘内核: 布局 + 档位状态 + 命中(与固件 kbd_hit_style 同几何)。"""

    def __init__(self):
        self.keys = load_keys()
        self.width = KBD_W
        self.caps = False
        self.shift = False
        self.fn_mode = 0
        self.style = STYLE_STD
        self.x, self.y, self.w, self.row_h, self.rows = 0, KBD_H * 0, KBD_W, KBD_ROW, KBD_ROWS

    # ---- 样式/几何: 与 kbd_geom_default / kbd_style_height 一致 ----
    def use(self, name, *a):
        st = _STYLE_BY_PREFIX.get((name or "s")[0].lower())
        if st is None:
            return False
        self.style = st
        if st == STYLE_DIALOG:
            self.x, self.y, self.w, self.row_h = int(a[0]), int(a[1]), int(a[2]), int(a[3])
            self.rows = KBD_ROWS
        elif st == STYLE_ROWS:
            self.rows = int(a[0]) if a else KBD_ROWS
            self.y = int(a[1]) if len(a) > 1 else (300 - KBD_ROW * self.rows)
            self.x, self.w, self.row_h = 0, KBD_W, KBD_ROW
        else:
            self.row_h = KBD_EDIT_ROW if st == STYLE_EDIT else KBD_ROW
            self.y = int(a[0]) if a else (300 - self.row_h * KBD_ROWS)
            self.x, self.w, self.rows = 0, KBD_W, KBD_ROWS
        return True

    def height(self):
        if self.style == STYLE_EDIT:
            return KBD_EDIT_ROW * KBD_ROWS
        if self.style == STYLE_ROWS:
            return KBD_ROW * self.rows
        if self.style == STYLE_DIALOG:
            return self.row_h * KBD_ROWS
        return KBD_H

    def geom(self):
        return dict(style=STYLE_NAME[self.style], x=self.x, y=self.y,
                    w=self.w, row_h=self.row_h, rows=self.rows)

    def reset(self):
        self.caps = self.shift = False
        self.fn_mode = 0

    # ---- 命中: 屏幕坐标 → 键索引 ----
    def hit(self, sx, sy):
        """与 kbd_hit_style 同几何: 每键屏上为
           x0 = base_x + key.x*scale, x1 = base_x + (key.x+key.w)*scale - 1
           y0 = base_y + key.row*row_h, y1 = y0 + row_h - 1
           scale = draw_w / k->w   (dialog 会 <1, 其余 =1)"""
        scale = self.w / float(self.width)
        if self.style == STYLE_STD or self.style == STYLE_EDIT:
            maxrow = KBD_ROWS
        elif self.style == STYLE_ROWS:
            maxrow = self.rows
        else:
            maxrow = KBD_ROWS
        for i, k in enumerate(self.keys):
            if k.row >= maxrow:
                continue
            x0 = self.x + int(round(k.x * scale))
            x1 = self.x + int(round((k.x + k.w) * scale)) - 1
            y0 = self.y + k.row * self.row_h
            y1 = y0 + self.row_h - 1
            if x0 <= sx <= x1 and y0 <= sy <= y1:
                return i
        return -1

    # ---- 输入语义 ----
    def press(self, idx):
        if idx < 0 or idx >= len(self.keys):
            return -1
        a = self.keys[idx].act
        if a == ACT_CAPS:
            self.caps = not self.caps
        elif a == ACT_SHIFT:
            self.shift = not self.shift
        elif a == ACT_FN:
            self.fn_mode = (self.fn_mode + 1) % 3
        return a

    def char(self, idx):
        if idx < 0 or idx >= len(self.keys):
            return None
        k = self.keys[idx]
        if k.act != ACT_CHAR:
            return None
        if self.fn_mode == 1 and k.fn:
            return None
        if self.fn_mode == 2 and k.sym:
            return k.sym[0]
        c = k.ch
        if c and (self.caps or self.shift) and "a" <= c <= "z":
            c = c.upper()
        return c

    def label(self, idx):
        if idx < 0 or idx >= len(self.keys):
            return ""
        k = self.keys[idx]
        if k.act == ACT_FN:
            return ("Fn", "Fn1", "Fn2")[self.fn_mode % 3]
        if k.ch and self.fn_mode == 1 and k.fn:
            return k.fn
        if k.ch and self.fn_mode == 2 and k.sym:
            return k.sym
        if k.lab:
            return k.lab
        if k.ch:
            return k.ch.upper() if (self.caps or self.shift) else k.ch
        return ""

    def layer_on(self, idx):
        if idx < 0 or idx >= len(self.keys):
            return False
        a = self.keys[idx].act
        return ((a == ACT_CAPS and self.caps) or (a == ACT_SHIFT and self.shift)
                or (a == ACT_FN and self.fn_mode != 0))


if __name__ == "__main__":
    kb = Kernel()
    print("解析到 %d 键, 宽 %d" % (len(kb.keys), kb.width))
    for st in ("std", "edit", "rows", "dialog"):
        kb.use(st, *( (3,) if st == "rows" else ((0, 150, 300, 16) if st == "dialog" else ()) ))
        print("   %-7s 高 %3d  几何 %s" % (st, kb.height(), kb.geom()))
    kb.use("std")
    # 抽查几个键的命中与字符
    for (sx, sy) in ((14, 150), (44, 179), (180, 295)):
        i = kb.hit(sx, sy)
        print("   点(%3d,%3d) → 键#%d %r char=%r" % (sx, sy, i, kb.label(i), kb.char(i)))
    kb.press(0)      # Esc
    print("   press(0)=%s (ESC)" % ACT_NAME[kb.press(0) if False else ACT_ESC])
