#!/usr/bin/env python3
"""lua_sim.py — LinTOS Lua 应用「本机模拟器」(固件字模精确版)

用 lupa (Lua 5.5, 与固件同版本) 跑纯 Lua 应用, 用 Python 实现固件页面里注册的
同一个 `app.*` 桥, 把绘制落到 400x300 的 1bpp 帧缓冲, 再导出 PNG。

★ 字模是真的: 通过 fw_fonts.py 直接读固件字库
    font8x16_zh.h / font_zh.bin / font_zh16.bin
  所以屏幕上每个点与设备一致, 可以用来判定"UI 是否一模一样"。

★ 语义也是真的: 逐条对齐 page_lua.c / ui_common.c
    - inv=true  = 填黑格 + 白字 (反白高亮)
    - app.zh / app.zh24 / app.zhs 是**汉字专用**: 每字固定占 16/24/12 宽,
      ASCII 落在里面是**空白格** (font_zh*_find_utf8 找不到 → 不画)
    - app.text   每字 16 宽 24 高; ASCII 用 FONT8X12 前 12 行的 2× 放大
    - app.texts  8×12   app.textm 12×18   app.textsb 8×16
    - app.mix    混合 16px: 汉字 16 宽 + ASCII 8 宽 (透明, 不铺底)
    - app.textw  宽度: ASCII 16 / 汉字 24
    - app.textc  **只有 (y, str[, inv])** —— 水平居中由固件按屏宽算, 不是传 cx
    - app.wifi.ap_info(i)   → 5 个返回值 (rssi, ssid, ch, auth, mac)
    - app.wifi.probe_info(i)→ 3 个返回值 (mac, rssi, ch)
    - app.hid.cdc_read(n)   → 字符串 或 nil

用法:
  python3 tools/lua_sim.py <app.lua>                       # 用该应用的默认场景
  python3 tools/lua_sim.py <app.lua> --steps "touch:200,70;key:5;move;render"
  python3 tools/lua_sim.py <app.lua> --out <目录> --scale 2

--steps 语法 (分号分隔, 每步出一张 PNG):
  render            仅重绘
  touch:X,Y         按下沿触发一次 (poll→松开→poll), 然后 render
  hold:X,Y          按住不放 (只设触摸点, 不抬)
  release           抬起
  move[:N]          仅走时间 (N 帧, 默认 1), 用于计时/动画
  move:X,Y          按住拖到 (X,Y) (需先 hold, 用于画线/拖动)
  key:N             action(N)  (1UP 2DOWN 3LEFT 4RIGHT 5OK 6BACK 7HOME)
  press:N           按住第 N 个动作键 (置"按住"态 → app.key_down/keys_mask/keys_repeat 可读)
  release:[N,...]   松开 (不给参数 = 全松)
  rpt:MS            只推进 MS 毫秒并调 poll (测 app.key_repeat 的自动重复节奏)

⚠ 固件里 key_down 的数据源是 input_get_held_gb_joypad()(保持态), 所以
  "按住移动"类逻辑必须用 press:N + rpt:MS + release 来测; 光 key:N 只是瞬时边沿。
"""
import argparse
import os
import sys

import lupa
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fw_fonts import FwFonts
from fw_kbd import Kernel as KbdKernel, ACT_NAME as KBD_ACT_NAME

W, H = 400, 300

# ---- 固件 draw_ascii_medium 的 1.5× 采样表 (ui_common.c:161-162) ----
_M_COLMAP = (0, 0, 1, 2, 2, 3, 4, 4, 5, 6, 6, 7)
_M_ROWMAP = (0, 0, 1, 2, 2, 3, 4, 4, 5, 6, 6, 7, 8, 8, 9, 10, 10, 11)
# ---- draw_zh_sb 的 22/24 居中采样 (ui_common.c:276-277) ----
_SB_OFF = 1
_SB_GLYPH = 22

# ---- 模拟用假 AP / 探针数据 (让 wifi 类应用有内容可画) ----
FAKE_APS = [
    ("LinTOS-Home",   -38,   6, 3, "A4:5E:60:11:22:33"),
    ("CMCC-2.4G",     -52,   1, 4, "10:7B:44:55:66:77"),
    ("TP-LINK_5G",    -61, 149, 4, "3C:46:D8:AA:BB:CC"),
    ("Xiaomi_AX3000", -67,  11, 3, "50:64:2B:DE:AD:01"),
    ("iPhone热点",    -74,  44, 3, "9C:F3:87:00:11:22"),
    ("",              -83,   9, 0, "00:11:22:33:44:55"),
]
FAKE_PROBES = [
    ("A4:5E:60:11:22:33", -40, 6), ("10:7B:44:55:66:77", -55, 1),
    ("3C:46:D8:AA:BB:CC", -63, 149), ("50:64:2B:DE:AD:01", -70, 11),
    ("9C:F3:87:00:11:22", -76, 44), ("F0:9F:C2:12:34:56", -80, 6),
    ("B8:27:EB:AA:BB:CC", -85, 3), ("DE:AD:BE:EF:00:01", -88, 36),
]

AUTH_NAME = {0: "OPEN", 1: "WEP", 2: "WPA", 3: "WPA2", 4: "WPA2/WPA3", 5: "WPA3"}

# OUI 假表 (app.net.vendor): 与假 AP/ARP 数据的前 3 字节对应
VENDOR_MAP = {
    "A4:5E:60": "TP-LINK", "10:7B:44": "Huawei", "3C:46:D8": "Xiaomi",
    "50:64:2B": "H3C", "9C:F3:87": "Apple", "F0:9F:C2": "Ubiquiti",
    "B8:27:EB": "Raspberry Pi", "DE:AD:BE": "?",
}

# 假公网信息 JSON (app.net.http_get): 与 C 版首选源 ip9.com.cn/get 的字段一致
FAKE_PUBJSON = '{"ip":"112.5.20.31","prov":"福建","city":"莆田","isp":"中国电信"}'

# 假 IP 定位应答 (app.net.fetch_start): 首选 api.ip.sb 给 latitude/longitude,
# 兜底 ip-api.com 给 lat/lon —— 与 planeradar.lua 的 GEO_URLS 对应
FAKE_GEO_IPSB   = '{"ip":"112.48.94.33","region":"Fujian","city":"Putian","latitude":25.4310,"longitude":119.0078,"isp":"China Mobile"}'
FAKE_GEO_IPAPI  = '{"status":"success","country":"\u4e2d\u56fd","regionName":"\u798f\u5efa\u7701","city":"\u8386\u7530\u5e02","lat":25.4310,"lon":119.0078}'

