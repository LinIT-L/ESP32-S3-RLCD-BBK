#!/usr/bin/env python3
"""tests/input_guard/contract_check.py — 按键 pad 自愈 / 解锁 / 静音的源码契约

背景（2026-11-23 现场）:
  GPIO0(返回) 与 GPIO4(确认) 同时恒读 0, 状态机双双停在 LONG_FIRED, 18 分钟零动作,
  只有重启才恢复。input.c 的修复分四块, 本脚本把每块**必须在代码里成立的前提**钉住,
  防止以后重构时悄悄改回"看起来修了其实没修"的形态。

检查的是**生产源码的结构契约**, 不是行为正确性 —— 电平/时序依赖真实 GPIO 与 PM,
主机无法替代。那些列入真机清单。

A 解锁序列   三道 pad 锁的顺序 + 必须早于 gpio_config
B 睡眠切换   键脚必须 sleep_sel_dis, 且排在 power_mgr_pin_init() 之后
C 自愈判据   A(整组全低) + B(单键卡低) 并存, B 必须用位图覆盖**所有**卡低键
D 静音       必须在 tick() 之前, 且只在"已修过且仍低"时生效
E 复位       自愈后状态机必须回 IDLE, 否则继续等释放
F 取证       keys 快照必须能看见 MUX 寄存器与低电平时长
G 唤醒源     键脚不得在开机时常驻登记为 EXT1 唤醒源（否则每次浅睡都被搬去 RTC 域）

用法: python3 tests/input_guard/contract_check.py [项目根]
退出码: 0=全部符合, 1=有违规, 2=源文件缺失
"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = os.path.join(ROOT, "components/system/input/input.c")
SRC_PM = os.path.join(ROOT, "components/system/power_mgr/power_mgr.c")

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
    """去掉注释但**保留换行**, 否则后面报的行号会整体偏移。"""
    s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def func_body(src, sig):
    """按函数签名取函数体（大括号配平）。

    sig 后面强制要求左花括号 —— 否则会先匹配到**前向声明**
    （如 `static void input_key_task(void *arg);`），把它后面某个函数的
    函数体当成自己的，导致整组检查假失败。
    """
    m = re.search(sig + r"\s*\{", src)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


def pos(body, pat, start=0):
    m = re.search(pat, body[start:])
    return -1 if not m else start + m.start()


def main():
    if not os.path.isfile(SRC):
        print("找不到 %s" % SRC)
        return 2
    raw = open(SRC, encoding="utf-8", errors="replace").read()
    src = strip_comments(raw)

    print("=" * 68)
    print("按键 pad 自愈 / 解锁 / 静音 契约检查")
    print("源码: %s" % os.path.relpath(SRC, ROOT))
    print("=" * 68)

    unlock = func_body(src, r"static void input_pad_unlock_pins\s*\([^)]*\)")
    reinit = func_body(src, r"static void input_keys_reinit_pins\s*\(\s*void\s*\)")
    guard = func_body(src, r"static void input_key_guard\s*\(\s*void\s*\)")
    dump = func_body(src, r"void input_keys_dump\s*\(\s*void\s*\)")
    initt = func_body(src, r"void input_init\s*\(\s*void\s*\)")
    ktask = func_body(src, r"static void input_key_task\s*\([^)]*\)")

    # ---------------- A 解锁序列 ----------------
    print("\n[A] pad 解锁序列")
    ck(unlock is not None, "A1 input_pad_unlock_pins() 存在（开机与自愈共用同一序列）")
    if unlock:
        p_deep = pos(unlock, r"gpio_deep_sleep_hold_dis\s*\(")
        p_pin = pos(unlock, r"gpio_hold_dis\s*\(")
        p_rtc_hold = pos(unlock, r"rtc_gpio_hold_dis\s*\(")
        p_rtc_deinit = pos(unlock, r"rtc_gpio_deinit\s*\(")
        ck(p_deep >= 0, "A2 解全局 pad 冻结 gpio_deep_sleep_hold_dis()")
        ck(p_pin >= 0, "A3 解单脚 hold gpio_hold_dis()")
        ck(p_rtc_hold >= 0, "A4 解 RTC hold rtc_gpio_hold_dis()")
        ck(p_rtc_deinit >= 0, "A5 解 RTC 隔离 rtc_gpio_deinit()")
        ck(p_deep >= 0 and p_pin >= 0 and p_deep < p_pin,
           "A6 顺序: 全局冻结 → 单脚 hold", "deep=%d pin=%d" % (p_deep, p_pin))
        ck(p_pin >= 0 and p_rtc_hold >= 0 and p_pin < p_rtc_hold,
           "A7 顺序: 单脚 hold → RTC hold", "pin=%d rtc=%d" % (p_pin, p_rtc_hold))
        ck(p_rtc_hold >= 0 and p_rtc_deinit >= 0 and p_rtc_hold < p_rtc_deinit,
           "A8 顺序: RTC 先解锁再 deinit（锁着时 deinit 无效）",
           "hold=%d deinit=%d" % (p_rtc_hold, p_rtc_deinit))

    # ---------------- B 睡眠切换 ----------------
    print("\n[B] 键脚不参与浅睡切换")
    if initt:
        p_unlock = pos(initt, r"input_pad_unlock_pins\s*\(")
        p_cfg = pos(initt, r"gpio_config\s*\(")
        ck(p_unlock >= 0, "B1 input_init 调 input_pad_unlock_pins()")
        ck(p_unlock >= 0 and p_cfg >= 0 and p_unlock < p_cfg,
           "B2 解锁必须早于 gpio_config（被锁 pad 会静默忽略 gpio_config）",
           "unlock=%d cfg=%d" % (p_unlock, p_cfg))
        n_sel = len(re.findall(r"gpio_sleep_sel_dis\s*\(", initt))
        ck(n_sel >= 3, "B3 input_init 对三个键脚都 sleep_sel_dis（找到 %d 处）" % n_sel)
        p_pininit = pos(initt, r"power_mgr_pin_init\s*\(\s*\)")
        p_sel = pos(initt, r"gpio_sleep_sel_dis\s*\(")
        ck(p_pininit >= 0 and p_sel >= 0 and p_pininit < p_sel,
           "B4 sleep_sel_dis 必须晚于 power_mgr_pin_init()"
           "（后者内部还会跑一次 gpio_config，早设会被覆盖）",
           "pininit=%d sel=%d" % (p_pininit, p_sel))
    if reinit:
        ck(re.search(r"gpio_sleep_sel_dis\s*\(", reinit) is not None,
           "B5 自愈重配也必须 sleep_sel_dis（否则修好→下次浅睡又坏）")
        p_u = pos(reinit, r"input_pad_unlock_pins\s*\(")
        p_c = pos(reinit, r"gpio_config\s*\(")
        ck(p_u >= 0 and p_c >= 0 and p_u < p_c,
           "B6 自愈路径同样是 先解锁 → 再配置", "unlock=%d cfg=%d" % (p_u, p_c))

    # ---------------- C 自愈判据 ----------------
    print("\n[C] 自愈判据")
    ck(re.search(r"#define\s+KEY_ALL_LOW_MS\s+(\d+)", src) is not None,
       "C1 判据 A 的整组全低阈值存在")
    m_stuck = re.search(r"#define\s+KEY_STUCK_MS\s+(\d+)", src)
    ck(m_stuck is not None, "C2 判据 B 的单键卡低阈值存在（小智板 PWR 恒高，判据 A 永不成立）")
    if m_stuck:
        stuck_ms = int(m_stuck.group(1))
        # 最长合法长按: BOOT 3s 回桌面 / PWR 2s 关机确认
        ck(stuck_ms > 3000,
           "C3 KEY_STUCK_MS(%dms) 必须大于最长合法长按 3000ms（BOOT 回桌面）" % stuck_ms)
    if guard:
        ck(re.search(r"low\s*==\s*n", guard) is not None,
           "C4 判据 A 条件仍为 low == n")
        ck(re.search(r"n\s*>=\s*2", guard) is not None,
           "C5 判据 A 要求 n >= 2（避免单键板误判）")
        ck(re.search(r"stuck_mask", guard) is not None,
           "C6 判据 B 用位图 stuck_mask")
        # 位图必须"或等", 不能像旧版那样只会记第一个
        ck(re.search(r"stuck_mask\s*\|=", guard) is not None,
           "C7 位图累积所有超时键（旧版 int stuck 只记第一个 → 两键同时卡低会漏标）")
        ck(re.search(r"for\s*\([^)]*\)\s*[^;]*s_stuck_repaired\s*\[\s*i\s*\]\s*=\s*1", guard) is not None,
           "C8 自愈时对**位图中每个**卡低键打标记（不是只标一个）")
        ck(re.search(r"KEY_HEAL_COOLDOWN", guard) is not None,
           "C9 自愈有冷却，防真短路时刷屏重配")

    # ---------------- D 静音 ----------------
    print("\n[D] 卡死键静音")
    if ktask:
        p_mute = pos(ktask, r"s_stuck_repaired\s*\[")
        p_tick = pos(ktask, r"tick\s*\(\s*&s_btns")
        ck(p_mute >= 0, "D1 input_key_task 有静音分支")
        ck(p_mute >= 0 and p_tick >= 0 and p_mute < p_tick,
           "D2 静音判断在 tick() **之前**（之后判断挡不住本轮动作）",
           "mute=%d tick=%d" % (p_mute, p_tick))
        ck(re.search(r"s_stuck_repaired\s*\[[^\]]*\]\s*&&\s*gpio_get_level", ktask) is not None,
           "D3 静音条件是 已修过 且 当前仍低（松开要能自动恢复）")
        ck(re.search(r"s_btns\s*\[[^\]]*\]\s*\.\s*state\s*=\s*ST_IDLE", ktask) is not None,
           "D4 静音期间把状态机压在 IDLE（不再累积 press_start）")
    ck(re.search(r"s_stuck_repaired\s*\[[^\]]*\]\s*=\s*0", src) is not None,
       "E1 电平回高时清 s_stuck_repaired（否则松开后永久静音）")

    # ---------------- E 状态复位 ----------------
    print("\n[E] 自愈后的状态复位")
    if reinit:
        ck(re.search(r"state\s*=\s*ST_IDLE", reinit) is not None,
           "E2 重配后状态机回 IDLE（否则继续等一个永远不来的释放）")
        ck(re.search(r"press_start\s*=\s*0", reinit) is not None,
           "E3 重配后清 press_start（否则下次按下立刻被当成长按）")

    # ---------------- F 取证 ----------------
    print("\n[F] 现场取证能力")
    if dump:
        ck(re.search(r"MUX\s*\[", dump) is not None or re.search(r"GPIO_PIN_MUX_REG", dump) is not None,
           "F1 keys 快照输出 IO_MUX 寄存器（FUN_IE/SLP_SEL 是判「停在睡眠影子」的唯一直接证据）")
        ck(re.search(r"SLP_SEL", dump) is not None, "F2 快照解码 SLP_SEL")
        ck(re.search(r"FUN_IE", dump) is not None, "F3 快照解码 FUN_IE")
        ck(re.search(r"s_low_since", dump) is not None,
           "F4 快照输出低电平时长（区分「真按住」与「pad 恒 0」的直接指标）")
        ck(re.search(r"MUX_SEL", dump) is not None,
           "F5 快照输出 RTC 域路由位 MUX_SEL"
           "（IO_MUX 那行看不到 pad 归谁管；只有这一位能证明 pad 被搬去模拟 RTC 模块）")
    else:
        ck(False, "F1 keys 快照函数存在")

    # ---------------- G EXT1 唤醒源注册时机 ----------------
    # 背景: `ext1_wakeup_prepare()` 会把掩码内的 RTC pad 接到模拟 RTC 模块
    #   (MUX_SEL=1 ⇒ 数字 GPIO 模块与 pad 断开, gpio_get_level() 恒 0), 且 IDF
    #   在浅睡唤醒后**不**把它切回数字域。本板 wake_key_mask() 退化成 {GP0,GP4}
    #   = 两个按键脚 ⇒ 只要 EXT1 在运行期是注册着的, 每次浅睡都会把按键废掉一次。
    #   ⇒ 契约: EXT1 只能在"真正要深睡的那一刻"注册, 不能在开机常驻。
    print("\n[G] EXT1 唤醒源只在深睡前注册")
    if not os.path.isfile(SRC_PM):
        ck(False, "G0 找到 power_mgr.c", SRC_PM)
    else:
        raw_pm = open(SRC_PM, encoding="utf-8", errors="replace").read()
        src_pm = strip_comments(raw_pm)
        pin_init = func_body(src_pm, r"void power_mgr_pin_init\s*\(\s*void\s*\)")
        ck(pin_init is not None, "G1 power_mgr_pin_init() 存在")
        if pin_init:
            ck(re.search(r"wake_src_set\s*\(", pin_init) is None,
               "G2 开机初始化**不得**注册 EXT1（常驻会让每次浅睡把键脚搬去 RTC 域）")
            ck(re.search(r"keys_config\s*\(", pin_init) is not None,
               "G3 开机仍必须配置键脚（上拉/输入），只是不注册唤醒源")
            ck(re.search(r"ESP_PD_DOMAIN_RTC_PERIPH", pin_init) is not None,
               "G4 RTC_PERIPH 供电策略仍要下发（深睡期间键脚的 RTC 上拉靠它，别顺手删）")

        # 每个 esp_deep_sleep_start() 都必须在同一函数内、且在它之前注册过 EXT1，
        # 否则“去掉开机注册”就会变成“关机后按不醒”。
        # ⚠ 只认**语句**(行首缩进后直接是调用)，否则 ESP_LOGE 里的同名文本会被误当成调用点。
        occ = [m.start() for m in
               re.finditer(r"(?m)^[ \t]*esp_deep_sleep_start\s*\(\s*\)\s*;", src_pm)]
        ck(len(occ) >= 1, "G5 找到 esp_deep_sleep_start() 调用点（%d 处）" % len(occ))
        for p in occ:
            line = src_pm.count("\n", 0, p) + 1
            fn_start = src_pm.rfind("\n}", 0, p)
            seg = src_pm[fn_start if fn_start >= 0 else 0:p]
            ck("wake_src_set" in seg,
               "G6 第 %d 行的深睡调用之前已注册 EXT1（否则关机后按键叫不醒）" % line)
        ck(pin_init is not None and "wake_src_set" not in (pin_init or ""),
           "G7 幂等: 开机不注册, 深睡路径各自注册 —— 不依赖任何残留状态")

    print("\n" + "=" * 68)
    total = _p + _f
    print("合计 %d 项: PASS=%d FAIL=%d" % (total, _p, _f))
    print("⚠ 本脚本只证明「代码结构符合契约」，不证明真机行为。")
    print("  真机项（浅睡唤醒后电平、pad 锁残留）见 docs/测试 的真机清单。")
    print("=" * 68)
    return 0 if _f == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
