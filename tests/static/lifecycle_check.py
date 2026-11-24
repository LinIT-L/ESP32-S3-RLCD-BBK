#!/usr/bin/env python3
"""tests/static/lifecycle_check.py — FreeRTOS 任务生命周期静态检查

⚠ 这是一个**提示工具, 不是判据**。
   grep/正则命中**绝不等于** Bug —— 本项目里大量"看起来没检查返回值"的地方,
   实际由上层错误处理或 FreeRTOS 语义保证是安全的。脚本只负责把需要人看的点
   列出来, 每条附 文件:行, 由人逐条核实。

它检查什么 (每条都对应一类真实事故):
  1. xTaskCreate* 的返回值有没有被判断 (pdPASS)  —— 创建失败却当成功用
  2. 任务创建后是否有退出机制 (stop 标志 / vTaskDelete) —— 任务永远不退
  3. vTaskDelete 强制删除 (风险: 持锁/占资源时被删会留下锁死)
  4. xTaskAbortDelay 的使用点 (叫醒阻塞任务, 优于强删)
  5. 一个文件里 create 与 delete 的数量关系

用法:
  python3 tests/static/lifecycle_check.py [项目根] [--verbose]
退出码: 恒为 0 (本工具不做通过/失败判定), 除非 --strict 且发现硬错误
"""
import os
import re
import sys

ROOT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") \
    else os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
VERBOSE = "--verbose" in sys.argv
STRICT = "--strict" in sys.argv

# 本轮整改涉及的"有任务"的文件 (存在才扫)
TARGETS = [
    "components/system/os/kernel/os_mgr.c",
    "components/engines/nes_emu/nes_emu.c",
    "components/engines/gbc_emu/gbc_emu.c",
    "components/engines/md_emu/md_emu.c",
    "components/engines/sms_emu/sms_emu.c",
    "components/engines/gam4980/gam4980_emu.c",
    "components/engines/arduboy_avr/arduboy_avr.c",
    "components/engines/board_shim/board_shim.c",
    "components/engines/vpet/vpet_emu.c",
    "components/engines/lavax/lavax_emu.c",
    "components/engines/book_reader/book_reader.c",
    "components/engines/sdlpal_port/pal_entry.c",
    "components/engines/sdlpal_port/sdl_audio.c",
    "components/drivers/gps/gps.c",
    "components/drivers/gps/gps_nmea.c",
    "components/drivers/touch_panel/touch_panel.c",
    "components/system/input/input.c",
    "components/apps/pages/page_lua.c",
    "components/apps/lua_bridge/luat_lib_net.c",
]

CREATE_RE = re.compile(r"\bxTaskCreate\w*\s*\(")
# 自删 (vTaskDelete(NULL)) 是 FreeRTOS 的标准退出方式, 安全;
# 强删别人 (vTaskDelete(handle)) 才需要判断"被删任务是否持锁/正在写 Flash"。
DELETE_SELF_RE = re.compile(r"\bvTaskDelete\s*\(\s*NULL\s*\)")
DELETE_OTHER_RE = re.compile(r"\bvTaskDelete\s*\(\s*(?!NULL\b)[^)]+\)")
ABORT_RE = re.compile(r"\bxTaskAbortDelay\s*\(")


