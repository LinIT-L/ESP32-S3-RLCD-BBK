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
SRC_DIR = os.path.expanduser("~/Desktop/LinTOS/src")   # 2026-10-15 linit 桌面重构: LinTOS_Lua应用 → LinTOS
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
# ★★★ 2026-10-24 linit 定案: **固件不内置任何 Lua 应用**, 一律「应用中心 → 商店」联网下载。
#   出厂 /store 里只剩 apps/_ui.lua (运行时 UI 库, 应用 require 它)。
#   BUILD_INTERNAL=False 时本脚本不复制、不打包、不预装, 只生成空的 apps 分区镜像。
#   要构建期调试(把 MANIFEST 里的应用塞回固件): 改成 True 再跑。
#   ⚠ 商店包不在这里生成 —— 用 tools/store_build.py (它直接读 ~/Desktop/LinTOS/src)。
BUILD_INTERNAL = False

MANIFEST = {
    # BIOS 装机模拟器（掌机版）: 纯黑白 1bpp，7 家品牌真实菜单结构 + 8 个装机任务。
    # 数据与电脑版 apps/biossim/biossim.html 同源（tools/biossim_gen_lua.py 生成数据段）。
    # 源码: ~/Desktop/LinTOS_Lua应用/src/biossim.lua
    # 图标: 2026-10-15 linit 要求换独立图标 (原借 ICON_DIAG=17 电脑诊断) →
    #   ICON_BIOSSIM(=35, 当天追加进 icons_main.inc, PNG=完美图标/应用管家/运维/BIOS模拟器.png)
    "biossim": ("ops", "BIOS模拟器", "运维",
                "7 家品牌 BIOS 装机模拟: 装 Win11/Linux 要改哪几项, 先在掌机上练", "ICON_BIOSSIM"),
    # 附近航班雷达 (掌机版): 数据源 adsb.fi 开放接口 (免 key) → 极坐标雷达 + 最近航班列表
    # + 航班详情卡。源码里带 `--@app need=wifi`: 打开时若未联网, 先弹窗走系统"连接WiFi",
    # 连上再进 (门禁实现在 components/apps/pages/am_lua.c 的 wl_open()/wl_gate_tick())。
    # 单文件小游戏示例: 精灵生成 / 按住移动 / 帧计时 / 存档 / 音效 全走桥 API。
    # 放商店 = 验证"下载安装就能跑"这条链路; 同时双写预装副本便于直接启动。
    # ★ QQ堂·冒险 (掌机版): 泡泡手感 + 无限分块地图 + 背包/技能成长。
    # 计划书见 docs/QQ堂冒险-计划书.md (3 遍修订定稿)。参考 qqtang-web 的瓦片码/泡泡/Server 门面。
    # ★ 《英雄坛说》掌机版 (对标 gmud.online): 全屏 400×300, 原版整图地图 + 原版精灵 + 原版数据。
    #   数据由 tools/wuxia_gen_data.py 从该站 4 个 JSON 生成 → /store/wuxia_data.lua
    #   素材由 tools/wuxia_pack.py / wuxia_world_gen.py 打包 → /store/wuxia/
    # ★ 人生指南: 676 条高性价比建议 (源自 zectrix-life-guide, Unlicense 公有领域)
    #   数据由 tools/gen_lifeguide_lua.py 生成 → 单文件 ~365KB (数据段 130K 字符)
    #   三个页面: 随机一条(像壁纸) / 按章节列表 / 详情可滚动
    "lifeguide": ("app", "人生指南", "学习",
                  "676 条高性价比建议：随机一条看，也能按章节翻阅", "ICON_BOOK"),
    # ★ 离线维基: TF 卡上的 /sdcard/wiki/<库名>/ 逐行流式阅读
    #   数据由 ~/AI/nomad-wiki/tools/build_wiki_db.py 从 ZIM 生成；解压在 C 侧（app.wiki.*）
    "wiki": ("app", "离线维基", "学习",
             "TF 卡上的维基百科：随机一条或按标题浏览，逐行解压不占内存", "ICON_BOOK"),
    "wuxia": ("app", "英雄坛说", "游戏",
              "原版地图整图 + 原版人物 + 63 张场景: 拜师/行囊/回合战斗/走江湖", "ICON_MINI_GAMES"),
    "qqtang": ("app", "QQ堂冒险", "游戏",
               "泡泡炸砖块探无限地牢: 道具/背包/技能升级/越深越难", "ICON_MINI_GAMES"),
    # ★ Mini DayZ 掌机版: 世界生成规则**学自源工程** environment/ground.gd
    #   (地块 17×17 格 / 整图 8×8 地块 / 逐行逐列"连前两个地点" / 空地 1/9 变水 /
    #    路宽 4 格在块内偏移 6..9), 素材 tools/mdz_pack.py 打包 → /store/minidayz/。
    #   僵尸分布按"贴地点密度"(源工程僵尸系统尚未实现, 只有美术)。
    "minidayz": ("app", "迷你DayZ", "游戏",
                 "废墟搜刮 + 僵尸生存: 找人烟、抢物资、别被围死", "ICON_MINI_GAMES"),
    # ★ Earth Survival 掌机版: 移植自开源项目 Cod-e-Codes/Love2D-TestGame (Love2D/Lua)。
    #   原版**全部用几何图形绘制**(没有贴图) → 转 1bpp 几乎零信息损失, 是"低分辨率原生"的正例。
    #   瓦片改成 32px(linit 定: 原版 60px 缩到 32px 这一档) + 相机跟随。
    "earthsurvival": ("app", "地球生存", "游戏",
                      "开源移植: 吃光能量球, 躲开敌人与鲨鱼, 水和流沙会耗能量", "ICON_MINI_GAMES"),
    # 图标: 2026-10-15 linit 要求换独立图标 (原借 ICON_NETDECT=27 网络检测) →
    #   ICON_PLANERADAR(=36, 当天追加进 icons_main.inc, PNG=完美图标/应用管家/运维/航班雷达.png)
    "planeradar": ("ops", "航班雷达", "运维",
                   "自动定位当前位置, 扫描附近空域航班: 距离/方位/高度/地速/呼号", "ICON_PLANERADAR"),
    # ★ 码表 (自行车码表, 第一版): 纯本机 GPS, **不依赖 Garmin**。
    #   数据链: MAX-M10S → gps.c(传输) → gps_nmea.c(解析) → app.gps.*(Lua 桥) → bikecomp.lua
    #   三页: 码表 / 轨迹 / GPS 诊断(上机调线就看这页)。GPS 未接好时自动降级成内置模拟。
    #   心率栏是**预留位**(显示 --), 后续接 BLE 心率带或 Garmin 广播心率时只填 G.hr, 界面不动。
    #   图标 ICON_BIKE 是程序化线描 (64×64, 墨量 0.17), 不是彩色缩图。
    "bikecomp": ("app", "码表", "程序",
                 "自行车码表: GPS 速度/里程/轨迹, 心率栏已预留", "ICON_BIKE"),
    # ★ 矢量离线地图: 数据是 tools/map_vec.py 从 OSM(Overpass) + terrarium DEM 编译出来的
    #   **矢量包** .ltm (只有折线点串, 不是图片), 放在 apps/lua/map/ → 设备上 /store/map/。
    #   设备端 map.lua 按当前缩放选 LOD 档实时投影+画线, 所以一份数据覆盖全部 zoom, 任意
    #   放大都清晰、文字永远是固件 16px 点阵。
    #   ⚠ 图标走**独立 PNG** (tools/gen_map_icon.py 程序化线描), 不放进 main_icons[] ——
    #     那张表的下标 = ABI (main_icon_index() 有重映射表), 往里插会动到既有应用。
    "map": ("app", "矢量地图", "工具",
            "离线矢量地图: 道路/等高线双图层, 任意缩放不糊, GPS 定位", "assets/icons/map_app.png"),
}
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

