#!/usr/bin/env python3
"""tests/emu_save/contract_check.py — 5 个模拟器存档实现的源码契约检查

NES / GBC / gam4980 的存档函数与内核上下文强耦合, 无法在主机独立编译
(要造假 nes_context / gw 上下文, 那就不是"测生产逻辑"了)。因此这三个用
**源码契约检查**, MD / SMS 另有完整主机测试 (test_md_savestate.cpp /
test_sms_savestate.cpp)。

这里检查的是"存档安全策略是否仍然在位", 每条都对应一个已发生或险些发生的事故:
  · 回退成 fopen("wb") 直接写 → 掉电/拔卡毁掉唯一存档
  · 丢掉 recover_auto      → 掉电后 .old 里的完好旧档永远救不回来
  · 周期存档改回每帧 mkdir  → 每 5 秒两次 FAT 操作, 一小时 1440 次
  · 周期写误置 s_sram_saved → 退出时不再写档, 丢最后 5 秒进度

用法: python3 tests/emu_save/contract_check.py [项目根]
退出码: 0=通过, 1=违规, 2=文件缺失
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))

FILES = {
    "NES": "components/engines/nes_emu/nes_emu.c",
    "GBC": "components/engines/gbc_emu/gbc_emu.c",
    "GAM": "components/engines/gam4980/gam4980_emu.c",
    # MD/SMS 的**包装层** (原子替换/门槛/recover)
    "MD":  "components/engines/md_emu/md_savestate.cpp",
    "SMS": "components/engines/sms_emu/sms_cbridge.cpp",
    # MD/SMS 的**主控层** (周期节奏/路径规则)
    "MD_MAIN":  "components/engines/md_emu/md_emu.c",
    "SMS_MAIN": "components/engines/sms_emu/sms_emu.c",
}

_p = _f = 0


def ck(cond, name, detail=""):
    global _p, _f
    if cond:
        _p += 1
        print("  PASS  %s" % name)
    else:
        _f += 1
        print("  FAIL  %s%s" % (name, ("  << " + detail) if detail else ""))


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def load(key):
    path = os.path.join(ROOT, FILES[key])
    if not os.path.isfile(path):
        print("SKIP: 找不到 %s" % FILES[key])
        sys.exit(2)
    with open(path, "r", encoding="utf-8", errors="replace") as fp:
        return strip_comments(fp.read())


def main():
    src = {k: load(k) for k in FILES}

    print("== A. 存档写入必须走原子接口 (不得裸 fopen \"wb\") ==")
    for k in ("NES", "GBC", "GAM"):
        ck(re.search(r"os_file_write_atomic\s*\(", src[k]) is not None,
           "A/%s 使用 os_file_write_atomic()" % k)
    for k in ("MD", "SMS"):
        ck(re.search(r"os_file_replace\s*\(", src[k]) is not None,
           "A/%s 使用 os_file_replace()" % k)
        ck("SAVE_MIN_BYTES" in src[k],
           "A/%s 有最小字节门槛 (挡住'完全没写成')" % k)
        ck(re.search(r"SAVE_MIN_BYTES\s*=\s*512", src[k]) is not None,
           "A/%s 门槛值 = 512B" % k)

    print("\n== B. 加载前必须做掉电恢复 ==")
    for k in ("NES", "GBC", "GAM", "MD", "SMS"):
        ck(re.search(r"os_file_recover_auto\s*\(", src[k]) is not None,
           "B/%s 调用 os_file_recover_auto()" % k)

    print("\n== C. 周期存档节奏统一 300 帧 ==")
    ck(re.search(r"#define\s+NES_SRAM_SAVE_FRAMES\s+300", src["NES"]) is not None,
       "C/NES 周期常量 = 300")
    ck(re.search(r"#define\s+GBC_SRAM_SAVE_FRAMES\s+300", src["GBC"]) is not None,
       "C/GBC 周期常量 = 300")
    ck(re.search(r"frame_count\s*%\s*300\s*==\s*0", src["MD_MAIN"]) is not None,
       "C/MD 周期 = frame_count % 300 (md_emu.c)")
    ck(re.search(r"frame_count\s*%\s*300\s*==\s*0", src["SMS_MAIN"]) is not None,
       "C/SMS 周期 = frame_count % 300 (sms_emu.c)")

    print("\n== D. 存档目录只建一次 (第4回合性能修正不得回退) ==")
    ck(re.search(r"static\s+bool\s+s_ready", src["NES"]) is not None,
       "D/NES 有 s_ready 标志")
    ck(re.search(r"if\s*\(\s*s_ready\s*\)\s*return", src["NES"]) is not None,
       "D/NES 建过就提前返回")
    ck("gbc_save_dir_ready" in src["GBC"], "D/GBC 有 gbc_save_dir_ready 标志")
    ck(re.search(r"if\s*\(\s*gbc_save_dir_ready\s*\)\s*return", src["GBC"]) is not None,
       "D/GBC 建过就提前返回")
    # GBC 在 GB/GBC 之间切换时目录会变, 标志必须复位
    ck(re.search(r"gbc_save_dir_ready\s*=\s*false", src["GBC"]) is not None,
       "D/GBC 切换存档目录时复位标志")

    print("\n== E. 周期写不得置'已保存'标志 (否则退出时不再补写) ==")
    m = re.search(r"Nes|NES", "")  # noqa: F841  (保持结构对称)
    # 取 NES 里 "save_tick >= NES_SRAM_SAVE_FRAMES" 附近 12 行, 不得出现 s_sram_saved
    for key, marker in (("NES", r"save_tick\s*>=\s*NES_SRAM_SAVE_FRAMES"),
                        ("GBC", r"save_tick\s*>=\s*GBC_SRAM_SAVE_FRAMES")):
        mm = re.search(marker, src[key])
        if not mm:
            ck(False, "E/%s 找到周期存档判定" % key)
            continue
        ck(True, "E/%s 找到周期存档判定" % key)
        win = src[key][mm.start():mm.start() + 600]
        ck("s_sram_saved" not in win,
           "E/%s 周期写块内不出现 s_sram_saved (=true 会吞掉退出补写)" % key)

    print("\n== F. 存档路径规则 ==")
    ck('"/sdcard/data/saves/NES/' in src["NES"], "F/NES → saves/NES/<name>.sav")
    ck(re.search(r'"/sdcard/data/saves/GB', src["GBC"]) is not None,
       "F/GBC → saves/GB 或 GB 子目录")
    ck('"/sdcard/data/saves/MD/' in src["MD_MAIN"], "F/MD → saves/MD/<name>.sav")
    ck('"/sdcard/data/saves/SMS/' in src["SMS_MAIN"], "F/SMS → saves/SMS/<name>.sav")
    ck(re.search(r'#define\s+SAVE_DIR\s+"/sdcard/data/saves"', src["GAM"]) is not None,
       "F/GAM SAVE_DIR = /sdcard/data/saves")
    # 记录: gam4980 存档直接放 saves/ 根下, 没有平台子目录 (既有设计, 非缺陷)。
    # 风险: 若两个不同目录的 rom 同名 (如 a/x.rom 与 b/x.rom), 存档会互相覆盖。
    has_subdir = re.search(r'SAVE_DIR\s+"/sdcard/data/saves/\w+"', src["GAM"])
    print("  INFO  gam4980 无平台子目录: %s" %
          ("是 (saves/ 根下, 同名 rom 会撞存档)" if not has_subdir else "否"))

    print("\n== G. 成功日志降级 (周期存档不得刷 INFO) ==")
    for k in ("NES", "GBC"):
        # 保存成功分支必须是 LOGD / 无日志, 不能是 LOGI
        seg = src[k]
        idx = seg.find("os_file_write_atomic")
        ok = False
        if idx > 0:
            tail = seg[idx:idx + 700]
            # 成功分支里 ESP_LOGI 会刷屏; 允许 W/D
            ok = ("ESP_LOGD" in tail) or ("ESP_LOGI" not in tail)
        ck(ok, "G/%s 周期存档成功不打 INFO" % k)

    print("\n---------------------------------------------")
    print("PASS=%d  FAIL=%d  合计=%d" % (_p, _f, _p + _f))
    print("结果: %s" % ("FAILED" if _f else "ALL PASS"))
    return 1 if _f else 0


if __name__ == "__main__":
    sys.exit(main())
