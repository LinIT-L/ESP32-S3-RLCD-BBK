#!/usr/bin/env python3
"""pack_app.py — LinTOS 单文件应用打包器 (代码 + 图标 + 元信息 全在一个 .lua 里)

格式 (LAPP v1, 纯文本, 向后兼容普通 .lua):
    文件头是若干行 `--@app` 注释 —— **固件可静态解析(不执行代码)**;
    Lua 解释器把它们当注释忽略, 所以普通 .lua 直接就能跑。

        --@app name=电脑诊断            ← 列表显示名 (可中文)
        --@app cat=运维                  ← 分类: 程序/游戏/系统/运维/网络/学习
        --@app desc=按现象追问, 概率排序  ← 商店列表简介
        --@app version=1.0
        --@app need=wifi                 ← 能力声明: 启动前需要联网 (未连则先弹窗连网)
        --@app icon=<base64, 64x64 1bpp> ← 图标位图 (512B → base64 684 字符)
        --@lapp v1                       ← 格式标记 (存在即视为已打包)

    头部之后是打包器注入的**图标自绘前奏** (`_APPICON` + `blit_app_icon(x,y)`),
    再往后是**应用原始 Lua 代码**(一字未改)。

    → 一个文件即全部资源: 商店列表图标/名称/简介 = 固件静态解析头部;
      应用自己画图标 = 调 blit_app_icon(x, y) (读内嵌 base64, 不碰文件系统)。

用法:
    # 打包 (图标从桌面 PNG 取)
    python3 tools/pack_app.py pack apps/lua/diag.lua --name 电脑诊断 --cat 运维 \
            --desc "按现象追问, 概率排序给出故障配件与排除方案" \
            --icon <HOME>/Desktop/完美图标/应用管家/运维/电脑诊断.png
    # 图标也可直接引用固件已有图标 (从 icons_main.inc 按枚举名取 64x64 1bpp)
    python3 tools/pack_app.py pack apps/lua/netdect.lua --name 网络扫描 --cat 运维 \
            --icon-from-main ICON_NETDECT
    # 查看/校验
    python3 tools/pack_app.py verify apps/lua/diag.lua
    # 导出图标为 PBM (纯文本, 无需依赖, 任何看图软件可开)
    python3 tools/pack_app.py unpack apps/lua/diag.lua -o /tmp/diag_icon.pbm
"""
import argparse
import base64
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS_INC = os.path.join(ROOT, "components", "ui", "ui_common", "icons_main.inc")
MARK = "--@lapp v1"
ICON_W = ICON_H = 64
ICON_BYTES = (ICON_W * ICON_H) // 8          # 512