# 商店待装包: 放 SPIFFS 根 (/store/<name>.lua) 让商店"可下载", **不预装**到 apps/{cat}/
#   (装了就不是"下载安装"了 —— am_store.c 的已装判定就是看 apps/{cat} 里有没有同名文件)
STORE_ONLY = set()   # ★ 2026-10-03 linit: 商店包与应用副本**二选一** —— 两份会让同一个应用
                     #   在「商店」和「应用」各出现一次, 看着像"分成两个"。已装的应用只保留
                     #   /store/apps/<cat>/<name>.lua 一份 (见 targets_of)。

# 能力声明 → LAPP 头部 `--@app need=...`。启动侧据此做门禁:
#   need=wifi 的应用在未联网时**不直接进**, 先弹确认框 → 复用系统"连接WiFi" → 连上再进
#   (实现在 components/apps/pages/am_lua.c; 取消/超时则不启动并 toast)。
NEED = {"planeradar": "wifi"}


def targets_of(n):
    """某个应用要写盘 / 要打包的**全部**路径。

    - 预装副本: apps/lua/apps/{cat}/{n}.lua  → 设备上 /store/apps/{cat}/{n}.lua (应用管家→应用)
    - 商店包  : apps/lua/{n}.lua            → 设备上 /store/{n}.lua (商店里可下载)

    ⚠ STORE_ONLY 的**两份都写**(2026-10-02 定案)。原因: 整片烧 16MB 会重建整个 SPIFFS,
      只放商店的话每烧一次应用就没了, 用户得重新去商店装一遍 —— 实测被误判成
      "应用进去没反应"(其实 /store/apps/app/xxx.lua 不存在 → Lua 页退回文件列表态)。
      两份内容一致(都过 LAPP 打包), 商店里仍显示"已安装"并可卸载, 语义不变。
    """
    inst = os.path.join(DEST_ROOT, MANIFEST[n][0], n + ".lua")
    if n in STORE_ONLY:
        store = os.path.normpath(os.path.join(DEST_ROOT, "..", n + ".lua"))
        return [store, inst]
    return [inst]


