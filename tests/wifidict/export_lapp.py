#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 wifidict 应用导出成**裸 .lua（LAPP 格式）**单文件

同一份 app/entry.lua，两种分发形态：
  · LPK  : pack_lpk.py --app  → dist/lpk/wifidict.lpk（manifest + icon.bin + 安装目录）
  · LAPP : 本脚本             → dist/Wi-Fi字典工具.lua（头部 --@app + 内嵌 base64 图标）

LAPP 头部规则以 components/services/apppack/apppack.c 的 apppack_parse() 为准：
  · 必须含 `--@app ` 开头的行，否则不算正式应用（应用中心直接跳过）
  · 键名：name / cat / desc / ver|version / build / author / date / need / icon
  · icon = 512 B（64x64 1bpp）位图的标准 base64
  · 只扫文件前 4096 B，且遇到首字符不是 '-' 也不是空行就停止

注意：设备端 `scan_lua_dir()` 对裸 .lua 的**显示名 = 文件名去扩展名**，
      `--@app name=` 会被忽略 —— 所以输出文件名就是应用名，改名前先想清楚。

用法: python3 tests/wifidict/export_lapp.py [输出路径]
"""
import base64
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))          # 仓库根
APP_DIR = os.path.join(ROOT, "apps", "lua", "lpk", "wifidict")
APP = os.path.join(APP_DIR, "app", "entry.lua")
ICON = os.path.join(APP_DIR, "icon.bin")
MANIFEST = os.path.join(APP_DIR, "manifest.json")

ICON_BYTES = 64 * 64 // 8          # 512，与 APPPACK_ICON_BYTES 一致
HEAD_SCAN = 4096                   # apppack_parse 的头扫描上限

# 裸 .lua 的显示名取自文件名，这里定死
OUT_NAME = "Wi-Fi字典工具.lua"
OUT_DEFAULT = os.path.join(ROOT, "dist", OUT_NAME)


def one_line(s):
    """头字段必须单行：换行 / 制表符压成空格，'=' 前不许出现 ' key=' 误判。"""
    return " ".join(str(s).replace("\t", " ").split())


def main():
    out_path = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else OUT_DEFAULT

    with open(MANIFEST, "r", encoding="utf-8") as f:
        mf = json.load(f)
    icon = open(ICON, "rb").read()
    if len(icon) != ICON_BYTES:
        print("icon.bin 尺寸错误: %d B (期望 %d B)" % (len(icon), ICON_BYTES))
        return 2
    body = open(APP, "rb").read()

    b64 = base64.b64encode(icon).decode("ascii")
    head = [
        "--@lapp v1",
        "--@app cat=" + one_line(mf.get("category", "")),
        "--@app desc=" + one_line(mf.get("description", "")),
        "--@app ver=" + one_line(mf.get("version", "")),
        "--@app build=" + one_line(mf.get("build", "")),
        "--@app author=" + one_line(mf.get("author", "")),
        "--@app icon=" + b64,
        "",
        "",
    ]
    head_bytes = ("\n".join(head)).encode("utf-8")

    # 头必须在扫描窗口内（manifest 字段很短，512B 图标 base64 约 684 字符）
    if len(head_bytes) >= HEAD_SCAN:
        print("头部 %d B 超出扫描窗口 %d B" % (len(head_bytes), HEAD_SCAN))
        return 2

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(head_bytes)
        f.write(body)

    print("输出: %s" % out_path)
    print("  头部 %d B / 正文 %d B / 合计 %d B" % (len(head_bytes), len(body), len(head_bytes) + len(body)))
    print("  应用名(文件名): %s" % os.path.basename(out_path)[:-4])
    print("  cat=%s ver=%s build=%s" % (mf.get("category"), mf.get("version"), mf.get("build")))
    print("  icon=%d B -> base64 %d 字符" % (len(icon), len(b64)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
