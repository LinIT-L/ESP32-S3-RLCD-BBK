#!/usr/bin/env python3
"""tests/sd_vfs/contract_check.py — SD / VFS 状态一致性契约检查

sd_scan.c 的挂载路径与 esp_vfs_fat_* / sdmmc_* 强耦合, 主机无法真实挂载 SD 卡。
这里检查的是**状态机与目录策略的代码契约**, 目标是把已经踩过的坑钉死:

  · 挂载与格式化用**同一份**目录表 —— 历史事故: 两份清单不同, 格式化后缺目录
  · sd_mkdir_p 逐级创建 + EEXIST 视为成功 —— 幂等, 重复调用不报错
  · 卸载走两级兜底 (unmount 失败 → unregister) —— 否则"路径已注册"卡死后续挂载
  · 卸载后必须复位 s_mounted / s_card / s_mnt_state —— 否则下次挂载走错分支

⚠ 本脚本证明"实现符合既定契约", 不能证明"真机上挂载/卸载真的成功"。
   后者列入真机清单。

用法: python3 tests/sd_vfs/contract_check.py [项目根]
退出码: 0=通过, 1=违规, 2=文件缺失
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = os.path.join(ROOT, "components/drivers/sd_scan/sd_scan.c")

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


def func_body(src, sig):
    m = re.search(sig, src)
    if not m:
        return None
    i = src.find("{", m.end() - 1)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


def main():
    if not os.path.isfile(SRC):
        print("SKIP: 找不到 %s" % SRC)
        return 2
    with open(SRC, "r", encoding="utf-8", errors="replace") as fp:
        src = strip_comments(fp.read())

    print("== A. 目录表: 挂载与格式化共用同一份 ==")
    m = re.search(r"static\s+const\s+char\s*\*\s*const\s+k_sd_dirs\s*\[\s*\]\s*=\s*\{", src)
    ck(m is not None, "A1 存在 k_sd_dirs[] 目录表")
    if m:
        i = src.find("{", m.end() - 1)
        depth, end = 0, -1
        for j in range(i, len(src)):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    end = j
                    break
        tbl = src[i:end + 1] if end > 0 else ""
        ck("NULL" in tbl, "A2 目录表以 NULL 结尾 (可安全遍历)")
        n = len(re.findall(r'"/sdcard/', tbl))
        ck(n >= 10, "A3 目录表条目数合理 (>=10)", "实际 %d" % n)
        for must in ("/sdcard/roms/nes", "/sdcard/roms/gb", "/sdcard/roms/md",
                     "/sdcard/roms/sms", "/sdcard/data/saves"):
            ck(must in tbl, "A4 目录表含 %s" % must)

    # 用量: 挂载路径与格式化路径都应引用同一张表
    n_use = len(re.findall(r"k_sd_dirs\s*\[\s*i\s*\]", src))
    ck(n_use >= 2, "A5 目录表被多处引用 (挂载 + 格式化), 不是各写一份",
       "引用 %d 次" % n_use)

    print("\n== B. sd_mkdir_p: 逐级创建 + 幂等 ==")
    b = func_body(src, r"static\s+bool\s+sd_mkdir_p\s*\(")
    ck(b is not None, "B1 存在 sd_mkdir_p()")
    if b:
        ck(re.search(r"mkdir\s*\(", b) is not None, "B2 调用 mkdir()")
        ck("EEXIST" in b, "B3 已存在(EEXIST)视为成功 (幂等)")
        ck(re.search(r"n\s*<\s*2|n\s*<\s*sizeof", b) is not None,
           "B4 拒绝过短/过长路径 (不越界)")
        ck(re.search(r"\*\s*p\s*==\s*'/'|\*p\s*!=\s*'/'", b) is not None,
           "B5 按 '/' 逐级切分")

    print("\n== C. sd_ensure_dirs: 遍历目录表 ==")
    e = func_body(src, r"static\s+int\s+sd_ensure_dirs\s*\(")
    ck(e is not None, "C1 存在 sd_ensure_dirs()")
    if e:
        ck("k_sd_dirs" in e, "C2 使用共享目录表")
        ck("sd_mkdir_p" in e, "C3 通过 sd_mkdir_p 创建")

    print("\n== D. 卸载: 两级兜底 + 状态复位 ==")
    u = func_body(src, r"static\s+void\s+sd_vfs_unmount_force\s*\(\s*void\s*\)")
    ck(u is not None, "D1 存在 sd_vfs_unmount_force()")
    if u:
        ck("esp_vfs_fat_sdcard_unmount" in u, "D2 先走正常卸载")
        ck("esp_vfs_fat_unregister_path" in u, "D3 失败后强制注销 VFS 路径")
        i_un = u.find("esp_vfs_fat_sdcard_unmount")
        i_reg = u.find("esp_vfs_fat_unregister_path")
        ck(0 <= i_un < i_reg, "D4 顺序正确 (先 unmount, 再 unregister)",
           "unmount@%d unregister@%d" % (i_un, i_reg))

    # 卸载后必须复位三件套。抓"包含 s_mounted = false 且 s_card = NULL"的片段
    ck(re.search(r"s_mounted\s*=\s*false\s*;[\s\S]{0,200}s_card\s*=\s*NULL", src) is not None,
       "D5 卸载后 s_mounted=false 且 s_card=NULL")
    ck(re.search(r"s_mnt_state\s*=\s*SD_MNT_UNKNOWN", src) is not None,
       "D6 卸载后 s_mnt_state=UNKNOWN (允许下次重新探测)")

    print("\n== E. 挂载: 短路 + 失败不留半挂状态 ==")
    ck(re.search(r"if\s*\(\s*s_mounted\s*\)\s*return", src) is not None,
       "E1 已挂载时短路返回 (重复挂载幂等)")
    # ESP-IDF 6.x 的实际 API: esp_vfs_fat_sdmmc_mount (sdcard_mount 是另一套接口名)
    ck(("esp_vfs_fat_sdmmc_mount" in src) or ("esp_vfs_fat_sdcard_mount" in src),
       "E2 使用 esp_vfs_fat_*_mount 挂载")

    print("\n== F. 互斥保护 ==")
    ck("mnt_lock" in src or "mnt_unlock" in src, "F1 挂载/卸载有锁保护")
    n_lock = len(re.findall(r"mnt_lock\s*\(\s*\)", src))
    n_unlock = len(re.findall(r"mnt_unlock\s*\(\s*\)", src))
    # ⚠ 这里**只检查"不漏解锁"**: unlock 数 >= lock 数是正确的, 因为每个加锁函数
    #   通常在"正常出口 + 若干早退分支"各调一次 unlock (本文件 6 个 lock 对应 15 个
    #   unlock, 逐条人工核对均为早退路径)。相反, unlock < lock 才是真的漏解锁。
    #   不要把这个计数差直接当缺陷 —— 见本轮"grep 命中必须人工核实"的要求。
    ck(n_lock > 0 and n_unlock >= n_lock,
       "F2 不会漏解锁 (unlock >= lock)", "lock=%d unlock=%d" % (n_lock, n_unlock))

    print("\n---------------------------------------------")
    print("PASS=%d  FAIL=%d  合计=%d" % (_p, _f, _p + _f))
    print("结果: %s" % ("FAILED" if _f else "ALL PASS"))
    return 1 if _f else 0


if __name__ == "__main__":
    sys.exit(main())
