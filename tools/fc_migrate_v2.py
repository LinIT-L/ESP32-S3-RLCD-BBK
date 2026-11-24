#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 DB 迁移到 v2 (16 字段) 并合并新数据。

- 现有 111 条: 补 layer / vendor / verify, severity 统一三档, keys 去中文
- 新增: 设备管理器 Code 1–57 / 蓝屏 STOP / 引导 0xC0000xxx / 常用系统错误码
- 输出: 重写两个 lua 文件的 DB 段 (逐字一致)

用法:
    python3 tools/fc_migrate_v2.py --dry     # 只统计, 不写文件
    python3 tools/fc_migrate_v2.py           # 写入两个 lua 文件
"""
import os, re, sys, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fc_seed_devmgr as S_DEV
import fc_seed_bsod as S_BSOD
import fc_seed_boot as S_BOOT

ROOT = os.path.dirname(HERE)
LUA_PATHS = [
    os.path.join(ROOT, "store/pkgs/故障信息库.lua"),
    os.path.join(ROOT, "apps/lua/apps/app/故障信息库.lua"),
]
CAT_PRE = ("local C_BOOT,C_DSK,C_RAM,C_MB,C_PWR,C_FAN,C_DSP,C_KBD,C_SEC,C_NET,C_BEEP,C_DEV,C_OS "
           "= 1,2,3,4,5,6,7,8,9,10,11,12,13\n")
DB_HEAD = "local DB = {"
DB_TAIL = "\n-- ------------------------------- 状态"

LAYER_ORDER = ["POST", "BEEP", "BOOT", "BSOD", "DEVMGR", "SYS", "SELF"]
LAYER_TITLE = {
    "POST":   ("固件自检数字码", "联想 ThinkPad 官方 HMM 错误码表"),
    "BEEP":   ("蜂鸣组合码", "联想 HMM 蜂鸣表; 节奏按「长/短 + 停顿」对照"),
    "BOOT":   ("引导失败", "Microsoft 启动管理器 / NTSTATUS"),
    "BSOD":   ("蓝屏 STOP", "Microsoft Bug Check Code Reference"),
    "DEVMGR": ("设备管理器代码", "Microsoft CM_PROB_* 官方错误消息"),
    "SYS":    ("系统与应用错误码", "Microsoft Win32 / HRESULT"),
    "SELF":   ("自检与系统文字提示", "屏幕原话, 不是数字码"),
}
SEV_MAP = {"可自修": "现场可修", "必须送修": "必须返厂"}


def load_rows(path):
    import lupa
    src = open(path, encoding="utf-8").read()
    body = DB_HEAD + src.split(DB_HEAD, 1)[1].split(DB_TAIL, 1)[0]
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    DB = L.execute(CAT_PRE + body + "\nreturn DB")
    out = []
    for i in range(1, len(DB) + 1):
        r = DB[i]
        out.append([str(r[k]) for k in range(1, len(r) + 1)])
    CATS = ["C_BOOT", "C_DSK", "C_RAM", "C_MB", "C_PWR", "C_FAN",
            "C_DSP", "C_KBD", "C_SEC", "C_NET", "C_BEEP", "C_DEV", "C_OS"]
    for r in out:
        if r[3].isdigit():
            n = int(r[3])
            r[3] = CATS[n - 1] if 1 <= n <= len(CATS) else "C_MB"
    return out


# ---- 现有条目: code -> (layer, vendor, severity覆盖, verify, code覆盖, keys追加) ----
BEEP_VERIFY = ("① 先数清长/短与停顿, 按「节奏」对照而不是按次数 ② 断电重插内存并换槽复测 "
               "③ 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件")

META = {
 "0175": ("POST", "联想", "必须返厂",
          "① 断电静置后连开 3 次, 每次必报 ⇒ 固定故障 ② 进 BIOS 按 F9 载入默认再退出, 仍报 ⇒ 排除设置错误 ③ 重刷官方 BIOS 后消失 ⇒ 刷写问题; 仍报 ⇒ 主板 EEPROM", None, ""),
 "0176": ("POST", "联想", "必须返厂",
          "① 回想近期是否刷过非官方 BIOS/换过主板 ② F9+F10 后是否消失 ③ 反复出现记下完整报错送检, 不要反复尝试改安全设置", None, ""),
 "0177": ("POST", "联想", "必须返厂",
          "① 断电静置后复测是否必现 ② 是否改过超级密码 ③ 必现且与密码操作无关 ⇒ 主板级, 直接送检", None, ""),
 "0182": ("POST", "联想", "现场可修",
          "① F9+F10 后是否消失 ② 隔夜冷启动是否复现 ⇒ 指向 CMOS 电池 ③ 万用表测纽扣电池低于 2.8V 即换", None, "cmos dianchi"),
 "0183": ("POST", "联想", "现场可修",
          "① 进 BIOS 保存一次设置后是否消失 ② 关掉 Secure Boot 再打开是否复现 ③ 升级 BIOS 后复测", None, ""),
 "0185": ("POST", "联想", "现场可修",
          "① 进 BIOS 看 Boot 页启动项是否为空或乱序 ② F9 载入默认后是否恢复 ③ 恢复后仍看不到硬盘 ⇒ 转查硬盘识别", None, ""),
 "0187": ("POST", "联想", "必须返厂",
          "① 断电静置后复测 ② 是否与 0175/0188 同时出现 ③ 必现 ⇒ 主板 EEPROM, 不要自行拆机", None, ""),
 "0188": ("POST", "联想", "必须返厂",
          "① 是否换过主板或写过序列号 ② BIOS 中序列号是否为空/异常 ③ 必现需服务站重写, 现场无解", None, ""),
 "0189": ("POST", "联想", "必须返厂",
          "① 是否与 0188 同时出现 ② BIOS 中机型/序列号是否完整 ③ 必现 ⇒ 主板配置区损坏, 服务站重写", None, ""),
 "0191": ("POST", "联想", "现场可修",
          "① 是否接入过远程管理/批量部署 ② 断开管理端后是否消失 ③ 仍报错查 BIOS 安全设置是否被策略锁定", None, ""),
 "0192": ("POST", "联想", "必须返厂",
          "① 是否设置过安全芯片密码 ② 清除安全芯片后是否消失 ③ 必现且清不掉 ⇒ 安全芯片硬件故障", None, ""),
 "0199": ("POST", "联想", "现场可修",
          "① 确认是否输错过开机密码 ② 静置 30 分钟后再试 ③ 计数继续增长 ⇒ 有人反复试密码或恶意尝试", None, ""),
 "01C8": ("POST", "联想", "现场可修",
          "① 是否同时接了内置与外接调制解调器 ② 拔掉外接设备后是否消失 ③ BIOS 中禁用对应设备复测", None, ""),
 "01C9": ("POST", "联想", "现场可修",
          "① 是否装了双网卡或外接网卡 ② 拔掉外接网卡后是否消失 ③ BIOS 禁用板载网卡复测", None, ""),
 "01CA": ("POST", "联想", "现场可修",
          "① 是否装了双无线网卡 ② 拔掉外接无线网卡后是否消失 ③ BIOS 禁用无线复测", None, ""),
 "0200": ("POST", "联想", "需备件",
          "① 听硬盘是否转动、有无异响 ② BIOS 中是否识别到硬盘 ③ 换线换口后仍不识别 ⇒ 盘体故障, 先备份", None, ""),
 "0210": ("POST", "联想", "需备件",
          "① 外接键盘是否正常 ⇒ 说明内置排线/键盘 ② BIOS 中内置键盘是否可用 ③ 换键盘复测", None, ""),
 "0220": ("POST", "联想", "现场可修",
          "① 是否接了外接显示器 ② 换线/换显示器是否消失 ③ 仅内置屏报错 ⇒ 屏线或屏", None, ""),
 "0230": ("POST", "联想", "需备件",
          "① 内存单条交替测试 ② 换槽后是否消失 ③ 换条无效 ⇒ 按主板故障处理", None, ""),
 "0231": ("POST", "联想", "需备件",
          "① 记录报错 offset, 换条后 offset 是否变化 ② 单条单槽测试 ③ 换条不复现 ⇒ 内存条", None, ""),
 "0232": ("POST", "联想", "需备件",
          "① 优先测扩展槽内存 ② 拔掉扩展内存只留板载复测 ③ 换条验证", None, ""),
 "0250": ("POST", "联想", "现场可修",
          "① 万用表测纽扣电池电压, 低于 2.8V 即换 ② 换电池后 F9+F10 是否消失 ③ 新电池很快又报 ⇒ 主板漏电", None, ""),
 "0251": ("POST", "联想", "现场可修",
          "① F9+F10 后是否消失 ② 隔夜冷启动是否复现 ⇒ 电池 ③ 每次开机时间归零 ⇒ 电池或 RTC 电路", None, ""),
 "0252": ("POST", "联想", "现场可修",
          "① 是否改过密码 ② 重设密码后是否消失 ③ 反复出现且密码被清 ⇒ 安全芯片/CMOS 异常", None, ""),
 "0253": ("POST", "联想", "必须返厂",
          "① 是否刷过 BIOS 或改过 Secure Boot ② F9+F10 后是否恢复 ③ 必现 ⇒ EFI 变量区损坏, 需专用工具", None, ""),
 "0260": ("POST", "联想", "需备件",
          "① 是否超频或改过外频 ② 换 CMOS 电池后是否消失 ③ BIOS 默认后仍报 ⇒ 主板时钟电路", None, ""),
 "0270": ("POST", "联想", "需备件",
          "① BIOS 中时间是否能保存 ② 换 CMOS 电池后是否消失 ③ 仍不保存 ⇒ RTC 电路/主板", None, ""),
 "0271": ("POST", "联想", "现场可修",
          "① 系统时间是否正确 ② 改对后保存重启是否消失 ③ 每次都变慢 ⇒ 电池", None, ""),
 "2000": ("POST", "联想", "必须返厂",
          "① 是否摔过/震动后出现 ② 官方 APS 诊断工具是否报传感器异常 ③ 必现 ⇒ 传感器或主板", None, ""),
 "2100": ("POST", "联想", "需备件",
          "① BIOS 中是否识别 HDD0 ② 换线换口后是否识别 ③ 换盘测试, 仍不识别 ⇒ 接口/主板", None, ""),
 "2101": ("POST", "联想", "需备件",
          "① 确认第二盘位是否装盘 ② 换线换口复测 ③ 换盘交叉验证", None, ""),
 "2102": ("POST", "联想", "需备件",
          "① 确认 M.2 是否插到底、螺丝是否固定 ② 换 M.2 槽复测 ③ 换盘验证", None, ""),
 "2110": ("POST", "联想", "需备件",
          "① SMART 看 C5/C6 是否非零 ② 换线后是否仍报 ③ 低格或换盘", None, ""),
 "2111": ("POST", "联想", "需备件",
          "① 查 SSD 健康度与剩余寿命 ② 换槽复测 ③ 换盘验证", None, ""),
 "2112": ("POST", "联想", "需备件",
          "① 检查 M.2 散热与接触 ② 换槽复测 ③ 换盘验证", None, ""),
 "2200": ("POST", "联想", "必须返厂",
          "① BIOS 中机型/序列号是否为空 ② 是否换过主板 ③ 需服务站重写, 现场无解", None, ""),
 "2201": ("POST", "联想", "必须返厂",
          "① BIOS 中 UUID 是否全 0 或异常 ② 是否换过主板 ③ 需服务站重写", None, ""),
 "0135": ("POST", "联想", "现场可修",
          "① 开机风扇是否转 ② BIOS 中风扇转速是否可读 ③ 清灰换硅脂后是否消失; 转但报错 ⇒ 测速线", None, ""),
 "0162": ("POST", "联想", "现场可修",
          "① 是否改过 BIOS 设置或刷过 BIOS ② F9+F10 后是否消失 ③ 仍报 ⇒ NVRAM 区损坏", None, ""),
 "0164": ("POST", "联想", "现场可修",
          "① BIOS 识别容量与实装是否一致 ② 是否拔过内存/接触不良 ③ 重插后容量恢复即消失", None, ""),
 "1962": ("POST", "联想", "现场可修",
          "① BIOS 中是否识别硬盘 ② 启动项是否为空 ③ 进 PE 看分区表与引导文件是否完整", None, ""),

 "Fan error": ("SELF", "通用", "需备件",
          "① 开机风扇是否转、有无异响 ② 清灰换硅脂后是否消失 ③ 风扇转但仍报 ⇒ 测速线或风扇本体", None, "fengshan sanre"),
 "Thermal sensing error": ("SELF", "通用", "必须返厂",
          "① 进 BIOS 看 CPU 温度是否合理 ② 摸散热器是否发烫 ③ 读数异常(0/-1/255) ⇒ 温度传感器故障", None, "wendu chuanganqi"),
 "UEFI 变量存储将满": ("SELF", "联想", "现场可修",
          "① 是否装过大量驱动或安全启动项 ② BIOS 中清理无效启动项后是否消失 ③ 反复报满 ⇒ 需清 NVRAM", None, "uefi bianliang"),
 "适配器功率不足": ("SELF", "联想", "现场可修",
          "① 适配器铭牌功率与机型要求是否一致 ② 换原装适配器是否消失 ③ 接扩展坞时出现 ⇒ 坞供电不足", None, "shipeiqi gonglv"),
 "未知适配器": ("SELF", "联想", "需备件",
          "① 是否使用非原装或转接头 ② 换原装适配器是否消失 ③ 原装也报 ⇒ 充电口或主板检测电路", None, "shipeiqi"),
 "非原装电池不支持充电": ("SELF", "联想", "现场可修",
          "① 电池是否原装(查 FRU) ② 换原装电池是否消失 ③ 原装也报 ⇒ 电池通信或主板", None, "dianchi chongdian"),
 "非正品电池警告": ("SELF", "联想", "现场可修",
          "① BIOS 中能否读出电池序列号 ② 换原装电池复测 ③ 原装也报 ⇒ 主板电池接口", None, "dianchi"),
 "默认启动设备丢失": ("BOOT", "联想", "现场可修",
          "① BIOS 中是否识别硬盘 ② 启动顺序是否正确 ③ 进 PE 看引导分区是否存在", None, "yindao qidong"),
 "Boot Failed / 自动进 F12": ("BOOT", "联想", "现场可修",
          "① 是否接了 U 盘或移动硬盘 ② 拔掉后能否正常启动 ③ 仍失败 ⇒ 引导文件或硬盘", None, "boot failed f12"),
 "NTLDR is missing": ("BOOT", "通用", "现场可修",
          "① 是否装过双系统或改过分区 ② 用 PE 看根目录是否有 NTLDR 与 boot.ini ③ 修复引导, 无效再重装", None, "ntldr"),
 "PXE 网络启动": ("BOOT", "通用", "现场可修",
          "① BIOS 启动顺序是否把网络放到第一 ② 硬盘是否被识别 ③ 改回硬盘启动是否正常", None, "pxe wangluo qidong"),
 "Secure Boot 校验失败": ("BOOT", "通用", "现场可修",
          "① 是否装过非签名系统或驱动 ② 关掉 Secure Boot 能否启动(仅用于验证) ③ 恢复默认密钥后复测", None, "secure boot anquanqidong"),
 "BitLocker 恢复": ("SELF", "通用", "现场可修",
          "① 是否改过 BIOS/TPM/主板 ② 输入恢复密钥能否进入 ③ 进系统后 manage-bde -status 查保护状态", None, "bitlocker huifu miyao"),
 "开机密码提示": ("SELF", "联想", "现场可修",
          "① 分清是开机密码还是 supervisor 密码 ② 输入正确密码能否进入 ③ 忘记需服务站处理, 不要反复试(会锁定)", None, "mima password"),
 "硬盘密码提示": ("SELF", "联想", "必须返厂",
          "① 密码是否用户自己设过 ② 输入后能否进入 ③ 忘记则数据基本无法恢复, 需专业服务", None, "yingpan mima hdd password"),
 "0x7B": ("BSOD", "Microsoft", "现场可修",
          "① BIOS 磁盘模式(AHCI/RAID/IDE)是否与装系统时一致 ② 进 PE 看系统盘能否识别 ③ 一致且能识别 ⇒ 存储驱动或引导修复", "0x0000007B", "ahci raid"),
 "0xED": ("BSOD", "Microsoft", "需备件",
          "① 进 PE 看分区能否挂载 ② chkdsk /f 是否修复 ③ SMART 是否报坏道", "0x000000ED", "unmountable"),
 "0xC000000E": ("BOOT", "Microsoft", "现场可修",
          "① BIOS 是否识别启动盘 ② 换线换口复测 ③ PE 下看分区与引导项", None, "status_no_such_device"),
 "0xC0000225": ("BOOT", "Microsoft", "现场可修",
          "① 记下报错里的文件名 ② PE 下检查该文件是否存在/损坏 ③ 修复引导或替换文件", None, "status_not_found"),
 "0xC000000F": ("BOOT", "Microsoft", "现场可修",
          "① 启动盘是否在位 ② PE 下 diskpart 看分区 ③ 重建 BCD", None, "status_no_such_file"),
 "0xC0000001": ("BOOT", "Microsoft", "现场可修",
          "① 拔掉所有外接设备 ② 启动修复 ③ 内存单条测试", None, "status_unsuccessful"),
 "0xC0000098": ("BOOT", "Microsoft", "现场可修",
          "① PE 下 bcdedit 是否可读 ② bootrec /rebuildbcd ③ 仍失败则重建引导分区", None, "bcd"),
 "Bootmgr is missing": ("BOOT", "Microsoft", "现场可修",
          "① PE 看引导分区是否有 bootmgr ② bcdboot 重建 ③ 启动模式 UEFI/Legacy 与分区表是否匹配", None, "bootmgr"),
 "Operating System Not Found": ("BOOT", "通用", "现场可修",
          "① BIOS 是否识别硬盘 ② 分区表 GPT/MBR 与启动模式是否匹配 ③ PE 下看分区是否标记活动", None, "os not found"),
 "启动修复循环": ("BOOT", "Microsoft", "现场可修",
          "① 记下每次修复报的失败项 ② PE 下 sfc 与 chkdsk ③ 回滚最近更新与驱动", None, "qidong xiufu xunhuan"),
 "No bootable device": ("BOOT", "通用", "现场可修",
          "① BIOS 是否识别硬盘 ② 启动顺序 ③ 换线换口复测", None, "no bootable"),
 "开机直接进 BIOS": ("SELF", "通用", "现场可修",
          "① 是否识别到启动盘 ② 启动顺序与 CSM 设置 ③ 硬盘是否在位", None, "jin bios"),
 "开机黑屏仅电源灯亮": ("SELF", "通用", "需备件",
          "① 外接显示器是否出图 ② 内存重插/单条测试 ③ 听风扇与硬盘是否有动作", None, "heiping hei ping"),
 "一体机无显示": ("SELF", "通用", "必须返厂",
          "① 外接显示器测试 ② 内存重插 ③ 用手电照屏看是否有暗影", None, "yitiji wu xianshi"),
 "Battery not detected": ("SELF", "联想", "需备件",
          "① BIOS 中是否识别电池 ② 重插电池接口或换电池 ③ 只用适配器时是否正常", None, "dianchi wei jiance"),
 "Thermal Shutdown": ("SELF", "通用", "需备件",
          "① 出风口温度与风扇转速 ② 清灰换硅脂 ③ 仍过热 ⇒ 热管或风扇", None, "guore wendu"),
 "键盘无响应": ("SELF", "通用", "需备件",
          "① 外接键盘是否正常 ② BIOS 中内置键盘是否可用 ③ 重插排线或换键盘", None, "jianpan wu xiangying"),
}

DROP = {"CRITICAL_PROCESS_DIED", "BAD_SYSTEM_CONFIG_INFO"}

# 蜂鸣码处置等级 (按各条 advice 文本逐条判断, 不用关键词猜)
BEEP_SEV = {
    "1-1-1": "需备件", "1-1-4": "现场可修", "1-2-1": "需备件", "1-2-2": "现场可修",
    "1-2-3": "必须返厂", "1-3-3-1": "需备件", "1-4-1": "需备件", "2-1-1": "需备件",
    "3-1-1": "必须返厂", "3-1-1-3": "必须返厂", "3-1-2": "必须返厂", "3-1-3": "必须返厂",
    "3-3-4": "需备件", "3-4-2": "需备件", "3-4-3": "需备件", "4-2-1": "必须返厂",
    "4-2-2": "需备件", "4-2-3": "必须返厂", "4-2-4": "需备件", "4-3-4": "必须返厂",
    "4-4-3": "必须返厂", "1 长 2 短": "需备件", "5-5": "现场可修", "4 组 4 短": "现场可修",
}


def norm_code(s):
    """归一化去重键。必须带命名空间, 否则 Code 19 与 0x19 会撞车。"""
    t = s.strip().lower()
    if t.startswith("code"):
        t = t[4:].strip()
        t = t.lstrip("0") or "0"
        return "dev:" + t
    if t.startswith("0x"):
        t = t[2:]
        if re.fullmatch(r"[0-9a-f]+", t):
            return "hex:" + (t.lstrip("0") or "0")
        return "hex:" + t
    if re.fullmatch(r"[0-9a-f]+", t):
        # 裸十六进制串(无 0x 前缀)归 raw:, 与 Lua norm_code 逐字一致。
        # 曾经写成 hex: 导致 "01CA"(POST 自检) 与 "0x000001CA"(蓝屏) 撞键丢条。
        return "raw:" + (t.lstrip("0") or "0")
    return "txt:" + t


def norm_source(s):
    """source 只保留枚举代号; 现库里有 'KB 1315…' 这类带文档号的写法。"""
    t = s.strip()
    if t.startswith("经验"):
        return "经验"
    head = t.split()[0] if t.split() else ""
    return head if head in {"HMM", "KB", "SUP", "X390", "MS", "OEM", "经验"} else "KB"


def clean_keys(keys, en):
    toks = [t for t in keys.split() if re.fullmatch(r"[\x21-\x7e]+", t)]
    if len(toks) < 2:
        for w in re.findall(r"[A-Za-z0-9_]{2,}", en)[:4]:
            w = w.lower()
            if w not in toks:
                toks.append(w)
    return " ".join(toks)


def sort_key(row):
    code = row[0]
    m = re.fullmatch(r"0x([0-9A-Fa-f]{2,8})", code)
    if m:
        return (0, int(m.group(1), 16), "")
    m = re.fullmatch(r"Code (\d+)", code)
    if m:
        return (1, int(m.group(1)), "")
    if re.fullmatch(r"[0-9A-F]{4}", code):
        return (2, int(code, 16), "")
    m = re.search(r"(\d{4})", code)
    if m:
        return (2, int(m.group(1)), code)
    return (3, 0, code)


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


# 固件字库没有 ①②③ (真机上显示为空白), 统一改 ASCII 编号
CIRC = {"①": "1) ", "②": "2) ", "③": "3) ", "④": "4) ", "⑤": "5) "}
TEXT_FIELDS = (1, 2, 4, 5, 6, 7, 8, 11, 15)


def fix_text(s):
    for k, v in CIRC.items():
        s = s.replace(k, v)
    return re.sub(r"  +", " ", s)


def build():
    rows = load_rows(LUA_PATHS[0])
    by_key, order = {}, []

    def put(row):
        k = norm_code(row[0])
        if k not in by_key:
            order.append(k)
        by_key[k] = row

    for r in rows:
        code = r[0]
        if code in DROP:
            continue
        if len(r) >= 16:                       # 已迁移过: 原样保留, 幂等重跑
            out = list(r[:16])
            out[12] = clean_keys(out[12], out[1])
            put(out)
            continue
        meta = META.get(code)
        if not meta:
            if r[3] == "C_BEEP":
                layer, vendor = "BEEP", "联想"
                sev = BEEP_SEV.get(code) or SEV_MAP.get(r[9]) or "需备件"
                verify, code_new, extra = BEEP_VERIFY, None, ""
            else:
                raise SystemExit(f"缺少 META: {code}")
        else:
            layer, vendor, sev, verify, code_new, extra = meta
        out = list(r[:13])
        out[0] = code_new or code
        if sev is None:
            sev = SEV_MAP.get(r[9], "现场可修")
        out[9] = sev
        out[10] = norm_source(out[10])
        keys = out[12]
        if extra:
            keys = keys + " " + extra
        out[12] = clean_keys(keys, out[1])
        out += [layer, vendor, verify]
        put(out)

    new_rows = list(S_DEV.ROWS) + list(S_BSOD.ROWS) + list(S_BOOT.BOOT) + list(S_BOOT.SYS)
    for t in new_rows:
        if len(t) != 16:
            raise SystemExit(f"字段数错误 {len(t)}: {t[0]}")
        put(list(t))                           # 同码以种子为准, 便于修订
    merged = [by_key[k] for k in order]

    merged.sort(key=lambda r: (LAYER_ORDER.index(r[13]), sort_key(r)))
    for r in merged:
        for k in TEXT_FIELDS:
            r[k] = fix_text(r[k])
    return merged


def render(rows):
    # cat 必须是裸常量 C_*; 解析失败会变成 "None"/空, 直接写回会生成
    # `, None,` (Lua 里求值成 nil, 静默丢字段), 所以这里兜底并按层回填
    CAT_FALLBACK = {"DEVMGR": "C_DEV", "SYS": "C_OS", "BSOD": "C_OS"}
    out = [DB_HEAD, ""]
    cur = None
    for r in rows:
        if not re.fullmatch(r"C_[A-Z]+", r[3] or ""):
            r[3] = CAT_FALLBACK.get(r[13], "C_MB")
        if r[13] != cur:
            cur = r[13]
            name, src = LAYER_TITLE[cur]
            out.append("")
            out.append(f"-- ========== {cur} · {name} ({src}) ==========")
        out.append("")
        out.append('{ "%s", "%s", "%s", %s, "%s",' % (esc(r[0]), esc(r[1]), esc(r[2]), r[3], esc(r[4])))
        out.append('  "%s",' % esc(r[5]))
        out.append('  "%s",' % esc(r[6]))
        out.append('  "%s",' % esc(r[7]))
        out.append('  "%s", "%s", "%s", "%s",' % (esc(r[8]), esc(r[9]), esc(r[10]), esc(r[11])))
        out.append('  "%s", "%s", "%s",' % (esc(r[12]), esc(r[13]), esc(r[14])))
        out.append('  "%s" },' % esc(r[15]))
    out.append("")
    out.append("}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    rows = build()
    from collections import Counter
    print("总计 %d 条" % len(rows))
    print("分层:", dict(Counter(r[13] for r in rows)))
    print("处置:", dict(Counter(r[9] for r in rows)))
    print("来源:", dict(Counter(r[10] for r in rows)))
    if a.dry:
        return
    block = render(rows)
    bad = [r[0] for r in rows if not re.fullmatch(r"C_[A-Z]+", r[3] or "")]
    if bad or ", None," in block:
        raise SystemExit("拒绝写入: cat 字段非法 %s" % bad[:5])
    src = open(LUA_PATHS[0], encoding="utf-8").read()
    i = src.index(DB_HEAD)
    j = src.index(DB_TAIL, i)
    new = src[:i] + block + src[j:]
    open(LUA_PATHS[0], "w", encoding="utf-8").write(new)
    print("写入", LUA_PATHS[0], len(new), "字节")
    # 两份文件必须逐字一致: 以 store 版为准整份同步
    import shutil
    shutil.copyfile(LUA_PATHS[0], LUA_PATHS[1])
    print("同步", LUA_PATHS[1], os.path.getsize(LUA_PATHS[1]), "字节")


if __name__ == "__main__":
    main()