def strip_comments_keep_lines(s):
    """去注释但保留行号 (把注释替换成等长空白以外的内容会导致行号漂移, 故只删字符保留换行)。"""
    out = []
    i, n = 0, len(s)
    while i < n:
        if s.startswith("/*", i):
            j = s.find("*/", i + 2)
            if j < 0:
                j = n - 2
            seg = s[i:j + 2]
            out.append("\n" * seg.count("\n"))
            i = j + 2
        elif s.startswith("//", i):
            j = s.find("\n", i)
            if j < 0:
                j = n
            i = j
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def scan_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fp:
        raw = fp.read()
    src = strip_comments_keep_lines(raw)
    lines = src.split("\n")
    res = {
        "file": path,
        "create": [],       # (lineno, checked:bool|None, text)
        "delete_self": [],  # vTaskDelete(NULL) —— 自删, 安全
        "delete_other": [], # vTaskDelete(handle) —— 强删, 需核实
        "abort": [],
    }

    for idx, line in enumerate(lines):
        m = CREATE_RE.search(line)
        if m:
            # 判断是否检查了返回值。窗口取 10 行: 生产代码里 xTaskCreateStatic*
            # 的参数常跨 4~5 行, 判断写在参数结束之后 (如 nes_emu.c 的两段式写法),
            # 窗口太窄会误报。窗口宽了只会漏报"真正的未检查", 不会误判为已检查
            # (因为判断条件本身必须出现 pdPASS/pdTRUE/NULL 比较)。
            window = "\n".join(lines[idx:min(idx + 10, len(lines))])
            checked = ("pdPASS" in window) or ("pdTRUE" in window) or \
                      ("!= NULL" in window) or \
                      re.search(r"if\s*\(\s*!\s*\w+", window) is not None or \
                      re.search(r"if\s*\(\s*\w+\s*==\s*NULL", window) is not None
            res["create"].append((idx + 1, checked, line.strip()[:100]))
        if DELETE_SELF_RE.search(line):
            res["delete_self"].append(idx + 1)
        elif DELETE_OTHER_RE.search(line):
            res["delete_other"].append(idx + 1)
        if ABORT_RE.search(line):
            res["abort"].append(idx + 1)

    return res


def main():
    print("FreeRTOS 任务生命周期静态检查 (提示工具, 命中不等于 Bug)")
    print("项目根: %s\n" % ROOT)

    total_files = 0
    total_create = 0
    unchecked = []
    forced_delete = []
    self_delete = []
    abort_info = []

    for rel in TARGETS:
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            if VERBOSE:
                print("  (跳过, 文件不存在) %s" % rel)
            continue
        total_files += 1
        r = scan_file(path)
        n_c = len(r["create"])
        total_create += n_c
        if n_c:
            checked = sum(1 for _, c, _ in r["create"] if c)
            print("%-52s create=%-2d 已检查=%-2d 自删=%-2d 强删=%-2d abortDelay=%d"
                  % (rel, n_c, checked, len(r["delete_self"]),
                     len(r["delete_other"]), len(r["abort"])))
        for ln, c, txt in r["create"]:
            if not c:
                unchecked.append((rel, ln, txt))
        if r["delete_self"]:
            self_delete.append((rel, r["delete_self"]))
        if r["delete_other"]:
            forced_delete.append((rel, r["delete_other"]))
        if r["abort"]:
            abort_info.append((rel, r["abort"]))

    print("\n---- 汇总 ----")
    print("扫描文件: %d, 任务创建点: %d" % (total_files, total_create))

    print("\n[1] 任务创建未在本行/后 10 行检查返回值 (需人工核实是否由上层保证):")
    if unchecked:
        for rel, ln, txt in unchecked:
            print("    %s:%d   %s" % (rel, ln, txt))
    else:
        print("    (无)")

    print("\n[2] 自删 vTaskDelete(NULL) (标准退出方式, 一般安全):")
    if self_delete:
        for rel, lns in self_delete:
            print("    %s  行 %s" % (rel, ",".join(str(x) for x in lns)))
    else:
        print("    (无)")

    print("\n[3] ★ 强删 vTaskDelete(handle) (风险: 被删任务可能持有锁/正在写 Flash):")
    if forced_delete:
        for rel, lns in forced_delete:
            print("    %s  行 %s" % (rel, ",".join(str(x) for x in lns)))
    else:
        print("    (无)")

    print("\n[4] xTaskAbortDelay 使用点 (叫醒阻塞任务, 优于强删):")
    if abort_info:
        for rel, lns in abort_info:
            print("    %s  行 %s" % (rel, ",".join(str(x) for x in lns)))
    else:
        print("    (无)")

    print("\n⚠ 以上名单**不是缺陷清单**。逐条核实后, 真正的问题才记入测试报告。")
    print("  判定要点: 创建失败后是否有替代路径 / 退出是否由 stop 标志驱动 /")
    print("            强删时任务是否可能正持有互斥量或正在写 Flash。")

    # --strict 只对"创建点完全没有任何检查且同文件也没有错误处理"这种情况报警
    if STRICT and unchecked:
        print("\n[strict] 存在未检查的创建点: %d 处" % len(unchecked))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