# 假 ADS-B 应答 (app.net.fetch_start/fetch_poll + app.json.pick) —— 字段与 adsb.fi
# v2 接口 (https://opendata.adsb.fi/api/v2/lat/{lat}/lon/{lon}/dist/{nm}) 一致,
# 含该接口**已算好的** dst(距离NM)/dir(方位角)。以湄洲岛为中心、50NM 内的十几个目标。
FAKE_ADSB = ('{"now":1790951848.001,"resultCount":9,"ptime":0.11,"aircraft":['
             '{"hex":"780abc","flight":"CES2345 ","r":"B-1234","t":"B738","lat":25.31,"lon":119.42,'
             '"alt_baro":31000,"alt_geom":31450,"gs":445.2,"track":212.4,"baro_rate":-640,"squawk":"3024",'
             '"dst":12.3,"dir":35.7,"rssi":-8.2,"ownOp":"China Eastern","emergency":"none"},'
             '{"hex":"780def","flight":"CCA1856 ","r":"B-5678","t":"A320","lat":24.95,"lon":118.81,'
             '"alt_baro":28000,"alt_geom":28300,"gs":402.0,"track":48.9,"baro_rate":0,"squawk":"1234",'
             '"dst":18.9,"dir":248.1,"rssi":-12.4,"ownOp":"Air China","emergency":"none"},'
             '{"hex":"781122","flight":"CSN3301 ","r":"B-9012","t":"A321","lat":25.62,"lon":119.19,'
             '"alt_baro":36000,"alt_geom":36300,"gs":471.5,"track":95.0,"baro_rate":1280,"squawk":"4512",'
             '"dst":24.6,"dir":2.4,"rssi":-9.9,"ownOp":"China Southern","emergency":"none"},'
             '{"hex":"781133","flight":"CXA8899 ","r":"B-2345","t":"B737","lat":25.03,"lon":119.73,'
             '"alt_baro":12400,"alt_geom":12600,"gs":318.7,"track":276.3,"baro_rate":-1920,"squawk":"2244",'
             '"dst":33.1,"dir":91.2,"rssi":-14.0,"ownOp":"Xiamen Air","emergency":"none"},'
             '{"hex":"781144","flight":"CHH7721 ","r":"B-3456","t":"B789","lat":24.66,"lon":119.65,'
             '"alt_baro":38000,"alt_geom":38250,"gs":488.9,"track":12.8,"baro_rate":0,"squawk":"6421",'
             '"dst":41.5,"dir":152.6,"rssi":-16.8,"ownOp":"Hainan Airlines","emergency":"none"},'
             '{"hex":"781155","flight":"CSZ9001 ","r":"B-4567","t":"A333","lat":25.71,"lon":118.55,'
             '"alt_baro":2900,"alt_geom":3000,"gs":212.4,"track":338.1,"baro_rate":-1024,"squawk":"7012",'
             '"dst":44.8,"dir":286.5,"rssi":-18.2,"ownOp":"Shenzhen Airlines","emergency":"none"},'
             '{"hex":"781166","flight":"","r":"","t":"","lat":25.05,"lon":119.16,'
             '"alt_baro":"ground","alt_geom":0,"gs":12.0,"track":180.0,"baro_rate":0,"squawk":"1200",'
             '"dst":0.6,"dir":210.0,"rssi":-22.0,"ownOp":"","emergency":"none"},'
             '{"hex":"781177","flight":"CPA301 ","r":"B-KQZ","t":"A35K","lat":24.40,"lon":118.30,'
             '"alt_baro":41000,"alt_geom":41300,"gs":502.1,"track":55.4,"baro_rate":0,"squawk":"3411",'
             '"dst":62.7,"dir":215.8,"rssi":-20.1,"ownOp":"Cathay Pacific","emergency":"none"},'
             '{"hex":"781188","flight":"NOK888  ","r":"B-7788","t":"B738","lat":25.44,"lon":119.86,'
             '"alt_baro":7600,"alt_geom":7800,"gs":268.3,"track":150.9,"baro_rate":1664,"squawk":"5501",'
             '"dst":47.9,"dir":78.3,"rssi":-17.5,"ownOp":"Thai AirAsia","emergency":"none"}'
             ']}')


