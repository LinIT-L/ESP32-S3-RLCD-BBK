#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
migrate_store.py — 把存量商店包迁成 LPK 应用包/资源包（本地离线，不碰服务器）

做法：
  · type=lua（散装 .lua）      → 解 LAPP 头 → manifest → app/entry.lua + icon.bin → .lpk
  · type=asset 且 zip 内含 .lua → 视为"应用 + 素材包"，entry 指向包内那个 .lua
  · 其它（字体/纯素材/旧 lpk）  → 本地没有源文件的一次性列出原因，不伪造

输出：
  store/packages/<app_id>.lpk
  store/index.v2.json          （schema_version=2，设备端新协议）
  逐包迁移表（stdout）

⚠ 不覆盖 store/index.json（线上协议仍在用），也不上传服务器。
"""
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.environ.get("WORK", "/tmp/migrate_store")
PACKAGES = os.path.join(ROOT, "store", "packages")
PACK = os.path.join(ROOT, "tools", "pack_lpk.py")

# 中文名 → 稳定 app_id（一次性冻结；以后新增应用必须在这里登记或由 manifest 自带）
APP_ID = {
    "录音机":     "com.lintos.recorder",
    "航班雷达":   "com.lintos.flight-radar",
    "矢量地图":   "com.lintos.vector-map",
    "自行车码表": "com.lintos.bike-comp",
    "BIOS模拟器": "com.lintos.biossim",
    "人生指南":   "com.lintos.lifeguide",
    "英雄坛说":   "com.lintos.wuxia",
    "QQ堂冒险":   "com.lintos.qqtang",
    "地球生存":   "com.lintos.earth-survival",
    "迷你DayZ":   "com.lintos.minidayz",
    # 资源包（本地无源文件，登记 id 只为让报告如实）
    "仙剑奇侠传": "com.lintos.sdlpal",
    "缝合像素8":  "com.lintos.resource.font.fusion8",
    "缝合像素12": "com.lintos.resource.font.fusion12",
    "方舟像素12": "com.lintos.resource.font.ark12",
    "文泉驿点阵宋12": "com.lintos.resource.font.wqy12",
    "文泉驿点阵宋14": "com.lintos.resource.font.wqy14",
    "文泉驿点阵宋16": "com.lintos.resource.font.wqy16",
    "GNU Unifont16": "com.lintos.resource.font.unifont16",
    "秋叶圆体16":  "com.lintos.resource.font.qiuye16",
    "正格点黑16":  "com.lintos.resource.font.zhengge16",
    "长坂点宋16":  "com.lintos.resource.font.changban16",
}

HDR = re.compile(r"--@app\s+([a-z_]+)=([^\r\n]*)")

# 中文分类 → 稳定 ID（与 lpk_manifest.c 的 lpk_mf_cat_id 完全一致）
# 程序只认 ID；中文只是输入别名。写进 manifest 和索引的都是 ID。
CAT_ID = {
    "软件": "software", "程序": "software", "工具": "software",
    "游戏": "games", "学习": "learning", "运维": "operations",
    "系统": "system", "壁纸": "wallpaper",
}


def cat_id(c):
    return CAT_ID.get(c or "", "software")


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def lapp_header(text):
    return {m.group(1): m.group(2).strip() for m in HDR.finditer(text[:4096])}


def build_app_dir(dst, entry_src_text, hdr, app_id, entry_rel, extra_files=(), keep_existing=False):
    if os.path.isdir(dst) and not keep_existing:
        shutil.rmtree(dst)
    os.makedirs(os.path.join(dst, os.path.dirname(entry_rel) or "."), exist_ok=True)
    open(os.path.join(dst, entry_rel), "w", encoding="utf-8").write(entry_src_text)

    icon_rel = ""
    ic = hdr.get("icon")
    if ic:
        try:
            raw = base64.b64decode(ic)
            if len(raw) == 512:
                open(os.path.join(dst, "icon.bin"), "wb").write(raw)
                icon_rel = "icon.bin"
            else:
                print("   ⚠ icon 长度 %d ≠ 512，跳过图标" % len(raw))
        except Exception as e:
            print("   ⚠ icon 解码失败:", e)

    mf = {
        "schema_version": 1,
        "app_id": app_id,
        "name": hdr.get("name", os.path.basename(entry_rel)),
        "label": hdr.get("name", os.path.basename(entry_rel)),
        "description": hdr.get("desc", ""),
        "category": cat_id(hdr.get("cat")),
        "package_type": "app",
        "runtime": "lua",
        "entry": entry_rel,
        "version": hdr.get("version", "1"),
        "build": hdr.get("build", ""),
        "author": hdr.get("author", "LinTOS"),
        "icon": icon_rel,
        "dependencies": [],
        "permissions": ["network"] if hdr.get("need") == "wifi" else [],
        "data_policy": "preserve",
        "min_firmware": "2.0.0",
    }
    open(os.path.join(dst, "manifest.json"), "w", encoding="utf-8").write(
        json.dumps(mf, ensure_ascii=False, indent=2) + "\n")

    for rel, src in extra_files:
        p = os.path.join(dst, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        shutil.copyfile(src, p)
    return mf


def pack(dir_path, out_path):
    r = sh([sys.executable, PACK, "--app", dir_path, "-o", out_path,
            "--index", "/nonexistent.json"])
    if not os.path.isfile(out_path):
        return None, (r.stdout or "") + (r.stderr or "")
    raw = open(out_path, "rb").read()
    return {
        "size": len(raw),
        "crc32": format(zlib.crc32(raw) & 0xFFFFFFFF, "08x"),
    }, ""


def main():
    os.makedirs(WORK, exist_ok=True)
    os.makedirs(PACKAGES, exist_ok=True)

    idx = json.load(open(os.path.join(ROOT, "store", "index.json"), encoding="utf-8"))
    pkgs = idx["pkgs"]

    v2 = {"schema_version": 2, "mirrors": idx.get("mirrors", []), "packages": []}
    rows = []

    for pk in pkgs:
        name = pk.get("name", "")
        path = pk.get("path", "")
        typ = pk.get("type") or ("lua" if path.endswith(".lua") else "?")
        src = os.path.join(ROOT, "store", path)
        app_id = APP_ID.get(name)

        if not app_id:
            rows.append((name, "-", "-", "跳过：未登记 app_id"))
            continue
        if not os.path.isfile(src):
            rows.append((name, app_id, typ, "跳过：本地无源文件（%s，只在服务器上）" % path))
            continue

        dst = os.path.join(WORK, app_id)
        try:
            if typ == "lua":
                text = open(src, encoding="utf-8").read()
                hdr = lapp_header(text)
                mf = build_app_dir(dst, text, hdr, app_id, "app/entry.lua")
            elif typ == "asset" and src.endswith(".zip"):
                z = zipfile.ZipFile(src)
                luas = [n for n in z.namelist()
                        if n.lower().endswith(".lua") and "/" not in n]
                if not luas:
                    rows.append((name, app_id, "asset", "跳过：zip 内无顶层 .lua（纯素材包）"))
                    continue
                entry = luas[0]
                text = z.read(entry).decode("utf-8", "ignore")
                hdr = lapp_header(text)
                # 原样解到目录，保持包内相对路径（素材在 wuxia/… 下）
                if os.path.isdir(dst):
                    shutil.rmtree(dst)
                z.extractall(dst)
                # ⚠ 必须 keep_existing=True：否则 build_app_dir 会把刚解出来的 1.7 MB
                #   素材整目录删掉，只剩一个 .lua（实测包体从 1.86 MB 掉到 95 KB）。
                mf = build_app_dir(dst, text, hdr, app_id, entry, keep_existing=True)
            else:
                rows.append((name, app_id, typ, "跳过：暂不支持该类型"))
                continue
        except Exception as e:
            rows.append((name, app_id, typ, "失败：%s" % e))
            continue

        out = os.path.join(PACKAGES, app_id + ".lpk")
        info, err = pack(dst, out)
        if not info:
            rows.append((name, app_id, typ, "打包失败: %s" % err.strip().splitlines()[-1:]))
            continue

        v2["packages"].append({
            "app_id": app_id,
            "name": name,
            "category": mf["category"],
            "package_type": mf["package_type"],
            "version": mf["version"],
            "build": mf["build"],
            "size": info["size"],
            "crc32": info["crc32"],
            "sha256": "",            # 由 --verify 步骤补（见下）
            "path": "packages/%s.lpk" % app_id,
            "min_firmware": "2.0.0",
        })
        import hashlib
        v2["packages"][-1]["sha256"] = hashlib.sha256(open(out, "rb").read()).hexdigest()
        rows.append((name, app_id, typ, "✅ %d B  crc=%s" % (info["size"], info["crc32"])))

    outp = os.path.join(ROOT, "store", "index.v2.json")
    open(outp, "w", encoding="utf-8").write(json.dumps(v2, ensure_ascii=False, indent=1) + "\n")

    print("%-14s %-28s %-8s %s" % ("原包", "新 app_id", "类型", "结果"))
    for r in rows:
        print("%-14s %-28s %-8s %s" % r)
    print("\n生成 %d 个 .lpk → %s" % (len(v2["packages"]), PACKAGES))
    print("新索引 → %s（%d 条）" % (outp, len(v2["packages"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
