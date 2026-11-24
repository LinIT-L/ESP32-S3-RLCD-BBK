#!/usr/bin/env python3
"""tests/lua_life/contract_check.py — Lua 生命周期/资源回收的**源码级契约检查**

补 C 语言桩测试覆盖不到的部分: 各 reset 函数体内部的**分支语义**。
这里检查的是"实现是否仍然符合当初的设计意图", 不是代码风格。

每条断言都对应一个真实风险:
  · lua_vm_close 顺序错      → lua_close 之后再用 VM → UAF
  · lua_close 被绕过        → 漏掉 reset, 跨应用状态污染
  · net reset 无条件 free   → 释放正在被网络任务写入的缓冲 → 野指针
  · img_release 不置 NULL   → 重复 reset → double free
  · 表项缺失                → 某个模块的状态永远不被回收

用法: python3 tests/lua_life/contract_check.py [项目根]
退出码: 0=全部通过, 1=有违规, 2=文件缺失
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))

FILES = {
    "page":    "components/apps/pages/page_lua.c",
    "bridge":  "components/apps/lua_bridge/lua_bridge.c",
    "hdr":     "components/apps/lua_bridge/lua_bridge.h",
    "net":     "components/apps/lua_bridge/luat_lib_net.c",
    "img":     "components/apps/lua_bridge/luat_lib_img.c",
    "util":    "components/apps/lua_bridge/luat_lib_util.c",
    "input":   "components/apps/lua_bridge/luat_lib_input.c",
    "sys":     "components/apps/lua_bridge/luat_lib_sys.c",
    "audio":   "components/services/audio_player/audio_player.c",
    "vib":     "components/services/vibrator/vibrator.c",
    "pkg":     "components/apps/lua_bridge/luat_lib_pkg.c",
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


def func_body(src, sig):
    """按签名找到函数体 (含花括号), 用括号配平截取。"""
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
    page = load("page")
    bridge = load("bridge")
    hdr = load("hdr")
    net = load("net")
    img = load("img")
    util = load("util")
    inp = load("input")
    sysm = load("sys")
    pkg = load("pkg")
    audio = load("audio")
    vib = load("vib")

    print("== A. VM 关闭路径 (page_lua.c) ==")

    vm_close = func_body(page, r"static\s+void\s+lua_vm_close\s*\(\s*void\s*\)")
    ck(vm_close is not None, "A1 存在 lua_vm_close()")
    if vm_close:
        i_reset = vm_close.find("lua_bridge_reset_app_state")
        i_close = vm_close.find("lua_close")
        ck(i_reset >= 0, "A2 lua_vm_close 内调用 lua_bridge_reset_app_state()")
        ck(i_close >= 0, "A3 lua_vm_close 内调用 lua_close()")
        ck(i_reset >= 0 and i_close >= 0 and i_reset < i_close,
           "A4 reset 在 lua_close **之前** (否则 VM 已销毁)", 
           "reset@%d close@%d" % (i_reset, i_close))
        ck(re.search(r"if\s*\(\s*s_L\s*\)", vm_close) is not None,
           "A5 有 if (s_L) 守卫 (重复关闭安全)")
        ck(re.search(r"s_L\s*=\s*NULL", vm_close) is not None,
           "A6 关闭后置 s_L = NULL (防悬空)")

    # lua_close 只允许出现在 lua_vm_close 里 —— 任何别处的直接调用都会绕过 reset
    n_close = len(re.findall(r"\blua_close\s*\(", page))
    ck(n_close == 1, "A7 page_lua.c 中 lua_close 仅出现 1 次 (统一走 lua_vm_close)",
       "实际 %d 次" % n_close)

    print("\n== B. 网络缓冲回收 (luat_lib_net.c) ==")

    net_reset = func_body(net, r"void\s+lua_net_reset_app\s*\(\s*void\s*\)")
    ck(net_reset is not None, "B1 存在 lua_net_reset_app()")
    if net_reset:
        ck("netc_busy()" in net_reset, "B2 检查请求是否在跑 netc_busy()")
        ck("netc_cancel()" in net_reset, "B3 在跑时先 netc_cancel()")
        # busy 分支必须 return, 绝不能落到 free
        m = re.search(r"if\s*\(\s*netc_busy\s*\([^)]*\)\s*\)\s*\{", net_reset)
        ck(m is not None, "B4 netc_busy() 用 if 块包裹")
        if m:
            depth, k, end = 0, m.end() - 1, -1
            for j in range(k, len(net_reset)):
                if net_reset[j] == "{":
                    depth += 1
                elif net_reset[j] == "}":
                    depth -= 1
                    if depth == 0:
                        end = j
                        break
            busy_blk = net_reset[k:end + 1] if end > 0 else ""
            ck("return" in busy_blk,
               "B5 busy 分支内 return (不释放正在被写的缓冲)")
            i_free = net_reset.find("heap_caps_free(s_fetch_body)")
            ck(i_free > end > 0, "B6 free 在 busy 分支**之后** (在跑时绝不 free)")
        else:
            ck(False, "B5 busy 分支内 return")
            ck(False, "B6 free 在 busy 分支之后")
        ck(re.search(r"heap_caps_free\s*\(\s*s_fetch_body\s*\)[\s\S]{0,80}s_fetch_body\s*=\s*NULL",
                     net_reset) is not None,
           "B7 free 后置 s_fetch_body = NULL (幂等)")

    print("\n== C. 位图回收 (luat_lib_img.c) ==")

    rel = func_body(img, r"static\s+void\s+img_release\s*\(\s*img_t\s*\*\s*\w+\s*\)")
    ck(rel is not None, "C1 存在 img_release()")
    if rel:
        ck(re.search(r"heap_caps_free\s*\([^)]*\)\s*;[\s\S]{0,60}->\s*bits\s*=\s*NULL", rel) is not None,
           "C2 free 后置 bits = NULL (重复 reset 不 double free)")
        ck("used = false" in rel, "C3 释放后清 used 标志")

    img_reset = func_body(img, r"void\s+lua_img_reset_all\s*\(\s*void\s*\)")
    ck(img_reset is not None, "C4 存在 lua_img_reset_all()")
    if img_reset:
        ck("IMG_MAX" in img_reset and "img_release" in img_reset,
           "C5 遍历全部 IMG_MAX 槽位调用 img_release")

    print("\n== D. 定时器 / 按键 / 弹窗复位 ==")

    ut = func_body(util, r"void\s+lua_util_reset\s*\(\s*void\s*\)")
    ck(ut is not None and "UTIL_TIMERS" in ut and "used" in ut,
       "D1 lua_util_reset 清空全部 UTIL_TIMERS 槽")

    ir = func_body(inp, r"void\s+lua_input_reset_app\s*\(\s*void\s*\)")
    ck(ir is not None and "s_rep_t0" in ir and "s_rep_last" in ir,
       "D2 lua_input_reset_app 同时清「按下时刻」与「上次状态」两张表")

    sr = func_body(sysm, r"void\s+lua_sys_reset_app\s*\(\s*void\s*\)")
    ck(sr is not None and "s_dlg_result" in sr,
       "D3 lua_sys_reset_app 复位弹窗结果")
    ck(sr is not None and "s_mel_end_ms" in sr,
       "D4 lua_sys_reset_app 复位旋律时长 (否则新应用首段音乐被吞)")

    print("\n== E. 调度表一致性 ==")

    tbl = func_body(bridge, r"static\s+void\s*\(\s*\*const\s+k_app_resets\s*\[\s*\]\s*\)")
    ck(tbl is not None, "E1 存在 k_app_resets[] 表")
    if tbl:
        n = len(re.findall(r"lua_\w+_reset\w*\s*,", tbl))
        ck(n == 6, "E2 k_app_resets 恰好 6 项", "实际 %d 项" % n)

    mods = func_body(bridge, r"static\s+void\s*\(\s*\*const\s+k_mods\s*\[\s*\]\s*\)")
    ck(mods is not None, "E3 存在 k_mods[] 表")
    if mods:
        n = len(re.findall(r"lua_mod_\w+_register\s*,", mods))
        ck(n == 18, "E4 k_mods 恰好 18 项", "实际 %d 项" % n)

    # 表里出现的每个 reset 都必须在头文件有声明、且在某 .c 里有定义
    names = []
    if tbl:
        names = re.findall(r"(lua_\w+_reset\w*)\s*,", tbl)
    for nm in names:
        ck(re.search(r"void\s+%s\s*\(\s*void\s*\)\s*;" % re.escape(nm), hdr) is not None,
           "E5 %s 在 lua_bridge.h 有声明" % nm)
        found = False
        for key in ("img", "pkg", "util", "input", "sys", "net"):
            if re.search(r"void\s+%s\s*\(\s*void\s*\)\s*\{" % re.escape(nm),
                         {"img": img, "pkg": pkg, "util": util, "input": inp,
                          "sys": sysm, "net": net}[key]):
                found = True
                break
        ck(found, "E6 %s 有实际定义" % nm)

    # ---------------- H 文件 IO 任务的栈位置 ----------------
    # 真机现场 (2026-11-23): 点电子木鱼 → rst=4 PANIC / task=wavply / IllegalInstruction,
    # backtrace: esp_flash_read → spi1_start → cache_disable。
    # 机理: 读 SPI flash 必须关 cache, 而 PSRAM 与 flash 共用 cache 映射 ⇒ 栈在 PSRAM 的任务
    #       在 fread 当场崩。任何做文件 IO 的任务都必须把栈放内部 RAM。
    print("\n[H] 文件 IO 任务的栈位置 (PSRAM 栈 + 读 flash = 关 cache 期崩)")
    wav = func_body(audio, r"int\s+audio_player_play_wav\s*\(")
    ck(wav is not None, "H1 找到 audio_player_play_wav()")
    if wav:
        m = re.search(r"xTaskCreateWithCaps\s*\(\s*wav_play_task\b[^;]*?\)\s*!=\s*pdPASS", wav, re.S)
        ck(m is not None, "H2 存在 wavply 任务创建")
        if m:
            ck("MALLOC_CAP_INTERNAL" in m.group(0),
               "H3 wavply 栈必须是内部 RAM (读 /store 音频文件的唯一安全位置)",
               "首个创建调用未指定 MALLOC_CAP_INTERNAL")

    rec = func_body(audio, r"int\s+audio_player_rec_start\s*\(")
    ck(rec is not None, "H4 找到 audio_player_rec_start()")
    if rec:
        m = re.search(r"xTaskCreateWithCaps\s*\(\s*rec_task\b[^;]*?\)\s*!=\s*pdPASS", rec, re.S)
        ck(m is not None, "H5 存在 rec 任务创建")
        if m:
            ck("MALLOC_CAP_INTERNAL" in m.group(0),
               "H6 rec 栈必须是内部 RAM (录音 fwrite 可能落在 SPI flash)",
               "首个创建调用未指定 MALLOC_CAP_INTERNAL")

    # ---------------- I 震动必须可停 ----------------
    print("\n[I] 震动停止保障 (duty 停在 255 = 马达常振)")
    vt = func_body(vib, r"void\s+vibrator_tap\s*\(")
    ck(vt is not None, "I1 找到 vibrator_tap()")
    if vt:
        # FreeRTOS 定时器命令用 0 阻塞时失败只返回 pdFAIL, 必须显式判返回值并兜底关断。
        ck(re.search(r"xTimerChangePeriod\s*\([^;]*!=\s*pdPASS", vt, re.S) is not None,
           "I2 定时器启动失败必须判返回值 (队列满时不能静默丢)")
        ck(re.search(r"out_off\s*\(\s*\)", vt) is not None,
           "I3 失败分支必须同步 out_off() 关断 PWM")
    ck(re.search(r"vibrator_stop\s*\(\s*void\s*\)\s*\{", vib) is not None,
       "I4 存在 vibrator_stop() 统一关断入口")
    vstop = func_body(vib, r"void\s+vibrator_stop\s*\(")
    if vstop:
        ck("out_off" in vstop, "I5 vibrator_stop() 必须真正把 duty 归零")

    # Lua 应用退出 = 它触发的震动必须停, 不依赖脚本 on_exit 自觉
    res = func_body(page, r"static\s+void\s+lua_release_app_res\s*\(")
    ck(res is not None, "I6 找到 lua_release_app_res()")
    if res:
        ck("os_notify_vibrate_stop" in res,
           "I7 Lua 退出兜底回收必须停震动 (崩溃/超时强杀时 on_exit 不执行)")

    print("\n---------------------------------------------")
    print("PASS=%d  FAIL=%d  合计=%d" % (_p, _f, _p + _f))
    print("结果: %s" % ("FAILED" if _f else "ALL PASS"))
    return 1 if _f else 0


if __name__ == "__main__":
    sys.exit(main())
