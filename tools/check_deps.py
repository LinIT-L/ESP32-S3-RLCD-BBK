#!/usr/bin/env python3
"""check_deps.py — LinTOS 分层依赖守卫 (V1.2 模块化)

为什么需要它: ESP-IDF 的 REQUIRES 只管**链接顺序**, 不管 include 可见性 ——
所有组件的 public 头文件默认全局可见, 所以写错方向也能编过. 因此必须用静态扫描兜底.

检查三类问题:
  [反] 反向依赖   : 下层依赖上层 (会造成循环依赖 / 层次倒置)  —— 默认判为错误
  [隐] 隐藏依赖   : include 了某组件头, 但 REQUIRES 里没写 (借 IDF 全局 include 的空子)
  [冗] 冗余依赖   : REQUIRES 写了某组件, 但源码没 include 它 (可裁剪)

分层 (rank 越小越底层, 依赖只允许 高 rank -> 低 rank):
    0 drivers/  libs/
    1 ui/
    2 services/
    3 system/
    4 engines/
    5 apps/
    9 tools/      (开发工具, 不参与分层约束)

用法:
    python3 tools/check_deps.py            # 全量检查
    python3 tools/check_deps.py --strict   # 隐藏依赖也算错误 (退出码非 0)

已知历史欠债登记在 ALLOW_INVERSIONS / ALLOW_INCLUDES 中, 只报警告;
每还清一笔就从表里删一行 —— 这张表就是"待还技术债清单".
"""
import os
import re
import sys
import glob
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMP_ROOT = os.path.join(ROOT, "components")

LAYER_RANK = {
    "drivers": 0, "libs": 0,
    "ui": 1,
    "services": 2,
    "system": 3,
    "engines": 4,
    "apps": 5,
    "tools": 9,
}

# ---- 已登记的历史欠债 (只警告, 不失败). 还清后请删除对应行. ----
ALLOW_INVERSIONS = {
    # wallpapers(ui) 需要蓝牙/音频/输入做电源与屏保, 属 ui→services 倒置;
    # 彻底修法: 把电源部分拆出 services/power, 由 os 回调驱动 (P5 待做).
    ("wallpapers", "bt_ctl"),
    ("wallpapers", "audio_player"),
    ("wallpapers", "board_shim"),
    ("wallpapers", "input"),
    ("wallpapers", "wifi_manager"),
    ("wallpapers", "svc_nvs"),
    # input(system) 直接吃反馈策略 (音/震动) 与网页手柄服务 —— 应改为事件回调.
    ("input", "tone_player"),
    ("input", "vibrator"),
    ("input", "web_gamepad"),
    # 引擎直接依赖底层硬件/输入 (应统一走 engine_api facade, P2 待做).
    ("gam4980", "web_gamepad"), ("gam4980", "virtual_keys"), ("gam4980", "input"),
    ("gam4980", "bt_ctl"), ("gam4980", "cheater"), ("gam4980", "fatfs"), ("gam4980", "fonts"),
    ("lavax", "input"), ("lavax", "virtual_keys"), ("lavax", "cheater"), ("lavax", "bt_ctl"),
    ("vpet", "input"), ("vpet", "virtual_keys"), ("vpet", "cheater"), ("vpet", "tone_player"),
    ("gbc_emu", "input"), ("gbc_emu", "cheater"), ("gbc_emu", "bt_ctl"), ("gbc_emu", "board_shim"),
    ("nes_emu", "board_shim"), ("md_emu", "board_shim"), ("sms_emu", "board_shim"),
    ("arduboy_avr", "input"), ("arduboy_avr", "virtual_keys"), ("arduboy_avr", "cheater"),
    ("arduboy_avr", "bt_ctl"), ("arduboy_avr", "audio_player"),
    ("gb_emu", "gbc_emu"),
    # ui 组件相互依赖 (ui_common/fonts 已是最底层驱动依赖, 这里仅登记同层调用)
    ("kbd_lib", "ui_common"), ("virtual_keys", "ui_common"),
}