# ---------------- 位图工具 ----------------
def bits_from_png(path):
    """PNG → 64x64 1bpp (MSB first, 1=黑), 与 gen_main_icons_64.py 同规范"""
    from PIL import Image                     # 延迟导入: 仅 PNG 路径需要
    img = Image.open(path).convert("L")
    if img.size != (ICON_W, ICON_H):
        img = img.resize((ICON_W, ICON_H), Image.Resampling.LANCZOS)
    out = bytearray()
    for y in range(ICON_H):
        for b in range(ICON_W // 8):
            byte = 0
            for bit in range(8):
                if img.getpixel((b * 8 + bit, y)) < 128:
                    byte |= 1 << (7 - bit)
            out.append(byte)
    return bytes(out)


def bits_from_main(enum_name):
    """从 icons_main.inc 按枚举名取 64x64 1bpp 位图 (与固件图标同源, 保证一致)"""
    s = open(ICONS_INC, encoding="utf-8").read()
    m = re.search(r"ICON_%s\s*=\s*(\d+)" % re.escape(enum_name.replace("ICON_", "")),
                  open(os.path.join(ROOT, "components", "ui", "ui_common",
                                    "icons_main_enum.h"), encoding="utf-8").read())
    if not m:
        raise SystemExit("icons_main_enum.h 里找不到 %s" % enum_name)
    idx = int(m.group(1))
    # MAIN_ICON_COUNT 个数组, 每个 MAIN_ICON_BYTES 字节, 按定义顺序
    arrs = re.findall(r"const uint8_t \w+\[MAIN_ICON_BYTES\]\s*=\s*\{(.*?)\};", s, re.S)
    if idx >= len(arrs):
        raise SystemExit("icons_main.inc 数组数(%d) < 索引 %d" % (len(arrs), idx))
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", arrs[idx])]
    data = bytes(vals)
    if len(data) != ICON_BYTES:
        raise SystemExit("图标字节数异常: %d (期望 %d)" % (len(data), ICON_BYTES))
    return data


def bits_from_b64(s):
    d = base64.b64decode(s)
    if len(d) != ICON_BYTES:
        raise SystemExit("icon base64 解码后 %d 字节 (期望 %d)" % (len(d), ICON_BYTES))
    return d


def b64(bits):
    return base64.b64encode(bits).decode("ascii")


def to_pbm(bits, path):
    """导出 PBM(P1) 纯文本, 无依赖"""
    with open(path, "w", encoding="ascii") as f:
        f.write("P1\n%d %d\n" % (ICON_W, ICON_H))
        for y in range(ICON_H):
            row = []
            for x in range(ICON_W):
                byte = bits[y * (ICON_W // 8) + (x >> 3)]
                row.append("1" if (byte >> (7 - (x & 7))) & 1 else "0")
            f.write(" ".join(row) + "\n")


# ---------------- 头部解析 ----------------
def parse_header(text):
    meta = {}
    for m in re.finditer(r"^--@app\s+([A-Za-z_]+)=(.*)$", text, re.M):
        meta[m.group(1)] = m.group(2).strip()
    return meta


def strip_lapp(text):
    """去掉已存在的 LAPP 头部 + 图标前奏, 还原为应用原始源码。

    ⚠ 必须**完全幂等**: 之前只删了 `--@app` 头和一段 `_APPICON` 正则, 但漏掉了
    紧随的 `-- 图标自绘:` 与 `-- ── 以上为注入前奏` 两行, 于是**再打包一次前奏就叠一层**
    (实测 calc.lua 里 prelude 出现两遍)。这里改为按**成对标记**整段删除。"""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        ln = lines[i].strip()
        if ln.startswith("--@app ") or ln == MARK or ln.startswith("-- 图标自绘:"):
            i += 1
            continue
        break
    rest = "\n".join(lines[i:])
    # 主删法: 成对标记整段删除 (前奏开始标记 → 结束标记)
    rest = re.sub(r"-- ── LAPP 图标自绘前奏.*?-- ── 以上为注入前奏, 以下为应用原始代码 ──\n?",
                  "", rest, flags=re.S)
    # 兜底: 老版本可能没有标记行, 只剩 _APPICON 段
    rest = re.sub(r"^local _APPICON\s*=.*?^end\n", "", rest, flags=re.S | re.M)
    return rest.strip("\n")          # 首尾空行都收掉, 配合 do_pack 补的那一个 \n → 幂等


PRELUDE = """-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "%s"
local function blit_app_icon(x, y, inv)
  local d = _APPICON
  local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local buf, acc, nbits = {}, 0, 0
  for i = 1, #d do
    local ch = d:sub(i, i)
    if ch == "=" then break end
    local v = B64:find(ch, 1, true)
    if v then
      acc = acc * 64 + (v - 1); nbits = nbits + 6
      if nbits >= 8 then
        nbits = nbits - 8
        buf[#buf + 1] = math.floor(acc / (2 ^ nbits)) %% 256
        acc = acc %% (2 ^ nbits)
      end
    end
  end
  for row = 0, 63 do
    for col = 0, 63 do
      local idx = row * 8 + (col >> 3) + 1
      local byte = buf[idx] or 0
      local on = (math.floor(byte / (2 ^ (7 - (col %% 8)))) %% 2) == 1
      if on ~= (inv == true) then app.px(x + col, y + row, inv and app.WHITE or app.BLACK) end
    end
  end
end
-- ── 以上为注入前奏, 以下为应用原始代码 ──
"""


def do_pack(a):
    src = open(a.file, encoding="utf-8").read()
    old = parse_header(src)          # 已打包过的文件: 用来"只改一项、其余照旧"
    body = strip_lapp(src)
    # 图标: 显式给了就重取, 否则复用包里已有的 (只加 need= 之类时不必再指图标)
    if a.icon:
        bits = bits_from_png(a.icon)
    elif a.icon_from_main:
        bits = bits_from_main(a.icon_from_main)
    elif "icon" in old:
        bits = bits_from_b64(old["icon"])
    else:
        raise SystemExit("需要 --icon <png> 或 --icon-from-main ICON_XXX")
    name = a.name or old.get("name")
    cat  = a.cat  or old.get("cat")
    desc = a.desc or old.get("desc")
    need = a.need if a.need is not None else old.get("need")
    head = [MARK]
    if name: head.append("--@app name=%s" % name)
    if cat:  head.append("--@app cat=%s" % cat)
    if desc: head.append("--@app desc=%s" % desc)
    if need: head.append("--@app need=%s" % need)
    head.append("--@app version=%s" % (a.version or old.get("version") or "1.0"))
    head.append("--@app icon=%s" % b64(bits))
    out = "\n".join(head) + "\n" + "-- 图标自绘: blit_app_icon(x, y[, inv=true])\n" \
          + (PRELUDE % b64(bits)) + body + "\n"
    open(a.file, "w", encoding="utf-8").write(out)
    print("  ✓ 已打包 %s" % os.path.relpath(a.file, ROOT))
    print("     名称: %s   分类: %s   图标: %d B(b64 %d 字符)%s"
          % (name, cat, ICON_BYTES, len(b64(bits)), "   need=%s" % need if need else ""))
    do_verify(a)


def do_verify(a):
    text = open(a.file, encoding="utf-8").read()
    meta = parse_header(text)
    packed = text.lstrip().startswith(MARK)
    print("  文件: %s  (%d 行, %d 字节)" % (os.path.basename(a.file),
                                          text.count("\n") + 1, len(text)))
    print("  LAPP: %s" % ("是" if packed else "否 (普通 .lua, 仍可直接运行)"))
    for k in ("name", "cat", "desc", "version", "need"):
        if k in meta: print("  %-8s %s" % (k + ":", meta[k]))
    if "icon" in meta:
        try:
            bits = bits_from_b64(meta["icon"])
            print("  icon:     64x64 1bpp 解码 OK (%d 字节)" % len(bits))
        except SystemExit as e:
            print("  icon:     ✗ %s" % e)
    else:
        print("  icon:     无 (未打包图标)")
    print("  自绘前奏: %s" % ("有 blit_app_icon()" if "blit_app_icon" in text else "无"))
    return 0


def do_unpack(a):
    meta = parse_header(open(a.file, encoding="utf-8").read())
    if "icon" not in meta:
        raise SystemExit("该文件没有内嵌图标")
    to_pbm(bits_from_b64(meta["icon"]), a.out)
    print("  ✓ 图标已导出: %s (PBM 纯文本, 可直接查看)" % a.out)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack"); p.add_argument("file")
    p.add_argument("--name"); p.add_argument("--cat"); p.add_argument("--desc")
    p.add_argument("--version", default="1.0")
    p.add_argument("--need", help="能力声明, 逗号分隔; 目前支持 wifi (启动前门禁: 未联网则先弹窗连网)")
    p.add_argument("--icon"); p.add_argument("--icon-from-main")
    v = sub.add_parser("verify"); v.add_argument("file")
    u = sub.add_parser("unpack"); u.add_argument("file"); u.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    {"pack": do_pack, "verify": do_verify, "unpack": do_unpack}[a.cmd](a)


if __name__ == "__main__":
    main()
