#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""wuxia_single_test.py — 验证「英雄坛说」单文件包 (脚本 + 尾部 ZIP) 的运行时读取链路.

能验证什么 / 不能验证什么
------------------------
✅ 包结构: 头部 offset/size、ZIP 可解析、全部 STORED、条目落界 (tools/pack_single.py --check)
✅ Lua 侧逻辑: app.pkg.self() 打开自身包 → pkg_mod 读 map.lua/world.lua/data.lua 并 load 成表
   → pkg_img 调 app.img.load_mem 拿图 → on_enter 走完不报错
✅ 不再依赖 wuxia/ 目录 (桩里根本没有文件系统素材)
❌ BMP 字节的**解码**在固件 C 里 (img_decode), 主机桩只记录调用 → 需真机/固件验证

桩设计
------
lupa 传二进制到 Lua 会走 UTF-8 解码, 真实 BMP 会炸。所以:
  · *.lua 条目返回**真实文本** (验证 load 成表这条关键路径)
  · *.bmp 条目返回 64 字节 ASCII 假串 (只验证调用链; BMP 合法性另有校验)
"""
import io
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lua_sim import Sim          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(ROOT, "store/pkgs/英雄坛说.lua")

CHECKS = []


def ck(cond, msg):
    CHECKS.append((bool(cond), msg))
    return bool(cond)


def split_pkg(path):
    data = open(path, "rb").read()
    head = data[:2048]
    i = head.find(b"--@pkg")
    line = data[i:data.find(b"\n", i)]
    off = int(line.split(b"offset=")[1].split(b" ")[0])
    size = int(line.split(b"size=")[1].split(b" ")[0])
    return data[:off], data[off:off + size], off, size


def main():
    if not os.path.exists(PKG):
        print("✗ 找不到 %s (先跑 tools/pack_single.py)" % PKG)
        return 1
    script, zdata, off, size = split_pkg(PKG)
    z = zipfile.ZipFile(io.BytesIO(zdata))
    names = z.namelist()

    ck(off + size == os.path.getsize(PKG), "offset+size == 文件长度")
    ck(all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist()), "全部 STORED")
    ck("wuxia/map.lua" in names and "wuxia/world.lua" in names and "wuxia/data.lua" in names,
       "包内数据模块齐全")
    ck(any(n.startswith("wuxia/map") and n.endswith(".bmp") for n in names), "包内有地图 BMP")
    ck(all(n.startswith("wuxia/") for n in names), "条目都在 wuxia/ 前缀下")

    calls = {"read": [], "img": []}

    def pkg_self():
        return 1

    def pkg_read(h, name):
        try:
            raw = z.read(name)
        except KeyError:
            return None
        calls["read"].append(name)
        if name.endswith(".lua"):
            return raw.decode("utf-8")
        return "B" * min(len(raw), 64)          # 假 BMP 头, 只走调用链

    def pkg_size(h, name):
        try:
            return len(z.read(name))
        except KeyError:
            return None

    def img_load_mem(d, opaque=None):
        calls["img"].append(len(d) if isinstance(d, str) else 0)
        return len(calls["img"]) + 100          # 假句柄

    sim = Sim()
    src = script.decode("utf-8", errors="replace")
    sim.chunk = sim.lua.eval("function(src, name) return load(src, name) end")(
        src, "@英雄坛说.lua")
    if sim.chunk is None:
        print("✗ 脚本部分无法编译 (--@pkg 截断有问题?)")
        return 1
    sim.globals = sim.lua.globals()
    sim.globals.app = sim.app
    sim.chunk()

    # 注入 pkg 桩 (在脚本求值之后: 脚本顶层不读素材, on_enter 才读)
    tbl = sim.lua.table_from({
        "self": pkg_self, "read": pkg_read, "size": pkg_size,
        "exists": lambda h, n: n in names,
        "list": lambda h: sim.lua.table(),
        "close": lambda h: None,
    })
    sim.app["pkg"] = tbl
    sim.app["img"]["load_mem"] = img_load_mem

    # 走启动流程
    err = None
    try:
        sim.call("on_enter")
    except Exception as e:
        err = e
    ck(err is None, "on_enter 无异常%s" % ("" if err is None else ": %s" % err))

    state = None
    try:
        state = sim.lua.eval("GAME and GAME.state")
    except Exception:
        pass
    if state is not None:
        ck(state == "entry", "进入标题页 (state=%s, 素材加载成功)" % state)

    ck(any(n.endswith("map.lua") for n in calls["read"]), "读到包内 map.lua")
    ck(any(n.endswith("world.lua") for n in calls["read"]), "读到包内 world.lua")
    ck(any(n.endswith("data.lua") for n in calls["read"]), "读到包内 data.lua")
    ck(len(calls["img"]) >= 2, "图集走 app.img.load_mem (%d 次)" % len(calls["img"]))
    ck(not any(n.startswith("/") for n in calls["read"]), "没有走绝对路径 (不依赖目录)")

    # 渲染一帧 (走 RES.load_map / 精灵 blit 路径)
    err2 = None
    try:
        sim.call("render")
    except Exception as e:
        err2 = e
    ck(err2 is None, "首帧渲染无异常%s" % ("" if err2 is None else ": %s" % err2))

    bad = [m for ok, m in CHECKS if not ok]
    print("\n包: %s" % os.path.relpath(PKG, ROOT))
    print("  脚本 %d B / ZIP %d B / %d 条目" % (off, size, len(names)))
    print("  读取记录: %d 次 read, %d 次 load_mem" % (len(calls["read"]), len(calls["img"])))
    print("\n共 %d 项, 通过 %d, 失败 %d" % (len(CHECKS), len(CHECKS) - len(bad), len(bad)))
    for m in bad:
        print("  ✗ " + m)
    if not bad:
        print("全部通过 ✅ (注: BMP 解码在固件 C, 本测试只验证调用链)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