# 允许"用了但不必在 REQUIRES 里写"的头 (由其它组件传递提供 / 纯接口)
ALLOW_INCLUDES = {
    ("app_board", "board_rlcd"),
    ("wallpapers", "board_rlcd"),
}


def component_dirs():
    """返回 {组件名: (分层组, 绝对路径)}"""
    out = {}
    for group in sorted(os.listdir(COMP_ROOT)):
        gdir = os.path.join(COMP_ROOT, group)
        if not os.path.isdir(gdir):
            continue
        if os.path.isfile(os.path.join(gdir, "CMakeLists.txt")):
            out[group] = (group, gdir)      # 顶层即组件 (兼容扁平)
            continue
        for comp in sorted(os.listdir(gdir)):
            cdir = os.path.join(gdir, comp)
            if os.path.isfile(os.path.join(cdir, "CMakeLists.txt")):
                out[comp] = (group, cdir)
    return out


def parse_requires(cdir):
    p = os.path.join(cdir, "CMakeLists.txt")
    s = re.sub(r"#[^\n]*", "", open(p, encoding="utf-8", errors="ignore").read())
    reqs = set()
    # 注: 起始锚点必须用 \b 而不是 (?:^|\s) —— 否则同一 CMakeLists 里
    # "PRIV_REQUIRES ... \n REQUIRES ..." 的第二个 REQUIRES 会漏解析
    # (前一次匹配的零宽 lookahead 停在关键字上, 后面没有空白可消费).
    for m in re.finditer(r"\b(?:PRIV_)?REQUIRES\b\s*(.*?)(?=\)|\b(?:PRIV_)?REQUIRES\b|$)", s, re.S):
        for tok in re.split(r'[\s"]+', m.group(1)):
            tok = tok.strip('"')
            if tok and not tok.startswith(("$", "${", "-", ")")):
                reqs.add(tok)
    return reqs


def parse_includes(cdir):
    """返回源码里 #include "..." 的**原始字符串**(可含路径, 如 gnuboy/sound.h)"""
    inc = set()
    for f in glob.glob(os.path.join(cdir, "**", "*.c"), recursive=True) + \
             glob.glob(os.path.join(cdir, "**", "*.h"), recursive=True):
        if "/build" in f:
            continue
        head = open(f, encoding="utf-8", errors="ignore").read(20000)
        for m in re.finditer(r'#\s*include\s+"([^"]+)"', head):
            inc.add(m.group(1).replace(os.sep, "/"))
    return inc


def parse_include_dirs(cdir):
    """解析组件的 INCLUDE_DIRS (决定其头文件的"可 include 名")"""
    p = os.path.join(cdir, "CMakeLists.txt")
    s = re.sub(r"#[^\n]*", "", open(p, encoding="utf-8", errors="ignore").read())
    dirs = []
    for m in re.finditer(r"\bINCLUDE_DIRS\b\s*(.*?)(?=\)|\b(?:PRIV_)?REQUIRES\b|$)", s, re.S):
        for tok in re.split(r'[\s"]+', m.group(1)):
            tok = tok.strip('"')
            if tok and not tok.startswith("$"):
                dirs.append(tok)
    return dirs or ["."]


def includable_names(cdir):
    """返回 {可 include 的名字: 头文件绝对路径}.

    关键: 头文件的可 include 名 = 相对某个 INCLUDE_DIR 的路径, **不是 basename**.
    例: third_party/gnuboy/include/gnuboy/sound.h 在 INCLUDE_DIRS=include 下,
        可 include 名是 "gnuboy/sound.h" —— 与外组件裸 "sound.h" 并不冲突.
    早期版本按 basename 索引, 会把带路径前缀的头误判成"同名冲突" (假阳性).
    """
    out = {}
    for d in parse_include_dirs(cdir):
        base = os.path.normpath(os.path.join(cdir, d))
        for f in glob.glob(os.path.join(base, "**", "*.h"), recursive=True):
            rel = os.path.relpath(f, base).replace(os.sep, "/")
            out[rel] = f
            # 若 INCLUDE_DIRS 直接指向含该头的目录, 裸文件名也可 include
            if "/" not in rel:
                out[rel] = f
    return out