def dest_of(n):
    """兼容旧调用: 打包/校验时的**主**路径"""
    return targets_of(n)[0]


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

    names = args.apps or list(MANIFEST) if BUILD_INTERNAL else []
    if not names:
        print("固件不内置任何 Lua 应用 (BUILD_INTERNAL=False) ⇒ 跳过复制/打包, 只出空镜像")
    for n in names:
        if n not in MANIFEST:
            raise SystemExit("MANIFEST 里没有这个应用: %s (已有: %s)"
                             % (n, ", ".join(MANIFEST)))

    print("① 桌面源码 → 应用安装区 / 商店待装包")
    for n in names:
        cat = MANIFEST[n][0]
        src = os.path.join(SRC_DIR, n + ".lua")
        if not os.path.exists(src):
            raise SystemExit("桌面没有源码: %s" % src)
        for t in targets_of(n):
            os.makedirs(os.path.dirname(t), exist_ok=True)
            shutil.copy(src, t)
        if n in STORE_ONLY:
            print("   %-11s → apps/lua/%s.lua [商店包] + apps/lua/apps/%s/%s.lua [预装]"
                  % (n, n, cat, n))
        else:
            print("   %-11s → apps/lua/apps/%s/%s.lua" % (n, cat, n))

    print("② 语法 + 契约闸门")
    files = [t for n in names for t in targets_of(n)]
    run([PY, "tools/lua_check.py"] + files, quiet=False)

    print("③ LAPP 打包 (图标取自固件 icons_main.inc, 与主菜单同源)")
    for n in names:
        cat, label, tag, desc, icon = MANIFEST[n]
        need = NEED.get(n)
        for f in targets_of(n):          # 商店包与预装副本都要打 LAPP 头(否则管家读不到名称/图标)
            cmd = [PY, "tools/pack_app.py", "pack", f, "--name", label, "--cat", tag,
                   "--desc", desc]
            # 图标两种来源: "ICON_XXX" = 取固件 main_icons[] 同源位图; "*.png" = 独立图片,
            # 打进 .lua 自己的 LAPP 前奏 (不碰固件图标表, 不动 ABI)。
            if icon.endswith(".png"):
                cmd += ["--icon", os.path.join(ROOT, icon)]
            else:
                cmd += ["--icon-from-main", icon]
            if need:
                cmd += ["--need", need]
            run(cmd)
        print("   ✓ %-11s %s%s" % (n, label, ("  (need=%s)" % need) if need else ""))

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
    store = [n for n in names if n in STORE_ONLY]
    if store:
        print("\n商店待装包: %s" % ", ".join(store))
        print("  设备上的用法: 应用管家 → 商店 → 点它 → 弹「安装 XXX ?」→ 确认 →")
        print("  复制到 /store/apps/{cat}/ → 回「应用」分类点开即可 (可再卸载)。")
    print("\n完成。烧录时由 merge_flash.sh 把该镜像写到 0x%X (apps 分区, 地址同样读自 partitions.csv)。" % off)


if __name__ == "__main__":
    main()
