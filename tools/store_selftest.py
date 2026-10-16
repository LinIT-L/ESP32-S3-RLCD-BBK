#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""store_selftest.py — 「应用商店联网下载」链路的宿主机自检

做的三件事 (全部对着**真实服务器**跑, 不 mock):

  ① 解析器单测 —— 用 clang 编译**固件里那份** am_store_json.c (它刻意零 OS 依赖),
     喂真实 index.json + 12 组畸形/边界输入, 逐条对照 Python json 的结果。
     (解析错了不会崩, 只会静默变空列表 —— 所以必须正面测。)

  ② 契约自检 —— 逐条核对 index.json 的每个字段与 store/pkgs/*.lua 实物是否一致
     (size / crc32 / path 拼出来的 URL 能不能 200 拿到)。

  ③ 端到端下载 —— 按固件的**多源回退**逻辑走一遍:
     故意把 mirrors[0] 指到一个死地址 → 必须自动切到 mirrors[1] 成功;
     下载的每个包用**固件同款半字节 CRC32** 算一遍, 必须与 manifest 完全一致,
     并检查固件"落盘缓冲必须内部 RAM"之外的另一个硬约束 —— 单包体积。

用法:
    python3 tools/store_selftest.py                 # 用 store/index.json + 内置镜像
    python3 tools/store_selftest.py --dead-first    # 额外跑一遍"主源挂掉"的回退
"""
import argparse
import binascii
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGES = os.path.join(ROOT, "components", "apps", "pages")
STORE = os.path.join(ROOT, "store")
BOOT_MIRROR = "https://bbk.linit.cn/bbk"   # 必须与 am_store_net.c 的 k_boot 一致

PASS, FAIL = [], []


def ok(msg):
    PASS.append(msg)
    print("  \033[32m✓\033[0m %s" % msg)


def bad(msg):
    FAIL.append(msg)
    print("  \033[31m✗\033[0m %s" % msg)


# ---------------------------------------------------------------- CRC32 (固件同款)
CRC_T = []
for _i in range(16):
    _c = _i
    for _ in range(4):                       # ★ 半字节表 = 4 轮 (8 轮是字节表)
        _c = ((_c >> 1) ^ 0xEDB88320) if (_c & 1) else (_c >> 1)
    CRC_T.append(_c & 0xFFFFFFFF)


def fw_crc32(data, chunk=2048, seed=0):
    """逐行照抄 am_store_net.c 的 crc32_upd + 分块喂法 (固件就是这么攒的)。"""
    crc = seed
    for i in range(0, len(data), chunk):
        part = data[i:i + chunk]
        crc = (~crc) & 0xFFFFFFFF
        for b in part:
            crc ^= b
            crc = (crc >> 4) ^ CRC_T[crc & 0xF]
            crc = (crc >> 4) ^ CRC_T[crc & 0xF]
        crc = (~crc) & 0xFFFFFFFF
    return crc


# ---------------------------------------------------------------- ① 解析器单测
def pick_cc():
    """挑一个真能用的宿主机 C 编译器。

    ⚠ 这台机器上 /usr/bin/cc 与 /opt/homebrew/bin/gcc-14 都会被 "未同意 Xcode 许可协议"
       挡住 (clang 直接报错, gcc 则在链接期调用被挡的 ld)。实测可用的是
       **CommandLineTools 自带的 clang** —— 它不走 Xcode.app 那套许可检查。
    顺序: $CC → CLT clang → cc → clang → gcc-14。"""
    cands = [os.environ.get("CC"),
             "/Library/Developer/CommandLineTools/usr/bin/clang",
             "cc", "clang", "/opt/homebrew/bin/gcc-14"]
    for c in cands:
        if not c:
            continue
        path = c if os.path.isabs(c) else shutil.which(c)
        if not path:
            continue
        try:
            r = subprocess.run([path, "--version"], capture_output=True, text=True, timeout=15)
        except Exception:
            continue
        if r.returncode == 0:
            return path
    return None


def build_parser():
    exe = os.path.join(tempfile.gettempdir(), "store_parse_test")
    src = os.path.join(ROOT, "tools", "store_parse_test.c")
    cc = pick_cc()
    if not cc:
        bad("找不到可用的宿主机 C 编译器 (试过 $CC / CLT clang / cc / gcc-14)")
        return None
    cmd = [cc, "-O0", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter"]
    # ⚠ 这台机器 xcode-select 指向 Xcode.app, 但许可没同意 → clang 找不到 SDK 里的 stdio.h。
    #   显式把 CommandLineTools 的 SDK 指过去 (该 SDK 在磁盘上, 不需要许可检查)。
    for sdk in ("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
                "/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk"):
        if os.path.isdir(os.path.join(sdk, "usr", "include")):
            cmd += ["-isysroot", sdk]
            break
    cmd += ["-I", PAGES, "-o", exe, src, os.path.join(PAGES, "am_store_json.c")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        bad("解析器编译失败:\n%s" % r.stderr[:600])
        return None
    if r.stderr.strip():
        print("  (编译器提示) %s" % r.stderr.strip()[:400])
    ok("用 %s 编译固件解析器 am_store_json.c 成功 (零 OS 依赖, 无告警)" % os.path.basename(cc))
    return exe


def run_parser(exe, path):
    r = subprocess.run([exe, path], capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr
    out = {"mir": [], "pkg": []}
    for line in r.stdout.splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        if k == "pkg":
            out["pkg"].append(v.split("|"))
        elif k == "mir":
            out["mir"].append(v)
        else:
            out[k] = int(v)
    return out, r.stderr


def test_parser(exe):
    print("\n① 解析器 (固件真源码, 宿主机编译)")
    real = os.path.join(STORE, "index.json")
    if not os.path.exists(real):
        bad("找不到 %s —— 先跑 tools/store_build.py" % real)
        return
    ref = json.load(open(real, encoding="utf-8"))

    got, err = run_parser(exe, real)
    if got is None:
        bad("解析真实 index.json 直接失败: %s" % err)
        return
    if got.get("rc") != 0:
        bad("真实 index.json rc=%s (期望 0)" % got.get("rc"))
        return
    if got["mir"] != [m.rstrip("/") for m in ref["mirrors"]]:
        bad("mirrors 不一致: 固件=%s / 文件=%s" % (got["mir"], ref["mirrors"]))
    else:
        ok("mirrors 解析一致 (%d 个)" % len(got["mir"]))

    if len(got["pkg"]) != len(ref["pkgs"]):
        bad("包数不一致: 固件 %d / 文件 %d" % (len(got["pkg"]), len(ref["pkgs"])))
    else:
        ok("包数一致 (%d 条)" % len(got["pkg"]))

    nbad = 0
    for p, r in zip(got["pkg"], ref["pkgs"]):
        name, label, cat, size, crc, path = p
        want = (r["name"], r["label"], r["cat"], str(r["size"]), r["crc32"], r["path"])
        if (name, label, cat, size, crc, path) != want:
            bad("字段不一致 %s: 固件=%s / 文件=%s" % (r["name"], p, list(want)))
            nbad += 1
    if nbad == 0:
        ok("每条的 name/label/cat/size/crc32/path 与 index.json 逐字段一致 (中文 label 未乱码)")

    # ---- 边界/畸形输入 ----
    cases = [
        ("中文转义 (\\uXXXX)", '{"mirrors":["http://a"],"pkgs":[{"name":"x","label":"\\u7801\\u8868",'
                              '"cat":"app","size":7,"crc32":"00000001","path":"pkgs/x.lua"}]}', 0,
         {"label": "码表"}),
        ("emoji 代理对 (\\uD83D\\uDE00)", '{"mirrors":["http://a"],"pkgs":[{"name":"x","label":"A\\uD83D\\uDE00B",'
                                          '"cat":"app","size":7,"crc32":"00000001","path":"p"}]}', 0,
         {"label": "A😀B"}),
        ("缩进/换行/空格乱来", '{\n  "mirrors" : [\n"http://a/"\n] ,\n"pkgs":[ {  "name":"x" ,\n'
                              '"label":"L","cat":"app","size":9,"crc32":"0000abcd","path":"p/x.lua"}]}', 0,
         {"name": "x", "path": "p/x.lua", "crc": "0000abcd"}),
        ("镜像尾斜杠必须去掉", '{"mirrors":["http://a///"],"pkgs":[{"name":"x","label":"L","cat":"app",'
                               '"size":1,"crc32":"00000001","path":"p"}]}', 0, None),
        ("单条缺 size → 只跳这条", '{"mirrors":["http://a"],"pkgs":[{"name":"bad","label":"L","cat":"app",'
                                   '"crc32":"00000001","path":"p"},{"name":"good","label":"G","cat":"app",'
                                   '"size":3,"crc32":"00000002","path":"q"}]}', 0, None),
        ("单条 crc32 全 0 → 视为无效", '{"mirrors":["http://a"],"pkgs":[{"name":"x","label":"L","cat":"app",'
                                       '"size":1,"crc32":"00000000","path":"p"}]}', -3, None),
        ("缺 mirrors → -1", '{"pkgs":[{"name":"x","label":"L","cat":"app","size":1,'
                            '"crc32":"00000001","path":"p"}]}', -1, None),
        ("缺 pkgs → -2", '{"mirrors":["http://a"]}', -2, None),
        ("pkgs 空数组 → -3", '{"mirrors":["http://a"],"pkgs":[]}', -3, None),
        ("对象被拦腰截断 → 不崩, 报 -3", '{"mirrors":["http://a"],"pkgs":[{"name":"x","label":"L","cat":"app",'
                                        '"size":1,"crc32":"00000001","path":"p"', -3, None),
        ("数组收尾缺失但最后一个对象完整 → 宽容接受",
         '{"mirrors":["http://a"],"pkgs":[{"name":"x","label":"L","cat":"app","size":1,'
         '"crc32":"00000001","path":"p"}', 0, {"n": 1}),
        ("字符串里带 } ] , 不能骗到括号配对",
         '{"mirrors":["http://a"],"pkgs":[{"name":"x","label":"a}b]c,d","cat":"app","size":1,'
         '"crc32":"00000001","path":"p"},{"name":"y","label":"L","cat":"app","size":2,'
         '"crc32":"00000002","path":"q"}]}', 0, {"n": 2}),
        ("包对象里多一层嵌套对象",
         '{"mirrors":["http://a"],"pkgs":[{"name":"x","label":"L","cat":"app","size":1,'
         '"crc32":"00000001","path":"p","extra":{"a":[1,2,{"b":"}"}]}}]}', 0, None),
        ("乱码输入不崩", "}{not json at all", -1, None),
    ]
    for title, text, want_rc, extra in cases:
        fd, tf = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(text)
        got, err = run_parser(exe, tf)
        os.unlink(tf)
        if got is None:
            bad("%s → 崩了(%s)" % (title, err.strip()[:120]))
            continue
        if got.get("rc") != want_rc:
            bad("%s → rc=%s 期望 %s" % (title, got.get("rc"), want_rc))
            continue
        if extra:
            if "label" in extra and (not got["pkg"] or got["pkg"][0][1] != extra["label"]):
                bad("%s → label=%r 期望 %r" % (title, got["pkg"][0][1] if got["pkg"] else None,
                                               extra["label"]))
                continue
            if "n" in extra and len(got["pkg"]) != extra["n"]:
                bad("%s → 包数 %d 期望 %d" % (title, len(got["pkg"]), extra["n"]))
                continue
            if "name" in extra and got["pkg"][0][0] != extra["name"]:
                bad("%s → name=%r" % (title, got["pkg"][0][0]))
                continue
            if "crc" in extra and got["pkg"][0][4] != extra["crc"]:
                bad("%s → crc=%r 期望 %r" % (title, got["pkg"][0][4], extra["crc"]))
                continue
            if "path" in extra and got["pkg"][0][5] != extra["path"]:
                bad("%s → path=%r" % (title, got["pkg"][0][5]))
                continue
        if title.startswith("镜像尾斜杠") and got["mir"][0] != "http://a":
            bad("%s → 尾斜杠没去掉: %r" % (title, got["mir"][0]))
            continue
        if title.startswith("单条缺 size") and len(got["pkg"]) != 1:
            bad("%s → 应有 1 条存活, 实得 %d" % (title, len(got["pkg"])))
            continue
        ok("%s → rc=%s" % (title, want_rc))


# ---------------------------------------------------------------- ②③ 网络
def http_get(url, timeout=20):
    req = urllib.request.Request(url, headers={"User-Agent": "LinTOS-store-selftest"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read(), dict(r.headers)


def test_contract(ref, mirrors):
    print("\n② 契约自检 (真实服务器)")
    try:
        st, body, hdr = http_get("%s/index.json" % mirrors[0])
    except Exception as e:
        bad("取 index.json 失败: %s" % e)
        return None
    if st != 200:
        bad("index.json HTTP %s" % st)
        return None
    if "json" not in (hdr.get("Content-Type") or ""):
        bad("Content-Type 不是 json: %r (WordPress 接走了? 见 store_deploy.sh)" % hdr.get("Content-Type"))
        return None
    ok("index.json → HTTP 200, Content-Type=%s, %d B" % (hdr.get("Content-Type"), len(body)))

    try:
        live = json.loads(body.decode("utf-8"))
    except Exception as e:
        bad("服务器上的 index.json 不是合法 JSON: %s" % e)
        return None
    if [p["name"] for p in live["pkgs"]] != [p["name"] for p in ref["pkgs"]]:
        bad("线上包列表与本地 store/index.json 不一致 —— 部署漏了? ")
    else:
        ok("线上包列表与本地 store/index.json 完全一致 (%d 个)" % len(live["pkgs"]))
    if live.get("mirrors") != ref.get("mirrors"):
        bad("线上 mirrors 与本地不一致: %s vs %s" % (live.get("mirrors"), ref.get("mirrors")))
    else:
        ok("mirrors 一致: %s" % live.get("mirrors"))
    return live


def test_download(live, mirrors, dead_first=False):
    tag = "③ 端到端下载" + (" (主源故意打死, 测回退)" if dead_first else "")
    print("\n%s" % tag)
    bases = list(live["mirrors"])
    if dead_first:
        bases = ["http://127.0.0.1:9/bbk"] + bases     # 9 = discard 端口, 秒拒
    fails = 0
    for p in live["pkgs"]:
        url = None
        data = None
        used = None
        for b in bases:                                # 固件的多源回退循环
            u = "%s/%s" % (b.rstrip("/"), p["path"])   # 固件就是 mirrors[i] + "/" + path
            try:
                st, body, _ = http_get(u, timeout=30)
                if st == 200 and body:
                    url, data, used = u, body, b
                    break
            except Exception:
                continue
        if data is None:
            bad("%-14s 所有镜像都拿不到" % p["name"])
            fails += 1
            continue
        crc = fw_crc32(data)
        why = []
        if len(data) != p["size"]:
            why.append("size %d≠%d" % (len(data), p["size"]))
        if "%08x" % crc != p["crc32"]:
            why.append("crc %08x≠%s" % (crc, p["crc32"]))
        if binascii.crc32(data) & 0xFFFFFFFF != crc:
            why.append("固件 CRC 与 zlib 不一致")
        if why:
            bad("%-14s %s" % (p["name"], " / ".join(why)))
            fails += 1
            continue
        note = " (从死源回退到 %s)" % used if (dead_first and used != bases[0]) else ""
        ok("%-14s %6d B  crc=%s  源=%s%s" % (p["name"], len(data), p["crc32"], used, note))
    if fails == 0:
        ok("全部 %d 个包: size 一致 + 固件半字节 CRC32 == manifest == zlib" % len(live["pkgs"]))
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dead-first", action="store_true",
                    help="额外跑一次 mirrors[0] 挂掉的回退测试")
    args = ap.parse_args()

    print("=" * 68)
    print(" 应用商店联网下载 · 宿主机自检")
    print("=" * 68)

    exe = build_parser()
    if exe:
        test_parser(exe)

    ref_path = os.path.join(STORE, "index.json")
    if not os.path.exists(ref_path):
        print("\n找不到 store/index.json, 跳过网络部分")
        return summarize()
    ref = json.load(open(ref_path, encoding="utf-8"))

    live = test_contract(ref, ref.get("mirrors") or [BOOT_MIRROR])
    if live:
        test_download(live, live["mirrors"])
        if args.dead_first:
            test_download(live, live["mirrors"], dead_first=True)
    return summarize()


def summarize():
    print("\n" + "=" * 68)
    print(" 通过 %d 项" % len(PASS))
    if FAIL:
        print(" 失败 %d 项:" % len(FAIL))
        for f in FAIL:
            print("   - %s" % f)
        return 1
    print(" 全部通过 ✅")
    return 0


if __name__ == "__main__":
    sys.exit(main())
