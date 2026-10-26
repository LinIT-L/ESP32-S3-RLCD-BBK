#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""map_pack.py — 打包「矢量地图包」: 出 TF 拷贝目录 + 出公网托管索引

地图应用 (apps/lua/apps/app/map.lua) **只从 TF 卡读** `/sdcard/Map/*.ltm`。
本脚本把 tools/map_vec.py 编译出来的 .ltm 整理成两份产物:

  ① dist/tf/Map/            直接整目录拷到 TF 卡根目录 (插卡即用)
     dist/tf/读我.txt
  ② store/maps/*.ltm        发布到托管站 (bbk.linit.cn) 的文件
     store/maps.json        设备端索引: mirror + files[{path,size,f16,t}]
                            ↑ 应用用 app.net.fetch_start + app.json.pick 读它

⛔ 校验和为什么用 **Fletcher-16 而不是 CRC32**
   设备端 Lua 是 LUA_32BITS=1, lua_Integer 只有 32 位。CRC32 的值常 > 2^31
   (实测 putian_road.ltm = 2794397018), 在 Lua 里既拿不到精确整数, 也过不了
   `%08x` 格式化 (会被拒 "number has no integer representation")。
   Fletcher-16 全程只用到 <65536 的数, 在 32 位整数和 float 下都精确;
   再配一个**字节数**比对, "下载被截断"和"内容被改坏"就都能抓到。

用法:
    python3 tools/map_pack.py                        # 用默认镜像
    python3 tools/map_pack.py --mirror https://bbk.linit.cn/bbk
    python3 tools/map_pack.py --dry-run
"""
import argparse
import json
import os
import shutil
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 地图数据放 dist/map/ 而不是 build/map/ —— build/ 会被 fullclean 清掉, 而 .ltm
# 要重新跑 Overpass + DEM 才能生成 (几分钟), 不该跟构建产物一起被删。
# 兼容旧的 build/map/。
SYRCS = [os.path.join(ROOT, "dist", "map"), os.path.join(ROOT, "build", "map")]
SRC = SYRCS[0]
TF_OUT = os.path.join(ROOT, "dist", "tf", "Map")
STORE_OUT = os.path.join(ROOT, "store", "maps")
INDEX = os.path.join(ROOT, "store", "maps.json")

# ★ 多源（换源）：设备端按**顺序**试，第 1 个挂了自动换下一个。
#   ⚠ 顺序 = 尝试顺序，所以把最快/最稳的放前面；境外源在国内慢，放最后。
#   与应用商店 (tools/store_build.py 的 DEFAULT_MIRRORS) 用**同一批源**，便于统一维护。
DEFAULT_MIRRORS = [
    "https://bbk.linit.cn/bbk",                                      # 主源：自有站（国内）
    "https://gitee.com/LinIT-L/ESP32-S3-RLCD-BBK/raw/master/store",  # 备源：Gitee raw（国内）
    # "https://raw.githubusercontent.com/<user>/<repo>/main/store",   # 以后挂 GitHub 时取消注释
    # "https://cdn.jsdelivr.net/gh/<user>/<repo>@main/store",         # 或走 jsDelivr CDN
]

# 地图包 = 一个区域一组文件。设备端会把 files[] 全部下到 /sdcard/Map/。
#
# ★ 两种包形态并存 (设备端 map.lua 都认):
#   ① 单文件包: 每个图层一个 .ltm (putian_*), 无清单 —— 老形态, 小区域够用
#   ② **省级单文件包**: `<id>.json` 索引 + `<id>.bin` 数据体
#      为什么不是"一瓦片一个 .ltm": 607 路块 + 564 标块 = 1171 个文件 ⇒
#      1171 次 HTTP 请求, 设备端二十分钟下不完。整省平铺进一个 bin,
#      索引记每块 (off,size), 设备端用 app.fs.read_at 精确读视口那几块 ——
#      **一次请求下完整省**。见 docs/矢量离线地图-方案与实现.md「省级包」。
PACKS = [
    dict(name="fj", label="示例省",
         # bin 放前面: 中途中断时"有 bin 没 json" ⇒ 设备端根本认不出包,
         # 干净地退回"未找到地图包"; 反过来会得到一个"半残的包"。
         files=[("bin", "fj.bin"), ("index", "fj.json")],
         sub="fj"),
    dict(name="putian", label="示例市区",
         files=[("road", "putian_road.ltm"), ("contour", "putian_contour.ltm"),
                ("poi", "putian_poi.ltm")]),
]

# ★ 多大的文件就不算 Fletcher-16 了。
#   设备端对大文件走**流式落盘**(整包不进内存), 本来就没法对一个 18 MB 的文件算校验和;
#   而 host 侧纯 Python 算 18 MB 也要十几秒。大文件的完整性靠三重保证:
#     ① HTTPS 传输完整性  ② 固件侧 Content-Length 比对 (短包必拒)  ③ 块内 LTM1 结构自检
FLETCH_MAX = 900 * 1024

README = """LinTOS 矢量离线地图 — TF 卡数据包
=====================================

把本目录下的 **Map 文件夹** 整个拷到 TF 卡根目录, 变成:

    /sdcard/Map/<id>.json + <id>.bin   分块包 (示例: 全省 18 MB)
    /sdcard/Map/putian_road.ltm        示例市区 道路 (矢量折线 + 路名)
    /sdcard/Map/putian_contour.ltm     示例市区 等高线 (20m 等高距)
    /sdcard/Map/putian_poi.ltm         示例市区 地标

然后插卡, 打开应用《矢量地图》即可。装了多个包时在「设置 → 地图包」里切换。

⚠ 应用**只从 TF 卡读地图**, 没插卡时会拒绝加载并提示。

也可以不拷: 连上 WiFi 后在应用里点「联网下载」,
会从 https://bbk.linit.cn/bbk/maps.json 自动下载到 TF 卡。

覆盖范围: 由 --bbox 指定 (示例: 115.8~120.4E, 23.6~28.3N)
数据来源: 道路/路名/地标 © OpenStreetMap 贡献者 (ODbL) · 高程 NASA/USGS SRTM (terrarium)
"""


def fletcher16(data):
    """与设备端 map.lua 的 fletcher16 逐字节同算法"""
    a = b = 0
    for by in data:
        a = (a + by) % 255
        b = (b + a) % 255
    return b * 256 + a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mirror", action="append", default=None,
                    help="镜像前缀，可给多次；给多次时按顺序尝试。缺省用 DEFAULT_MIRRORS")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    if not any(os.path.isdir(s) for s in SYRCS):
        raise SystemExit("找不到地图数据目录 (%s) —— 先跑 tools/map_vec.py roads / contour"
                         % " 或 ".join(SYRCS))

    def src_of(fn, sub=None):
        for s in SYRCS:
            p = os.path.join(s, fn) if not sub else os.path.join(s, sub, fn)
            if os.path.exists(p):
                return p
        return None

    files = []
    missing = []
    for pk in PACKS:
        for kind, fn in pk["files"]:
            p = src_of(fn, pk.get("sub"))
            if not p:
                missing.append("%s/%s" % (pk.get("sub", ""), fn))
                continue
            blob = open(p, "rb").read()
            # .bin 是块平铺, 没有整体 magic; .ltm 必须有 LTM1; .json 必须以 { 开头
            if fn.endswith(".ltm") and blob[:4] != b"LTM1":
                raise SystemExit("%s 不是 LTM1 文件" % fn)
            if fn.endswith(".json") and blob[:1] not in (b"{", b"["):
                raise SystemExit("%s 不是 JSON" % fn)
            big = len(blob) > FLETCH_MAX
            # ⚠ path 必须带 `maps/` 前缀: 设备端拼的是 `mirror .. "/" .. path`,
            #   而托管站布局是 bbk/maps/*.ltm。写裸文件名会 404。
            #   设备端落盘时只取 basename, 所以 TF 卡上仍是 /sdcard/Map/fj.bin。
            files.append(dict(t=kind, path="maps/" + fn, size=len(blob),
                              f16=(-1 if big else fletcher16(blob)),
                              _src=p, _blob=blob, _pack=pk["name"]))
    if missing:
        raise SystemExit("缺少地图数据: %s" % ", ".join(missing))
    if not files:
        raise SystemExit("没有可打包的文件")

    total = sum(f["size"] for f in files)
    print("地图包: %d 个文件, 共 %.1f MB%s"
          % (len(files), total / 1048576.0,
             " (校验: >%.0fKB 的只比长度)" % (FLETCH_MAX / 1024) if any(f["f16"] < 0 for f in files) else ""))
    for f in files:
        print("   %-28s %9.1f KB  %s" % (f["path"], f["size"] / 1024.0,
                                         "f16=%5d" % f["f16"] if f["f16"] >= 0 else "只比长度"))
    if a.dry_run:
        print("[--dry-run] 未写盘")
        return

    # ---- ① TF 拷贝目录 ----
    os.makedirs(TF_OUT, exist_ok=True)
    for f in files:
        shutil.copyfile(f["_src"], os.path.join(TF_OUT, os.path.basename(f["path"])))
    with open(os.path.join(ROOT, "dist", "tf", "读我.txt"), "w", encoding="utf-8") as fh:
        fh.write(README)
    print("  ✓ dist/tf/Map/  (整目录拷到 TF 卡根目录即可)")

    # ---- ② 公网托管 ----
    os.makedirs(STORE_OUT, exist_ok=True)
    for f in files:
        shutil.copyfile(f["_src"], os.path.join(STORE_OUT, os.path.basename(f["path"])))
    mirrors = a.mirror if a.mirror else DEFAULT_MIRRORS
    idx = {
        "ver": 2,
        # 设备端: mirrors[i] .. "/" .. path，**按顺序回退**；第 1 个也单独留一份 "mirror"
        # 做向后兼容（老版本应用只认单源）。
        "mirrors": mirrors,
        "mirror": mirrors[0] if mirrors else "",
        "updated": time.strftime("%Y-%m-%d"),
        # ⚠ 每个包只列**自己**的文件: 老写法 `[f["path"] for f in files]` 会让每个包
        #   都列出全部文件 (两个包时看着像"每个包 5 个文件")。设备端用的是扁平 files[],
        #   这里只是给人看的元信息, 但写错会误导。
        "packs": [{"id": pk["name"], "label": pk["label"],
                   "files": [f["path"] for f in files if f["_pack"] == pk["name"]]}
                  for pk in PACKS],
        "files": [{k: f[k] for k in ("t", "path", "size", "f16")} for f in files],
    }
    with open(INDEX, "w", encoding="utf-8") as fh:
        json.dump(idx, fh, ensure_ascii=False, indent=2)
    print("  ✓ store/maps/*.ltm + store/maps.json")
    print("    镜像源 (%d 个, 按序回退):" % len(mirrors))
    for i, m in enumerate(mirrors, 1):
        print("      %d. %s" % (i, m))
    print("    发布: bash tools/store_deploy.sh          (默认值已配好)")
    print("    ⚠ 备源（Gitee/GitHub）要设备端能下到，同样得把 store/ 推上去；")
    print("      这次没推（linit: 不发 GitHub）—— Gitee 推完再跑一次本脚本即可。")


if __name__ == "__main__":
    main()
