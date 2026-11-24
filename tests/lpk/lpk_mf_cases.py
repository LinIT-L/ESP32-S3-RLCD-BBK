#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
lpk_mf_cases.py — 应用包（manifest.json）校验用例驱动

做的事只有三件：
  ① 用 tools/pack_lpk.py 现造一个合规应用包（manifest + 标准 512B 图标 + 入口）
  ② 在每个坏包上只改**一处**，其余保持合法 —— 这样失败时能确定是哪条规则拦住的
  ③ 逐个跑 tests/lpk/lpk_mf_test（一用例一进程），统计 PASS/FAIL

退出码 = 失败用例数（0 表示全过）。
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.environ.get("WORK", "/tmp/lpk_mf_test")
CC = os.environ.get("CC", "/Library/Developer/CommandLineTools/usr/bin/clang")
SDK = os.environ.get("SDK", "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")

PACK = os.path.join(ROOT, "tools", "pack_lpk.py")
TESTC = os.path.join(ROOT, "tests", "lpk", "lpk_mf_test.c")
LPK_SRC = [os.path.join(ROOT, "components/services/lpk", f) for f in ("lpk1.c", "lpk_manifest.c")]
INC = os.path.join(ROOT, "components/services/lpk/include")

BASE_MF = {
    "schema_version": 1,
    "app_id": "com.lintos.demo",
    "name": "Demo",
    "label": "示例应用",
    "description": "仅用于校验器的样例包",
    "category": "软件",
    "package_type": "app",
    "runtime": "lua",
    "entry": "app/entry.lua",
    "version": "1.0.0",
    "build": "202611011000",
    "author": "LinTOS",
    "icon": "icon.bin",
    "dependencies": [],
    "permissions": [],
    "data_policy": "preserve",
    "min_firmware": "2.0.0",
}


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def make_app(dst, mf, icon_bytes=512, entry=True, extra_files=()):
    """造一个应用目录；icon_bytes=None 表示不放图标"""
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(os.path.join(dst, "app"), exist_ok=True)
    if entry:
        open(os.path.join(dst, "app", "entry.lua"), "w", encoding="utf-8").write(
            "-- demo entry\nprint('hi')\n")
    if icon_bytes is not None:
        open(os.path.join(dst, "icon.bin"), "wb").write(b"\x00" * icon_bytes)
    for rel, data in extra_files:
        p = os.path.join(dst, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data)
    if mf is not None:
        open(os.path.join(dst, "manifest.json"), "w", encoding="utf-8").write(
            json.dumps(mf, ensure_ascii=False, indent=2))
    return dst


def pack(app_dir, out):
    r = sh([sys.executable, PACK, "--app", app_dir, "-o", out, "--index", "/nonexistent.json"])
    if not os.path.isfile(out):
        raise SystemExit("打包失败 %s\n%s\n%s" % (app_dir, r.stdout, r.stderr))
    return out


def pack_raw(src_dir, out):
    """绕过 --app 校验直接打包（负向用例专用）。
    ⚠ 必须 --no-default-exclude，否则 manifest.json 会被默认排除表丢掉，
      变成"缺 manifest"用例，测不到真正想测的那条规则。"""
    r = sh([sys.executable, PACK, "--assets", src_dir, "-o", out, "--no-default-exclude"])
    if not os.path.isfile(out):
        raise SystemExit("打包失败 %s\n%s\n%s" % (src_dir, r.stdout, r.stderr))
    return out