def main():
    strict = "--strict" in sys.argv
    comps = component_dirs()
    # 可 include 名 -> 归属组件集合 (多个 = 真实同名冲突)
    header_owners = defaultdict(set)
    for name, (group, cdir) in comps.items():
        for inc_name in includable_names(cdir):
            header_owners[inc_name].add(name)

    problems = defaultdict(list)
    for name, (group, cdir) in sorted(comps.items()):
        my_rank = LAYER_RANK.get(group, 9)
        reqs = parse_requires(cdir)
        incs = parse_includes(cdir)

        # 由 include 反推真实依赖 (处理同名头文件: 优先取 REQUIRES 里声明的那个)
        used = set()
        for h in incs:
            owners_all = header_owners.get(h, set())
            if name in owners_all:
                # 自己也提供同名头 (如 input.c include "input.h"): 视为自己的头, 不算外部依赖.
                # 但若同时被别的组件占用同名, 记一条解析风险 (IDF 全局 include 可能先命中别人).
                others = owners_all - {name}
                if others:
                    problems["歧义"].append(
                        '%s 自己的 "%s" 与 %s 同名 (IDF 全局 include 路径有解析风险)'
                        % (name, h, "/".join(sorted(others))))
                continue
            owners = owners_all - {name}
            if not owners:
                continue
            if len(owners) == 1:
                used.add(next(iter(owners)))
            else:
                picked = owners & reqs
                if len(picked) == 1:
                    used.add(next(iter(picked)))
                else:
                    problems["歧义"].append(
                        '%s include "%s" —— 同名头文件属于 %s, 无法确定归属 (IDF 全局 include 路径有解析风险)'
                        % (name, h, "/".join(sorted(owners))))

        for dep in sorted(used):
            if dep not in comps:
                problems["未知组件"].append("%s -> %s (头文件归属不明)" % (name, dep))
                continue
            dep_rank = LAYER_RANK.get(comps[dep][0], 9)
            # 只有"严格反向"才算倒置; 同层互调 (如 services 内 A 用 B) 允许.
            if dep_rank > my_rank and my_rank < 9 and dep_rank < 9:
                tag = "已登记" if (name, dep) in ALLOW_INVERSIONS else "反"
                problems[tag].append("%s[%s] -> %s[%s]" % (name, group, dep, comps[dep][0]))
            if dep not in reqs and (name, dep) not in ALLOW_INCLUDES:
                problems["隐"].append("%s include %s 但未在 REQUIRES" % (name, dep))

        for dep in sorted(reqs):
            if dep in comps and dep not in used and dep != name:
                # 只报"本组件自己的源码没用", 传递依赖除外 (宽松: 不报错)
                problems["冗"].append("%s REQUIRES %s 但源码未 include" % (name, dep))

    def show(title, key):
        items = problems.get(key, [])
        if items:
            print("\n[%s] %d 条" % (title, len(items)))
            for i in sorted(set(items)):
                print("   -", i)

    print("=" * 66)
    print("LinTOS 分层依赖检查 (components/ 共 %d 个组件)" % len(comps))
    print("=" * 66)
    show("反 反向依赖 (错误)", "反")
    show("已登记 历史欠债 (警告, 见 ALLOW_INVERSIONS)", "已登记")
    show("隐 隐藏依赖 (include 未声明)", "隐")
    show("歧义 同名头文件 (解析风险)", "歧义")
    show("冗 冗余依赖 (可裁剪)", "冗")
    show("未知组件", "未知组件")

    hard = len(problems.get("反", []))
    print("\n" + "=" * 66)
    if hard:
        print("❌ 存在 %d 条未登记的反向依赖 —— 请修, 或登记进 ALLOW_INVERSIONS 说明原因" % hard)
        return 1
    print("✅ 无未登记的反向依赖 (历史欠债 %d 条, 隐藏依赖 %d 条)"
          % (len(problems.get("已登记", [])), len(problems.get("隐", []))))
    if strict and problems.get("隐"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
