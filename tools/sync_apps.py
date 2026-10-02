#!/usr/bin/env python3
"""sync_apps.py — 桌面源码 → 应用安装区(apps 分区) 一键同步 + LAPP 打包 + 生成 SPIFFS 镜像

背景: Lua 应用的开发流程是两段式的 ——
  ① 源码在桌面 (便于本机模拟/编辑):   ~/Desktop/LinTOS_Lua应用/src/*.lua
  ② 上线要进应用安装区 (设备开机就能看到): SPIIFS(apps 分区) 内 /apps/{app,ops}/
     └ 挂载点 /store, 所以设备上的路径是 /store/apps/{app,ops}/xxx.lua
       (am_lua.c 就是按 /store/apps/{engine|app|ops} 扫的)

本脚本把 ①② 串起来, 并强制走完三道闸门, 避免手工漏步:
  语法+契约闸门 (lua_check) → LAPP 打包 (pack_app) → 生成 apps_l.bin (spiffsgen)

用法:
    python3 tools/sync_apps.py                # 同步全部 + 打包 + 出镜像
    python3 tools/sync_apps.py pomodoro diag  # 只处理指定应用
    python3 tools/sync_apps.py --check-only   # 只跑闸门与打包校验, 不出镜像
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.expanduser("~/Desktop/LinTOS_Lua应用/src")
DEST_ROOT = os.path.join(ROOT, "apps", "lua", "apps")     # SPIFFS 根 (含 apps/ 一层)
SPIFFS_BIN = os.path.join(ROOT, "build", "apps_l.bin")


def apps_part():
    """apps 分区的 (offset, size) **从 partitions.csv 读** (单一真相源)。

    历史上这里硬编码 0x5A0000, 后来分区改成 5MB 而脚本没改 →
    spiffsgen 会按错的大小生成镜像, dd 进去后 SPIFFS 挂载失败。"""
    csv_path = os.path.join(ROOT, "partitions.csv")
    for ln in open(csv_path, encoding="utf-8"):
        ln = ln.split("#")[0].strip()
        if not ln:
            continue
        f = [x.strip() for x in ln.split(",")]
        if len(f) >= 5 and f[0] == "apps":
            return int(f[3], 0), int(f[4], 0)
    raise SystemExit("partitions.csv 里找不到 apps 分区")

# name → (分类目录, 显示名, 分类标签, 简介, 固件图标枚举)
# ⚠ 第三列 (分类标签) 是**真分类来源** —— 打进 LAPP 头部, 由 am_lua.c 读取后决定
#   应用管家里的子分类 (程序/软件→软件, 系统/工具→系统, 运维→应用分类里显示为"壁纸"之外的那类)。
#   规则: **放在哪个目录 = 左侧哪个分类** (app/ → 应用, ops/ → 运维);
#         标签 = 该分类里的子分类。所以 ops/ 里的都写"运维"。
#
# 🚫 2026-09-28 linit 决定: 这 10 个**改为以内置 C 版为准**, 外置 Lua 版暂不发行
#    (真机上 Lua 版有问题: 无线探测进入即重启 —— on_enter 里 app.wifi.init()+scan() 开射频
#     触发掉电复位; 另外外置包与内置两份并存、商店与内置重复)。
#    对应动作: apps/lua/apps 下的包已移出到 backup_lua_apps_20260928/;
#    C 版已在 components/apps/apps_registry.c 的 s_builtin_apps[] 登记 (工厂内置, 商店不显示)。
#    ⚠ 要恢复发行 Lua 版: 取消下面 MANIFEST 的注释 + 重跑本脚本 + 撤掉那 10 条内置登记。
MANIFEST = {}   # 暂空 (数据保留在下方注释里, 恢复时取消注释即可)
# MANIFEST = {
#     "calc":      ("app", "计算器",   "程序", "表达式求值, 支持括号与四则运算",             "ICON_CALC"),
#     "morse":     ("app", "摩斯电码", "程序", "文本与摩斯码互转, 可发声播放",               "ICON_MORSE"),
#     "pomodoro":  ("app", "番茄钟",   "程序", "圆环进度专注计时, 工作/休息自动切换",       "ICON_POMODORO"),
#     "wb":        ("app", "白板",     "程序", "铅笔/马克/橡皮/直线/矩形/椭圆 + 撤销重做", "ICON_WHITEBOARD"),
#     "wifiprobe": ("ops", "无线探测", "运维", "雷达图 + 信道热力图 + 探针嗅探 三视图",     "ICON_WIFIPROBE"),
#     "netdect":   ("ops", "网络检测", "运维", "六步网络体检: 本机/公网/主机/端口/口令/报告", "ICON_NETDECT"),
#     "diag":      ("ops", "电脑诊断", "运维", "按现象追问, 概率排序给出故障配件与排除方案", "ICON_DIAG"),
#     "logdiag":   ("ops", "日志诊断", "运维", "插入电脑自动注入命令链, 收集系统与错误日志", "ICON_LOGDIAG"),
#     "term":      ("ops", "终端",     "运维", "命令行练习: 4 套系统模拟 + 52 键键盘",     "ICON_TERMINAL"),
#     "winassist": ("ops", "运维助手", "运维", "远程键鼠操控 Windows 常用面板与诊断命令",   "ICON_WINASSIST"),
# }

PY = sys.executable
IDF_SPIFFSGEN = os.path.expanduser(
    "~/esp/esp-idf-v55/components/spiffs/spiffsgen.py")


def run(cmd, quiet=True):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print("  ✗ 失败: %s" % " ".join(cmd))
        print((r.stdout or "")[-1200:])
        print((r.stderr or "")[-1200:])
        raise SystemExit(1)
    if not quiet:
        sys.stdout.write(r.stdout if r.stdout.endswith("\n") else r.stdout + "\n")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("apps", nargs="*", help="只处理这些应用 (默认全部)")
    ap.add_argument("--check-only", action="store_true")
    args = ap.parse_args()

    names = args.apps or list(MANIFEST)
    for n in names:
        if n not in MANIFEST:
            raise SystemExit("MANIFEST 里没有这个应用: %s (已有: %s)"
                             % (n, ", ".join(MANIFEST)))

    print("① 桌面源码 → 应用安装区")
    for n in names:
        cat = MANIFEST[n][0]
        src = os.path.join(SRC_DIR, n + ".lua")
        if not os.path.exists(src):
            raise SystemExit("桌面没有源码: %s" % src)
        dst_dir = os.path.join(DEST_ROOT, cat)
        os.makedirs(dst_dir, exist_ok=True)
        shutil.copy(src, os.path.join(dst_dir, n + ".lua"))
        print("   %-11s → apps/lua/apps/%s/%s.lua" % (n, cat, n))

    print("② 语法 + 契约闸门")
    files = [os.path.join(DEST_ROOT, MANIFEST[n][0], n + ".lua") for n in names]
    run([PY, "tools/lua_check.py"] + files, quiet=False)

    print("③ LAPP 打包 (图标取自固件 icons_main.inc, 与主菜单同源)")
    for n in names:
        cat, label, tag, desc, icon = MANIFEST[n]
        f = os.path.join(DEST_ROOT, cat, n + ".lua")
        run([PY, "tools/pack_app.py", "pack", f, "--name", label, "--cat", tag,
             "--desc", desc, "--icon-from-main", icon])
        print("   ✓ %-11s %s" % (n, label))

    if args.check_only:
        print("\n[--check-only] 未生成镜像")
        return

    print("④ 生成 apps 分区 SPIFFS 镜像")
    if not os.path.exists(IDF_SPIFFSGEN):
        raise SystemExit("找不到 spiffsgen.py: %s" % IDF_SPIFFSGEN)
    sz = apps_part()[1]
    print("   apps 分区大小 %.2f MB (读自 partitions.csv)" % (sz / 1048576))
    run([PY, IDF_SPIFFSGEN, "--page-size", "256", "--block-size", "4096",
         "--meta-len", "4", "--obj-name-len", "64", "--use-magic",
         "0x%X" % sz, os.path.join(ROOT, "apps", "lua"), SPIFFS_BIN],
        quiet=False)
    print("   ✓ build/apps_l.bin (%.2f MB)" % (os.path.getsize(SPIFFS_BIN) / 1048576))
    off = apps_part()[0]
    print("\n完成。烧录时由 merge_flash.sh 把该镜像写到 0x%X (apps 分区, 地址同样读自 partitions.csv)。" % off)


if __name__ == "__main__":
    main()