def main():
    os.makedirs(WORK, exist_ok=True)
    exe = os.path.join(WORK, "lpk_mf_test")
    r = sh([CC, "-isysroot", SDK, "-O2", "-std=c11", "-Wall", "-Wextra",
            "-I", INC, TESTC] + LPK_SRC + ["-o", exe])
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        raise SystemExit("测试程序编译失败")

    cases = []          # (名字, 包路径, 期望 rc, 期望 app_id, 期望 type)

    # ① 合规包
    d = make_app(os.path.join(WORK, "ok"), dict(BASE_MF))
    cases.append(("合规应用包", pack(d, os.path.join(WORK, "ok.lpk")), 0,
                  "com.lintos.demo", "app"))

    # ② 缺 manifest（纯资源目录）
    d = os.path.join(WORK, "nomanifest")
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "a.bin"), "wb").write(b"x")
    cases.append(("缺 manifest.json", pack_raw(d, os.path.join(WORK, "no_mf.lpk")), -3, None, None))

    # ③ app_id 非法（大写 + 无命名空间）
    mf = dict(BASE_MF); mf["app_id"] = "Recorder"
    d = make_app(os.path.join(WORK, "bad_id"), mf)
    cases.append(("app_id 非法", pack_raw(d, os.path.join(WORK, "bad_id.lpk")), -6, None, None))

    # ④ 入口不在包里（manifest 声明了，但文件没打进去）
    mf = dict(BASE_MF); mf["entry"] = "app/missing.lua"
    d = make_app(os.path.join(WORK, "no_entry"), mf, entry=False)
    cases.append(("entry 不在包里", pack_raw(d, os.path.join(WORK, "no_entry.lpk")), -8, None, None))

    # ⑤ 图标长度不是 512
    d = make_app(os.path.join(WORK, "bad_icon"), dict(BASE_MF), icon_bytes=256)
    cases.append(("icon 长度 256", pack_raw(d, os.path.join(WORK, "bad_icon.lpk")), -10, None, None))

    # ⑥ 图标声明了但没放
    mf = dict(BASE_MF); mf["icon"] = "icon.bin"
    d = make_app(os.path.join(WORK, "no_icon"), mf, icon_bytes=None)
    cases.append(("声明了 icon 但包里没有", pack_raw(d, os.path.join(WORK, "no_icon.lpk")), -9, None, None))

    # ⑦ package_type 非法
    mf = dict(BASE_MF); mf["package_type"] = "plugin"
    d = make_app(os.path.join(WORK, "bad_type"), mf)
    cases.append(("package_type=plugin", pack_raw(d, os.path.join(WORK, "bad_type.lpk")), -6, None, None))

    # ⑧ schema_version 缺失（=0）
    mf = dict(BASE_MF); mf.pop("schema_version")
    d = make_app(os.path.join(WORK, "no_schema"), mf)
    cases.append(("schema_version 缺失", pack_raw(d, os.path.join(WORK, "no_schema.lpk")), -6, None, None))

    # ⑨ 资源包（不可启动，但必须能被识别）
    res = {
        "schema_version": 1, "app_id": "com.lintos.resource.demo-font",
        "name": "DemoFont", "label": "示例字体", "description": "字体资源",
        "package_type": "resource", "resource_type": "font",
        "version": "1.0.0", "build": "202611011000", "author": "LinTOS",
        "install_target": "resources/fonts",
    }
    d = make_app(os.path.join(WORK, "res"), res, icon_bytes=None, entry=False,
                 extra_files=[("resources/fonts/a.bin", b"f" * 32)])
    cases.append(("资源包（resource）", pack(d, os.path.join(WORK, "res.lpk")), 0,
                  "com.lintos.resource.demo-font", "resource"))

    # ⑩ 入口路径穿越
    mf = dict(BASE_MF); mf["entry"] = "../escape.lua"
    d = make_app(os.path.join(WORK, "trav"), mf)
    cases.append(("entry 路径穿越", pack_raw(d, os.path.join(WORK, "trav.lpk")), -7, None, None))

    fails = 0
    print("=========== manifest.json 校验用例 ===========")
    for name, path, want, aid, typ in cases:
        args = [exe, path, str(want)]
        if aid:
            args.append(aid)
        if typ:
            args.append(typ)
        r = sh(args)
        line = (r.stdout or "").rstrip()
        ok = (r.returncode == 0)
        print(line)
        if not ok:
            fails += 1
            print("      ✗ %s" % name)

    total = len(cases)
    print("---------- %d 项：%d PASS / %d FAIL ----------" % (total, total - fails, fails))
    return fails


if __name__ == "__main__":
    sys.exit(main())