class Sim:
    """实现 app.* 桥 → 1bpp 帧缓冲 (fb 值: 1=白, 0=黑, 与固件同义)"""

    def __init__(self, verbose=False):
        self.fw = FwFonts()
        self.kbd = KbdKernel()
        self.fb = bytearray(b"\x01" * (W * H))
        self.touch_xy = None
        self.kfifo = []          # 按键 FIFO (app.key() 从这里取)
        self.kdown = set()       # 当前"按住"的动作码集合 (app.key_down/keys_mask)
        self.krep = {}           # a -> [t0, last]  (app.key_repeat 的内部状态)
        self.applog = []         # app.log() 落这里, 兼作"是不是真跑到这行"的判据
        self.touch_from = None          # 本次按下的起点 (app.touch_start)
        self.t_down_ms = 0              # 按下时刻
        self.t_was = False
        self.ms = 0
        self.nvs, self.fs, self.db = {}, {}, {}
        self._seed_fs()
        self.imgs = {}
        self.log = []
        self._seed_store()   # 需在 self.log 之后 (预载会写日志)
        self.exited = False
        self.sbar_invert = False        # app.sbar(v) 记录: 暗底页面应置 true, 退出还原
        self.toast_msg, self.toast_until = "", 0     # app.dlg.toast 提示 (叠加层)
        self.cdc_queue = []
        self.lua = lupa.LuaRuntime(unpack_returned_tuples=True)
        self.app = self.lua.table()
        self._build_api()

    # ================= 底层像素 =================
    def px(self, x, y, black=True):
        x, y = int(x), int(y)
        if 0 <= x < W and 0 <= y < H:
            self.fb[y * W + x] = 0 if black else 1

    def fill(self, x0, y0, x1, y1, black=True):
        x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
        if x1 < x0: x0, x1 = x1, x0
        if y1 < y0: y0, y1 = y1, y0
        for y in range(max(0, y0), min(H - 1, y1) + 1):
            row = y * W
            v = 0 if black else 1
            for x in range(max(0, x0), min(W - 1, x1) + 1):
                self.fb[row + x] = v

    def rect(self, x0, y0, x1, y1, black=True):
        x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
        if x1 < x0: x0, x1 = x1, x0
        if y1 < y0: y0, y1 = y1, y0
        self.hline(x0, x1, y0, black); self.hline(x0, x1, y1, black)
        self.vline(x0, y0, y1, black); self.vline(x1, y0, y1, black)

    def hline(self, x0, x1, y, black=True):
        x0, x1 = int(x0), int(x1)
        if x1 < x0: x0, x1 = x1, x0
        for x in range(max(0, x0), min(W - 1, x1) + 1):
            self.px(x, y, black)

    def vline(self, x, y0, y1, black=True):
        y0, y1 = int(y0), int(y1)
        if y1 < y0: y0, y1 = y1, y0
        for y in range(max(0, y0), min(H - 1, y1) + 1):
            self.px(x, y, black)

    def clear(self, c=1):
        self.fb = bytearray(bytes([0 if int(c) == 0 else 1]) * (W * H))

    def bmp(self, x, y, w, h, hexstr):
        hx = str(hexstr)
        try:
            b = bytes.fromhex(hx)
        except ValueError:
            return
        bpr = (int(w) + 7) // 8
        for row in range(int(h)):
            for col in range(int(w)):
                i2 = row * bpr + (col >> 3)
                if i2 < len(b) and (b[i2] >> (7 - (col & 7))) & 1:
                    self.px(int(x) + col, int(y) + row, True)

    # ================= 固件字模绘制原语 =================
    @staticmethod
    def _put_glyph(sim, x, y, w, h, bit, inv, transparent=False):
        """按固件语义绘制一个字: 非透明时先铺 bg 矩形, 再逐点画 fg。"""
        fgc = not inv
        if not transparent:
            sim.fill(x, y, x + w - 1, y + h - 1, bool(inv))
        for r in range(h):
            for c in range(w):
                if bit(r, c):
                    sim.px(x + c, y + r, fgc)

    # ---- ASCII ----
    def texts(self, x, y, s, inv=False):
        """draw_ascii_small: 8×12 (FONT8X12 前 12 行, 1×)。"""
        s, x, y = str(s), int(x), int(y)
        for i, ch in enumerate(s):
            self._put_glyph(self, x + i * 8, y, 8, 12,
                            lambda r, c, ch=ch: self.fw.ascii8x16_bit(ch, r, c, rows=12), inv)
        return x + len(s) * 8

    def textm(self, x, y, s, inv=False):
        """draw_ascii_medium: 12×18 (前 12 行 1.5×)。"""
        s, x, y = str(s), int(x), int(y)
        for i, ch in enumerate(s):
            cx = x + i * 12
            self.fill(cx, y, cx + 11, y + 17, bool(inv))
            fgc = not inv
            for dy in range(18):
                for dx in range(12):
                    if self.fw.ascii8x16_bit(ch, _M_ROWMAP[dy], _M_COLMAP[dx], rows=12):
                        self.px(cx + dx, y + dy, fgc)
        return x + len(s) * 12

    def textsb(self, x, y, s, inv=False):
        """draw_ascii_sb: 底铺 12×16 格, 字形 8×16 全高。"""
        s, x, y = str(s), int(x), int(y)
        for i, ch in enumerate(s):
            cx = x + i * 8
            self.fill(cx, y, cx + 11, y + 15, bool(inv))
            fgc = not inv
            for r in range(16):
                for c in range(8):
                    if self.fw.ascii8x16_bit(ch, r, c, rows=16):
                        self.px(cx + c, y + r, fgc)
        return x + len(s) * 8

    def text(self, x, y, s, inv=False):
        """draw_text (菜单字号=24 默认): 每字 16 宽 24 高。
        汉字 → 24×24 (font_zh); ASCII → FONT8X12 前 12 行 2× 放大 (16×24)。"""
        s, x, y = str(s), int(x), int(y)
        cx = x
        for ch in s:
            if ord(ch) >= 0x80:
                self._zh24_cell(cx, y, ch, inv)
                cx += 24
            else:
                self.fill(cx, y, cx + 15, y + 23, bool(inv))
                fgc = not inv
                for r in range(12):
                    for c in range(8):
                        if self.fw.ascii8x16_bit(ch, r, c, rows=12):
                            self.fill(cx + c * 2, y + r * 2, cx + c * 2 + 1, y + r * 2 + 1, fgc)
                cx += 16
        return cx

    def textc(self, y, s, inv=False):
        """draw_text_centered: **只有 y** —— 水平居中由固件按屏宽(400)算。"""
        w = self.textw(s)
        x = (W - w) // 2
        if x < 0:
            x = 0
        self.text(x, y, s, inv)
        return x

    def textw(self, s):
        """text_width: ASCII(+2字节) 16 / 3~4 字节 24。"""
        w = 0
        b = str(s).encode("utf-8")
        i = 0
        while i < len(b):
            c = b[i]
            if c < 0x80: w += 16; i += 1
            elif c & 0xE0 == 0xC0: w += 16; i += 2
            elif c & 0xF0 == 0xE0: w += 24; i += 3
            elif c & 0xF8 == 0xF0: w += 24; i += 4
            else: i += 1
        return w

    def big(self, x, y, s, scale=1, inv=False):
        """app_big_ascii (page_lua.c): 8*scale 宽 × 16*scale 高, 全 16 行。"""
        s, x, y = str(s), int(x), int(y)
        sc = int(scale) if scale else 1
        for i, ch in enumerate(s):
            cx = x + i * 8 * sc
            self.fill(cx, y, cx + 8 * sc - 1, y + 16 * sc - 1, bool(inv))
            fgc = not inv
            for r in range(16):
                for c in range(8):
                    if self.fw.ascii8x16_bit(ch, r, c, rows=16):
                        self.fill(cx + c * sc, y + r * sc, cx + c * sc + sc - 1, y + r * sc + sc - 1, fgc)

    def bigc(self, cx, y, s, scale=1, inv=False):
        sc = int(scale) if scale else 1
        self.big(int(cx) - len(str(s)) * 4 * sc, y, s, sc, inv)

    # ---- 中文 ----
    def _zh24_cell(self, x, y, ch, inv, transparent=False):
        """中文 24×24: 找不到字形也要铺 24×24 底 (固件如此), ASCII 即空 24×24 格。"""
        x, y = int(x), int(y)
        if transparent:
            for r in range(24):
                for c in range(24):
                    if self.fw.zh24_bit(ch, r, c):
                        self.px(x + c, y + r, not inv)
            return
        self.fill(x, y, x + 23, y + 23, bool(inv))
        fgc = not inv
        for r in range(24):
            for c in range(24):
                if self.fw.zh24_bit(ch, r, c):
                    self.px(x + c, y + r, fgc)

    def zh24(self, x, y, s, inv=False):
        x, y = int(x), int(y)
        cx = x
        for ch in str(s):
            self._zh24_cell(cx, y, ch, inv)
            cx += 24
        return cx

    def zh(self, x, y, s, inv=False):
        """app.zh = font_zh16 16×16 (page_lua.c lua_zh16); ASCII 空 16×16 格。"""
        x, y = int(x), int(y)
        cx = x
        for ch in str(s):
            self.fill(cx, y, cx + 15, y + 15, bool(inv))
            if self.fw.has_zh16(ch):
                for r in range(16):
                    for c in range(16):
                        if self.fw.zh16_bit(ch, r, c):
                            self.px(cx + c, y + r, not inv)
            cx += 16
        return cx

    def zhs(self, x, y, s, inv=False):
        """app.zhs: 12×12 (font_zh 24×24 的 2×2 降采样); ASCII 空 12×12 格。"""
        x, y = int(x), int(y)
        cx = x
        for ch in str(s):
            self.fill(cx, y, cx + 11, y + 11, bool(inv))
            if self.fw.has_zh24(ch):
                for r in range(12):
                    for c in range(12):
                        any_on = any(self.fw.zh24_bit(ch, r * 2 + dy, c * 2 + dx)
                                     for dy in (0, 1) for dx in (0, 1))
                        if any_on:
                            self.px(cx + c, y + r, not inv)
            cx += 12
        return cx

    def zhsm(self, x, y, s, inv=False):
        return self.zhs(x, y, s, inv)

    def zhsb(self, x, y, s, inv=False):
        """app.zhsb: 16×16, font_zh 24×24 的中心 22×22 采样到 16×16。"""
        x, y = int(x), int(y)
        cx = x
        for ch in str(s):
            self.fill(cx, y, cx + 15, y + 15, bool(inv))
            if self.fw.has_zh24(ch):
                for dy in range(16):
                    sr = _SB_OFF + (dy * _SB_GLYPH) // 16
                    for dx in range(16):
                        sc = _SB_OFF + (dx * _SB_GLYPH) // 16
                        if self.fw.zh24_bit(ch, sr, sc):
                            self.px(cx + dx, y + dy, not inv)
            cx += 16
        return cx

    # ---- 混合 16px (ui_text16) ----
    # ⚠ 与固件一致: 固件用 `(*p & 0xe0) == 0xe0` 判定"汉字", 所以 **2 字节 UTF-8
    #   (÷ × − · 这类符号) 会走 ASCII 分支** —— 占 8 宽且字形缺失 → 画成空白。
    #   (踩过: calc 的 ÷ × − 三键在设备上是空白)
    @staticmethod
    def _is_wide(ch):
        """固件判定"汉字"看的是 **UTF-8 首字节**: `(*p & 0xe0) == 0xe0`。
        ⚠ 不能拿码点判: 汉(U+6C49) 的码点 &0xE0 = 0x40, 会被误判成 ASCII → 画成 '?'。"""
        return (ch.encode("utf-8")[0] & 0xE0) == 0xE0

    def mix(self, x, y, s, inv=False):
        """ui_text16: 汉字 16×16(font_zh16) + ASCII 8×16(FONT8X12) 垂直居中; 透明底。"""
        x, y = int(x), int(y)
        cx = x
        for ch in str(s):
            if self._is_wide(ch):
                if self.fw.has_zh16(ch):
                    for r in range(16):
                        for c in range(16):
                            if self.fw.zh16_bit(ch, r, c):
                                self.px(cx + c, y + r, not inv)
                cx += 16
            else:
                for r in range(16):
                    for c in range(8):
                        if self.fw.ascii8x16_bit(ch, r, c, rows=16):
                            self.px(cx + c, y + r, not inv)
                cx += 8
        return cx

    def mixc(self, cx, y, s, inv=False):
        x = int(cx) - self.mixw(s) // 2
        self.mix(x, y, s, inv)
        return x

    def mixw(self, s):
        """ui_text16w: 宽字符 16 / 其余 8。"""
        return sum(16 if self._is_wide(ch) else 8 for ch in str(s))


    # ================= 软键盘 (与固件 kbd_draw_style 同几何) =================
    def kbd_draw(self, press=-1):
        """按当前 style/几何画键盘: 外框 + 居中标签; 按下=整键反白, 激活档层=整键反白。
        (与固件外观"差不多": 固件用 kbd_ui.c 的圆角/箭头细节, 这里只保证几何与命中一致)"""
        kb = self.kbd
        scale = kb.w / float(kb.width)
        maxrow = kb.rows if kb.style == 2 else 5          # 2 = STYLE_ROWS
        for i, k in enumerate(kb.keys):
            if k.row >= maxrow:
                continue
            x0 = kb.x + int(round(k.x * scale))
            x1 = kb.x + int(round((k.x + k.w) * scale)) - 1
            y0 = kb.y + k.row * kb.row_h
            y1 = y0 + kb.row_h - 1
            inv = (i == press) or kb.layer_on(i)
            if inv:
                self.fill(x0, y0, x1, y1, True)
            else:
                self.rect(x0, y0, x1, y1, True)
            lab = kb.label(i)
            if not lab:
                continue
            lw = len(lab) * 8
            if x1 - x0 < lw + 2 or kb.row_h < 12:
                continue                                   # 缩放后放不下就不画 (固件用位图箭头/缩字)
            self.texts(x0 + (x1 - x0 + 1 - lw) // 2,
                       y0 + (kb.row_h - 12) // 2, lab, inv)

    # ================= 桥 =================
    @staticmethod
    def _c(color):
        """固件颜色参数: 0=BLACK(画黑) / 1=WHITE(画白)。
        ⚠ 固件用 luaL_checkinteger 取色 —— **传布尔值在设备上会直接报错**,
        所以这里也拒绝非数值: 它同时是一道"参数契约闸门"。
        (曾经踩过: 早期 5 个 Lua 应用传 true/false, 设备上必然 Lua error。)"""
        if isinstance(color, bool) or not isinstance(color, (int, float)):
            raise TypeError("颜色参数必须是数值 (app.BLACK=0 / app.WHITE=1), 收到 %r" % (color,))
        return int(color) == 0        # True = 该像素画黑

    def _build_api(self):
        a = self.app
        a.clear = lambda c=1: self.clear(c)
        a.px = lambda x, y, c: self.px(x, y, self._c(c))
        a.fill = lambda x0, y0, x1, y1, c: self.fill(x0, y0, x1, y1, self._c(c))
        a.store_rect = lambda x0, y0, x1, y1, c: self.rect(x0, y0, x1, y1, self._c(c))
        a.rect = lambda x0, y0, x1, y1, c: self.rect(x0, y0, x1, y1, self._c(c))
        a.hline = lambda x0, x1, y, c: self.hline(x0, x1, y, self._c(c))
        a.vline = lambda x, y0, y1, c: self.vline(x, y0, y1, self._c(c))
        a.bmp = lambda x, y, w, h, data: self.bmp(x, y, w, h, data)
        a.bmpr = lambda x, y, w, h, data: self._bmpr(x, y, w, h, data)
        # 字模族
        a.text = lambda x, y, s, inv=False: self.text(x, y, s, inv)
        a.textc = lambda y, s, inv=False: self.textc(y, s, inv)
        a.textw = self.textw
        a.texts = lambda x, y, s, inv=False: self.texts(x, y, s, inv)
        a.textm = lambda x, y, s, inv=False: self.textm(x, y, s, inv)
        a.textsb = lambda x, y, s, inv=False: self.textsb(x, y, s, inv)
        a.zh = lambda x, y, s, inv=False: self.zh(x, y, s, inv)
        a.zhs = lambda x, y, s, inv=False: self.zhs(x, y, s, inv)
        a.zhsm = lambda x, y, s, inv=False: self.zhsm(x, y, s, inv)
        a.zhsb = lambda x, y, s, inv=False: self.zhsb(x, y, s, inv)
        a.zh24 = lambda x, y, s, inv=False: self.zh24(x, y, s, inv)
        a.mix = lambda x, y, s, inv=False: self.mix(x, y, s, inv)
        a.mixc = lambda cx, y, s, inv=False: self.mixc(cx, y, s, inv)
        a.mixw = self.mixw
        a.big = lambda x, y, s, scale=1, inv=False: self.big(x, y, s, scale, inv)
        a.bigc = lambda cx, y, s, scale=1, inv=False: self.bigc(cx, y, s, scale, inv)
        # 时间 / 输入 / 声音
        a.millis = lambda: self.ms
        # app.key(): 非阻塞取按键。⚠ 必须和固件一致 —— 固件里 lua_action 会先 key_push((int)a)
        # 再调 action(a), 所以"只读 app.key() 的应用"在模拟器里也必须能拿到键(原来恒返回 None → 测不动)
        a.key = self._key_pop
        a.delay = lambda ms: setattr(self, "ms", self.ms + int(ms))
        a.tone = lambda f, ms, *rest: None
        a.tone_async = lambda f, ms, amp=0: None      # 非阻塞: 模拟器无音频, 只吞掉
        a.melody = lambda tbl=None: None
        # ⚠ 与固件一致: 固件的 key_is_down 取自 input_get_held_gb_joypad()(保持态),
        #   模拟器用 kdown 集合模拟"按住"; key:N 是瞬时边沿(压完即松), press:N/release:N 才产生按住。
        a.key_down = lambda n: int(n) in self.kdown
        a.keys_mask = lambda: sum((1 << (int(n) - 1)) for n in self.kdown if 1 <= int(n) <= 6)
        a.key_repeat = self._key_repeat
        a.log = self._applog
        a.vibrate = lambda ms: None
        a.exit = lambda: setattr(self, "exited", True)
        a.redraw = lambda: None
        a.sbar = lambda v=False: setattr(self, "sbar_invert", bool(v))
        a.touch = lambda: self._touch()
        a.touch_start = lambda: self._touch_start()
        a.touch_hold = lambda: (self.ms - self.t_down_ms) if self.touch_xy else 0
        # 常量
        for k, v in (("UP", 1), ("DOWN", 2), ("LEFT", 3), ("RIGHT", 4),
                     ("OK", 5), ("CONFIRM", 5), ("BACK", 6), ("HOME", 7),
                     ("W", W), ("H", H), ("BLACK", 0), ("WHITE", 1)):
            setattr(a, k, v)
        # 命名空间
        a.sys = self.lua.table(battery=lambda: 4000, heap_free=lambda: 50000,
                               time_ms=lambda: self.ms, clock=lambda: self._clock())
        # ⚠ 二进制安全: lupa 默认把 Lua 串按 UTF-8 解码成 Python str, 一旦内容含非 UTF-8 字节
        #   (如 1-bit BMP 里的 0x90) 就会 UnicodeDecodeError —— 且异常发生在"进入 Python 之前",
        #   回调里救不回来。所以写盘走 hex 通道: Lua 侧先 encode 成 hex 再过桥。
        # ---- app.img: 1bpp BMP 大图 (对齐 luat_lib_img.c 的语义) ----
        #   语义与固件一致: BMP 位=1 → 白, 所以 ink = 位==0;
        #   mode=1 时"位=1 画白" (精灵底版刷白用)。
        def _img_load(path, opaque=False):
            b = self.fs.get(str(path))
            if not b or len(b) < 62 or b[:2] != b"BM":
                return None
            off = int.from_bytes(b[10:14], "little")
            w = int.from_bytes(b[18:22], "little", signed=True)
            h = int.from_bytes(b[22:26], "little", signed=True)
            bpp = int.from_bytes(b[28:30], "little")
            if bpp != 1 or w <= 0 or h == 0:
                return None
            flip, h = (h > 0), abs(h)
            ssrc, sdst = ((w + 31) // 32) * 4, (w + 7) // 8
            bits = bytearray(sdst * h)
            for fy in range(h):
                y = (h - 1 - fy) if flip else fy
                row = off + fy * ssrc
                for x in range(w):
                    if not ((b[row + (x >> 3)] >> (7 - (x & 7))) & 1):
                        bits[y * sdst + (x >> 3)] |= (0x80 >> (x & 7))
            i = (max(self.imgs) + 1) if self.imgs else 1
            self.imgs[i] = {"w": w, "h": h, "stride": sdst, "bits": bits,
                            "opaque": bool(opaque)}
            return i

        def _img_blit(i, sx, sy, dx, dy, w, h, mode=0):
            im = self.imgs.get(int(i))
            if not im:
                return
            sx, sy = int(sx), int(sy)
            dx, dy, w, h = int(dx), int(dy), int(w), int(h)
            mode = int(mode or 0)
            for y in range(h):
                ry = sy + y
                if ry < 0 or ry >= im["h"]:
                    continue
                for x in range(w):
                    rx = sx + x
                    if rx < 0 or rx >= im["w"]:
                        continue
                    ink = (im["bits"][ry * im["stride"] + (rx >> 3)] >> (7 - (rx & 7))) & 1
                    if mode == 1:
                        if ink:
                            self.px(dx + x, dy + y, black=False)
                    elif ink:
                        self.px(dx + x, dy + y, black=True)
                    elif im["opaque"]:
                        self.px(dx + x, dy + y, black=False)

        a.img = self.lua.table(
            load=_img_load, blit=_img_blit,
            size=lambda i: ((self.imgs[int(i)]["w"], self.imgs[int(i)]["h"])
                            if int(i) in self.imgs else None),
            free=lambda i: self.imgs.pop(int(i), None),
            freeall=lambda: self.imgs.clear())
        a.fs = self.lua.table(exists=lambda p: str(p) in self.fs,
                              read=lambda p: self.fs.get(str(p)),
                              write=lambda p, hexd: (self.fs.__setitem__(
                                  str(p), bytes.fromhex(str(hexd))), True)[1],
                              append=lambda p, d: True,
                              remove=lambda p: self.fs.pop(str(p), None) is not None,
                              listdir=lambda d: self._listdir(d))
        # ⚠ 必须与固件一致(page_lua.c): 命名空间是**固定的** "luaapp"，
        #    签名里没有 sp 参数 —— get_int(key) / set_int(key,v) / get_str(key) / set_str(key,v)
        #    且 get_* 在键不存在时返回 **0 个值**(即 nil)，不是默认值。
        #    NVS 键名上限 15 字符(ESP-IDF 限制)。
        #    (原先这里多了一个 sp 形参 → 用模拟器写出来的 app 在真机上会写到错的键上)
        a.nvs = self.lua.table(get_int=lambda k: self.nvs.get(("luaapp", str(k))),
                               set_int=lambda k, v: (self.nvs.__setitem__(("luaapp", str(k)), v), True)[1],
                               get_str=lambda k: self.nvs.get(("luaapp", str(k))),
                               set_str=lambda k, v: (self.nvs.__setitem__(("luaapp", str(k)), str(v)), True)[1])
        # 软键盘 kbd_lib 统一入口 (与固件 page_lua.c 的 app.kbd 对齐)
        _kb = self.kbd
        a.kbd = self.lua.table(
            use=lambda name, *ar: _kb.use(name, *ar),
            reset=lambda: _kb.reset(),
            draw=lambda press=-1: self.kbd_draw(press),
            hit=lambda tx, ty: _kb.hit(int(tx), int(ty)),
            geom=lambda: self.lua.table(**_kb.geom()),
            height=lambda: _kb.height(),
            count=lambda: len(_kb.keys),
            press=lambda i: _kb.press(int(i)),
            char=lambda i: _kb.char(int(i)),
            label=lambda i: _kb.label(int(i)),
            layer_on=lambda i: _kb.layer_on(int(i)),
            get=lambda k="caps": (getattr(_kb, "shift" if (k or "c")[0] == "s" else
                                          ("fn_mode" if (k or "c")[0] == "f" else "caps"))),
            set=lambda k, v: self._kbd_set(k, v),
            CHAR=0, ENTER=1, BKSP=2, SPACE=3, ESC=4, TAB=5,
            UP=6, DOWN=7, LEFT=8, RIGHT=9, CAPS=10, SHIFT=11, FN=12, CTRL=13, ALT=14,
            ROWS=5, ROW=29, ROW_EDIT=19, H=145,
            STYLE_STD=0, STYLE_EDIT=1, STYLE_ROWS=2, STYLE_DIALOG=3,
        )
        a.db = self.lua.table(set=lambda k, v: True, get=lambda k: None,
                              seti=lambda k, v: True, geti=lambda k: None, del_=lambda k: True)
        a.net = self.lua.table(is_connected=lambda: True, get_ip=lambda: "192.168.1.23",
                               http_get=lambda *ar: self._http_get(*ar),
                               ping=lambda ip, c=4, t=500: self._ping(ip, c),
                               port=lambda ip, p, ms=300: int(p) in (80, 443, 8080, 22, 21, 445) and
                                                          int(str(ip).rsplit(".", 1)[-1]) % 3 != 0,
                               arp=lambda ip: "A4:5E:60:%02X:11:22" % (int(str(ip).rsplit(".", 1)[-1]) % 256),
                               arp_list=lambda: self._arp_list(),
                               ipinfo=lambda: ("192.168.1.23", "255.255.255.0", "192.168.1.1",
                                               "192.168.1.1", "WIFI_STA_DEF", "A4:5E:60:AA:BB:CC"),
                               vendor=lambda mac: VENDOR_MAP.get(str(mac)[:8].upper()),
                               fetch_start=lambda url, t=8000: self._fetch_start(url, t),
                               fetch_poll=lambda: self._fetch_poll())
        # JSON 模块 (与固件 luat_lib_json.c 同契约: 只物化用得到的字段)
        a.json = self.lua.table(pick=lambda b, k="", f="", m=200: self._json_pick(b, k, f, m),
                                num=lambda b, k, d=0: self._json_num(b, k, d),
                                str=lambda b, k: self._json_str(b, k),
                                bool=lambda b, k: self._json_bool(b, k))
        a.wifi = self.lua.table(init=lambda: True, release=lambda: True, scan=lambda: True,
                                scan_done=lambda: True,
                                ap_count=lambda: len(FAKE_APS), ap_info=lambda i: self._ap_info(i),
                                ch_stat=lambda: self._ch_stat(), probe_init=lambda: True,
                                probe_stop=lambda: True,
                                probe_count=lambda: len(FAKE_PROBES), probe_info=lambda i: self._probe_info(i),
                                cur_ssid=lambda: "LinTOS-Home", connecting=lambda: False,
                                connect=lambda s, p="": True, clear_saved=lambda: None)
        # 弹窗模块 app.dlg: 与固件 os_dialog 同款 (居中 + 2px 框 + 12px 内边距 + 高 40)
        a.dlg = self.lua.table(toast=lambda msg, ms=0: self._dlg_toast(msg, ms))
        a.hid = self.lua.table(start=lambda: True, stop=lambda: True, connected=lambda: True,
                               running=lambda: True, cdc_connected=lambda: True,
                               key=lambda m, k: None, key_str=lambda s: None,
                               mouse_move=lambda dx, dy: None, mouse_click=lambda b: None,
                               shortcut=lambda n: None, cdc_read=lambda n=256: self._cdc_read(n))

    def _kbd_set(self, k, v):
        k = (k or "c")[0]
        if k == "s":   self.kbd.shift = bool(v)
        elif k == "f": self.kbd.fn_mode = int(v) % 3
        else:          self.kbd.caps = bool(v)
        return None

    # ---- 触摸 ----
    def _key_pop(self):
        if self.kfifo:
            return self.kfifo.pop(0)
        return None

    def _applog(self, *args):
        """app.log(...): 拼成一行存进 self.applog + 打到 stdout (对标固件 ESP_LOGI "[APP]")"""
        line = " ".join(str(x) for x in args)
        self.applog.append(line)
        print("  [APP] " + line)
        return None

    def _key_repeat(self, a, first=400, rate=90):
        """app.key_repeat(a, first, rate): 按下沿立即 true; 按住超 first 后每 rate 一次 true。
        与固件的 s_rep_t0/s_rep_last 逻辑逐行对齐 (固件用 esp_timer, 这里用 self.ms)。"""
        a = int(a)
        if not (1 <= a <= 6):
            return False
        rate = max(20, int(rate))
        now = self.ms
        if a not in self.kdown:
            self.krep.pop(a, None)
            return False
        st = self.krep.get(a)
        if st is None or st[0] is None:          # 本次按下沿
            self.krep[a] = [now if now else 1, now]
            return True
        if (now - st[0]) >= int(first) and (now - st[1]) >= rate:
            st[1] = now
            return True
        return False

    def _touch(self):
        if self.touch_xy is None:
            return None
        return (int(self.touch_xy[0]), int(self.touch_xy[1]))

    def _touch_start(self):
        if self.touch_xy is None or self.touch_from is None:
            return None
        return (int(self.touch_from[0]), int(self.touch_from[1]))

    # ---- 假数据 ----
    def _ap_info(self, i):
        i = int(i)
        if not (0 <= i < len(FAKE_APS)):
            return None
        ssid, rssi, ch, auth, mac = FAKE_APS[i]
        return (rssi, ssid, ch, auth, mac)

    def _probe_info(self, i):
        """与固件一致: mac, rssi, channel, ssid, count (5 个返回值)。"""
        i = int(i)
        if not (0 <= i < len(FAKE_PROBES)):
            return None
        mac, rssi, ch = FAKE_PROBES[i]
        ssid = "" if i % 3 else "Hidden_AP_%d" % i      # 探针多为隐藏 SSID
        return (mac, rssi, ch, ssid, 3 + i * 2)

    def _seed_store(self):
        """把工程的 apps/lua/ 挂到设备路径 /store/ 下。

        没有这一步, 模拟器里 app.fs.read("/store/...") 全是 nil —— 依赖素材的 Lua 应用
        (江湖行 / QQ堂冒险) 在本地根本跑不起来, 只能靠烧录试错。这正是"先在电脑上测"卡住的原因。
        """
        root = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "apps", "lua")
        if not os.path.isdir(root):
            return
        n = 0
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                try:
                    if os.path.getsize(full) > (2 << 20):
                        continue
                    self.fs["/store/" + rel] = open(full, "rb").read()
                    n += 1
                except OSError:
                    pass
        self.log.append("[sim] /store 预载 %d 个文件" % n)

    def _seed_fs(self):
        """预置几个白板存档 (路径/命名与固件 WB_DIR 一致: /sdcard/App/图片/wbHHMMSSSS.bmp)。

        内容是真 1-bit BMP (62 字节头 + 每行 52 字节), 画一条对角线 → 载入后能看出确实读了文件。
        """
        DIR = "/sdcard/App/图片"
        rowb, off = 52, 62
        for name, k in (("wb09300001.bmp", 1), ("wb14300007.bmp", 2), ("wb18120012.bmp", 3)):
            buf = bytearray(b"\xff" * (off + rowb * H))       # 全白 (BMP bit1 = 白)
            # 头部与固件 wb_bmp_write 逐字节一致 (否则"载入→保存"的 md5 回归判据没意义)
            h = bytearray(54)
            fsz = off + rowb * H
            h[0:2] = b"BM"
            h[2:6] = fsz.to_bytes(4, "little")
            h[10:14] = off.to_bytes(4, "little")
            h[14:18] = (40).to_bytes(4, "little")
            h[18:22] = W.to_bytes(4, "little")
            h[22:26] = H.to_bytes(4, "little")
            h[26:28] = (1).to_bytes(2, "little")
            h[28:30] = (1).to_bytes(2, "little")
            h[34:38] = (rowb * H).to_bytes(4, "little")
            h[46:50] = (2).to_bytes(4, "little")
            buf[0:54] = h
            buf[54:58] = bytes((0, 0, 0, 0))
            buf[58:62] = bytes((255, 255, 255, 0))
            # ⚠ 墨点只画在**画布区** (x>=64, y>=32): 白板的基线与导出只覆盖画布区,
            #   画到顶栏/工具列的部分本来就存不下来, 会造成"往返不无损"的假象。
            for y in range(32, H):
                fy = H - 1 - y
                for d in range(40):
                    x = 64 + ((k * 70 + d + (y - 32) * 2) % 330)
                    buf[off + fy * rowb + (x >> 3)] &= ~(0x80 >> (x & 7))   # 墨 = BMP bit 0
            # 行尾填充 (400 px → 50 字节, 对齐到 52) 必须是 0: 固件的 memset(line,0,stride) 是这样
            for fy in range(H):
                buf[off + fy * rowb + 50] = 0
                buf[off + fy * rowb + 51] = 0
            self.fs["%s/%s" % (DIR, name)] = bytes(buf)

    def _http_get(self, *ar):
        """app.net.http_get(host, port, path, timeout) → body 或 nil。netdect 取公网信息时用。"""
        if ar and "ip9" in str(ar[0]):
            return FAKE_PUBJSON
        return None

    # ---- 异步 HTTPS (app.net.fetch_start / fetch_poll) ----
    # 固件把它丢到后台任务, 脚本每帧 poll; 模拟器用"隔两帧再给结果"复现这个节奏
    # (让脚本里的 loading 分支真的被走到)。
    def _fetch_start(self, url, timeout=8000):
        u = str(url)
        # ★ 本地回环 → **走真实 HTTP**（2026-10-04 加：为了在本地把"应用商店联网下载"
        #   这条链路跑通 —— 脚本指向 http://127.0.0.1:PORT/... 就能真的拉到文件）。
        #   其余仍用假 payload，保证既有应用（测速/航班雷达）的回归不变。
        if u.startswith("http://127.0.0.1") or u.startswith("http://localhost"):
            try:
                import urllib.request
                with urllib.request.urlopen(u, timeout=max(1, int(timeout) // 1000)) as r:
                    body = r.read()
                self._fetch = {"url": u, "n": 1, "body_bytes": body, "code": 200}
            except Exception as e:
                self._fetch = {"url": u, "n": 1, "err": str(e)}
            return True
        if "ip.sb" in u:
            payload = FAKE_GEO_IPSB
        elif "ip-api" in u:
            payload = FAKE_GEO_IPAPI
        else:
            payload = FAKE_ADSB
        self._fetch = {"url": u, "n": 2, "body": payload}
        return True

    def _fetch_poll(self):
        """返回 1 或 3 个值 —— 必须是 tuple, lupa 才会展开成多返回值。"""
        st = getattr(self, "_fetch", None)
        if not st:
            return ("idle",)
        if st["n"] > 0:
            st["n"] -= 1
            return ("wait",)
        if "err" in st:
            err = st["err"]
            self._fetch = None
            return ("err", "", -1, err)
        if "body_bytes" in st:                    # 本地真实 HTTP 的字节体
            b = st["body_bytes"]
            self._fetch = None
            # ⚠ lupa 把 Python str 按 UTF-8 编回 Lua 串 ⇒ 只有**合法 UTF-8**能无损往返。
            #   商店包是 .lua 文本(UTF-8) ✓ 没问题; 纯二进制要走 hex 通道(见技能)。
            try:
                return ("ok", b.decode("utf-8"), 200)
            except UnicodeDecodeError:
                return ("ok", b.decode("latin-1"), 200)
        body = st.get("body", FAKE_ADSB)
        self._fetch = None
        return ("ok", body, 200)

    # ---- JSON (app.json.*) ----
    # 契约与固件一致: pick 只保留白名单字段, 缺字段则键不出现 (与 lua_setfield 不写该键等价)
    @staticmethod
    def _json_load(body):
        import json as _json
        try:
            return _json.loads(str(body))
        except Exception:
            return None

    def _json_pick(self, body, akey="", fields="", mx=200):
        want = [f for f in str(fields).split(",") if f]
        out = self.lua.table()
        d = self._json_load(body)
        arr = d.get(str(akey)) if isinstance(d, dict) else None
        if not isinstance(arr, list):
            return out
        k = 0
        for o in arr[: max(0, int(mx))]:
            if not isinstance(o, dict):
                continue
            row = self.lua.table()
            for f in want:
                v = o.get(f)
                if isinstance(v, (int, float, str, bool)):
                    row[f] = v
            k += 1
            out[k] = row
        return out

    def _json_num(self, body, key, default=0):
        d = self._json_load(body)
        v = d.get(str(key)) if isinstance(d, dict) else None
        try:
            return float(v)
        except (TypeError, ValueError):
            return default

    def _json_str(self, body, key):
        d = self._json_load(body)
        v = d.get(str(key)) if isinstance(d, dict) else None
        return v if isinstance(v, str) else None

    def _json_bool(self, body, key):
        d = self._json_load(body)
        return bool(d.get(str(key))) if isinstance(d, dict) else False

    def _ping(self, ip, c=4):
        """app.net.ping → **Lua 表** {ok, rtt, ok, rtt, ...} (与固件 lua_net_ping 一致)。

        ⚠ 不能返回 Python tuple: lupa 会把它展开成多个返回值, `local r = app.net.ping(...)`
          只会拿到第一个数, 后面 `r[1]` 直接报 "attempt to index a number value"。
        假数据: 末位 1(网关)/10/20/23/100/105 视为在线, 其余超时 → 主机列表有内容也有空段。
        """
        last = int(str(ip).rsplit(".", 1)[-1])
        alive = last in (1, 10, 20, 23, 100, 105)
        t = self.lua.table()
        for i in range(int(c)):
            t[i * 2 + 1] = alive
            t[i * 2 + 2] = 8 if alive else 0
        return t

    def _dlg_toast(self, msg, ms=0):
        """app.dlg.toast: 记下提示, 由 render_frame 按固件样式叠加 (框架统一, 应用只给文字)。

        时长与固件一致: ms=0 → 0.5s (TOAST_MS); 到点自动消失。
        """
        self.toast_msg = str(msg)
        self.toast_until = self.ms + (int(ms) if int(ms) > 0 else 500)
        self.redraw_pending = True

    def _clock(self):
        """本地墙上时钟 (时, 分, 秒)。固定 14:30 便于复现截图 (固件走 time()/localtime)。"""
        return (14, 30, 0)

    def _listdir(self, d):
        """与固件一致: 返回**数组** (1..n 的条目名, 跳过 "." 开头), 不是 {名=值} 表。

        ⚠ 这个语义踩过坑: 脚本用 pairs() 迭代会拿到下标 1/2/3…, 把下标当文件名。
        """
        d = str(d).rstrip("/")
        names = []
        for k in self.fs:
            if d and not k.startswith(d + "/"):
                continue
            rest = k[len(d) + 1:] if d else k
            if "/" in rest or rest.startswith("."):
                continue
            names.append(rest)
        t = self.lua.table()
        for i, nm in enumerate(sorted(names)):
            t[i + 1] = nm
        return t

    def _ch_stat(self):
        # 真实感分布: 2.4G 的 1/6/11 是非重叠主信道, 噪声底 >= 1 个 beacon
        base = [22, 7, 5, 3, 9, 16, 6, 4, 2, 8, 21, 5, 3, 1]
        t = self.lua.table()
        for ch in range(14):      # 固件 WP_CH_COUNT = 14
            b = base[ch] if ch < len(base) else 1
            t[ch + 1] = self.lua.table(beacon=b, probe=(b * 7) // 3)
        return t

    def _arp_list(self):
        t = self.lua.table()
        for i in range(5):
            t[i + 1] = self.lua.table(ip="192.168.1.%d" % (i + 10),
                                      mac="A4:5E:60:%02X:11:22" % (0x10 + i))
        return t

    def _cdc_read(self, n=256):
        if self.cdc_queue:
            return self.cdc_queue.pop(0)
        return None

    # ================= 运行 =================
    def _bmpr(self, x, y, w, h, hexdata):
        """大位图直绘 (app.bmpr): 与固件 st7305_draw_bitmap_1bit 同语义 —— MSB 在前, 1=黑, 0=透明。"""
        try:
            b = bytes.fromhex(str(hexdata))
        except ValueError:
            return
        x, y, w, h = int(x), int(y), int(w), int(h)
        rowb = (w + 7) // 8
        for row in range(h):
            for col in range(w):
                i = row * rowb + (col >> 3)
                if i < len(b) and (b[i] >> (7 - (col & 7))) & 1:
                    self.px(x + col, y + row, True)

    def _patch_binary_channels(self):
        """二进制安全补丁: lupa 默认把 Lua 串按 UTF-8 解码成 Python str, 内容一旦含非 UTF-8
        字节 (1-bit BMP 里的 0x90/0xFF) 就 UnicodeDecodeError —— 且异常发生在"进入 Python
        之前", 回调里 try/except 也救不回来。所以**传二进制**的桥统一走 hex 通道。

        - app.fs.write(p, data)    data 为串时先 encode 成 hex
        - app.bmpr(x,y,w,h, data)  data 为串时先 encode 成 hex
        """
        self.lua.execute("""
            local _hex = function(d)
              local t = {}
              for i = 1, #d do t[i] = string.format("%02x", d:byte(i)) end
              return table.concat(t)
            end
            local _w = app.fs.write
            app.fs.write = function(p, d)
              if type(d) ~= "string" then return _w(p, d) end
              return _w(p, _hex(d))
            end
            local _r = app.bmpr
            app.bmpr = function(x, y, w, h, d)
              if type(d) ~= "string" then return _r(x, y, w, h, d) end
              return _r(x, y, w, h, _hex(d))
            end
        """)

    def load(self, path):
        src = open(path, encoding="utf-8").read()
        self.chunk = self.lua.eval("function(src, name) return load(src, name) end")(
            src, "@" + os.path.basename(path))
        if self.chunk is None:
            raise SystemExit("Lua 载入失败(语法错?)")
        self.globals = self.lua.globals()
        self.globals.app = self.app
        self.chunk()
        self._patch_binary_channels()      # 必须在脚本求值后打 (要包住脚本里拿到的 app.*)

    def call(self, name, *args):
        f = self.globals[name]
        if f is None:
            return None
        try:
            return f(*args)
        except Exception as e:
            self.log.append("%s() 出错: %s" % (name, e))
            raise

    def _draw_toast_overlay(self):
        """把 app.dlg.toast 的提示按固件 os_dialog 样式叠到帧缓冲上 (全局顶层覆盖)。

        几何与固件 dlg_draw_toast 逐条对齐: 2px 黑框 + 左右各 12px 内边距 + 高 40,
        24px 文字上下各 8px, 严格居屏。到点自动消失 (与固件一致)。
        """
        if not self.toast_msg or self.ms >= self.toast_until:
            return
        PAD, BORD, Hh = 12, 2, 40
        w = (BORD + PAD) * 2 + self.textw(self.toast_msg)
        if w > W - 40:
            w = W - 40
        x0, y0 = (W - w) // 2, (H - Hh) // 2
        self.fill(x0, y0, x0 + w - 1, y0 + Hh - 1, False)          # 白底
        for k in range(BORD):
            self.hline(x0 + k, x0 + w - 1 - k, y0 + k, True)
            self.hline(x0 + k, x0 + w - 1 - k, y0 + Hh - 1 - k, True)
            self.vline(x0 + k, y0 + k, y0 + Hh - 1 - k, True)
            self.vline(x0 + w - 1 - k, y0 + k, y0 + Hh - 1 - k, True)
        self.textc(y0 + (Hh - 24) // 2, self.toast_msg, False)

    def render_frame(self, label, outdir, scale=2, idx=0):
        self._draw_toast_overlay()
        img = Image.new("1", (W, H), 1)
        px = img.load()
        for y in range(H):
            row = y * W
            for x in range(W):
                px[x, y] = self.fb[row + x]
        img = img.convert("L").resize((W * scale, H * scale), Image.NEAREST)
        os.makedirs(outdir, exist_ok=True)
        name = "%02d_%s.png" % (idx, label.replace("/", "_").replace(" ", "").replace(":", "-"))
        p = os.path.join(outdir, name)
        img.save(p)
        return p


def press(sim, x, y):
    """按下沿: 置触摸点 → poll(首次) → 抬起 → poll。"""
    sim.t_was = False
    sim.touch_xy = (x, y)
    sim.touch_from = (x, y)
    sim.t_down_ms = sim.ms
    sim.call("poll")
    sim.t_was = True
    sim.touch_xy = None
    sim.touch_from = None
    sim.call("poll")


def run_steps(sim, steps, outdir, scale, verbose=False):
    shots = []
    sim.call("on_enter")
    n = 0
    for st in steps:
        parts = st.split(":", 1)
        kind = parts[0].strip()
        arg = parts[1] if len(parts) > 1 else ""
        label = st
        if kind == "touch":
            x, y = (int(v) for v in arg.split(","))
            press(sim, x, y)
        elif kind == "hold":
            x, y = (int(v) for v in arg.split(","))
            if sim.touch_xy is None:
                sim.touch_from = (x, y)
                sim.t_down_ms = sim.ms
            sim.touch_xy = (x, y)
            sim.call("poll")
        elif kind == "release":
            # release[:N,...] —— 同时管两件事:
            #   · 触摸: 总是抬起 (保持老行为, 老的 release 无参数)
            #   · 按键: 给了参数只松列出的动作码; 没给参数则全部松开
            sim.touch_xy = None
            sim.touch_from = None
            sim.ms += 40
            if arg:
                for v in str(arg).split(","):
                    if v.strip():
                        sim.kdown.discard(int(v))
            else:
                sim.kdown.clear()
            sim.call("poll")
        elif kind == "move":
            if "," in arg:
                # move:X,Y = 按住拖到 (X,Y) (配合 hold 用, 画线/拖动)
                x, y = (int(v) for v in arg.split(","))
                sim.touch_xy = (x, y)
                sim.ms += 40
                sim.call("poll")
            else:
                for _ in range(int(arg) if arg else 1):
                    sim.ms += 40
                    sim.call("poll")
        elif kind == "key":
            # 与固件 lua_action 完全一致: key_push((int)a) → action(a); 然后下一次 poll 会取走。
            # ⚠ 先推进 40ms —— 真人按键有间隔(>100ms), 不推进的话"连按同一个键"会被应用的
            #   防抖(同值 12ms 内只算一次)吃掉, 造成"那一步没生效"的假象。
            sim.ms += 40
            sim.kfifo.append(int(arg))
            sim.call("action", int(arg))
            # ⚠ 必须紧跟一帧 poll —— 固件里 lua_poll 每帧都跑, 应用正是在 poll() 里读 app.key()
            sim.call("poll")
        elif kind == "press":
            # press:N = 按住第 N 个动作键 (不产生边沿, 只置"按住"态) → 下一次 poll 供 key_down 用
            sim.ms += 40
            sim.kdown.add(int(arg))
            sim.call("poll")
        elif kind == "rpt":
            # rpt:N = 只推进时间并调 poll (不重新触发边沿), 用来测 key_repeat 的自动重复
            sim.ms += int(arg) if arg else 40
            sim.call("poll")
        elif kind == "enter":
            continue
        elif kind != "render":
            raise SystemExit("未知步骤: %s" % st)
        sim.call("render")
        n += 1
        shots.append(sim.render_frame(label, outdir, scale, n))
    if verbose:
        sim.call("on_exit")
    return shots


# ---- 各应用默认场景 (没给 --steps 时用) ----
DEFAULT_STEPS = {
    "logdiag": "render;touch:200,76;render;move:30;touch:200,118;render;move:40;touch:200,160;render;key:6;render",
    "term": "render;touch:200,60;render;move:5;touch:200,120;render;key:6;render",
    # 231,233 = 假数据里最强 AP(LinTOS-Home) 的球心, 见 ap_polar/ap_radius 几何
    "wifiprobe": "render;touch:231,233;render;key:4;render;key:4;render;key:6;render",
    # wb: 画一笔 → 开载入列表(顶部"加载"按钮 x=360) → 长按第1行 880ms 触发删除确认
    #     → 松手 → 点确认框左半"确定" → 列表少一项
    # ⚠ 长按之后必须显式 release: 不松手就再 press, 脚本侧的"按下沿"标志还是 true → 点不响应。
    "wb": "render;touch:60,150;hold:60,150;move:2;move:60,170;release;render;"
          "touch:360,16;render;hold:200,100;move:22;render;release;render;"
          "touch:165,168;render",
    # netdect: 进入(1s延迟) → 列表 → 右下"下一步"开跑 → 每步 key:5 继续 → 走到报告
    # ⚠ 每个 key:5 后面必须给 tick (move): 引擎跑在 poll 里, 不给 tick 它不会推进
    "netdect": "render;move:30;render;touch:315,283;render;move:6;render;"
               "key:5;render;move:200;render;key:5;render;move:200;render;"
               "key:5;render;move:40;render;key:5;render;move:40;render",
}
GENERIC_STEPS = ("render;touch:200,70;touch:200,108;touch:200,146;touch:200,184;"
                 "touch:200,222;touch:200,260;key:5;key:2;key:3;key:4;key:6;move")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--steps", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    name = os.path.splitext(os.path.basename(args.file))[0]
    outdir = args.out or os.path.join(os.path.expanduser("~/Desktop/LinTOS_Lua应用/sim_out"), name)
    steps = (args.steps or DEFAULT_STEPS.get(name, GENERIC_STEPS)).split(";")
    steps = ["enter"] + [s for s in steps if s.strip()]

    sim = Sim(verbose=args.verbose)
    sim.load(args.file)
    shots = run_steps(sim, steps, outdir, args.scale, args.verbose)
    print("=== %s 模拟完成: %d 帧 → %s ===" % (name, len(shots), outdir))
    print("    状态栏反白(app.sbar): %s" % ("开" if sim.sbar_invert else "关"))
    for s in shots:
        print("  " + os.path.basename(s))
    if sim.log:
        print("--- 运行日志 ---")
        for l in sim.log:
            print("  " + l)


if __name__ == "__main__":
    main()
