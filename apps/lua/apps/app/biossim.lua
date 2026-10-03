--@lapp v1
--@app name=BIOS模拟器
--@app cat=系统
--@app desc=7 家品牌 BIOS 装机模拟: 装 Win11/Linux 要改哪几项, 先在掌机上练
--@app version=1.0
--@app icon=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP///////8AB////////4ANVVVVVVVVwA6qqqqqqqrADf///////cAPAAAAAAAGwA1sAAAAAAXAD2wADwAADsANAAAwgD/1wA9sAMBAwAb//+wBAEAABeAAgAEAQAAG4ADsAMeA/AXkAOwAOAGGBuAAgAAD/zP/YADsAAYIMEGn/v58BfgAfqAAw4fNB/+CoAD7AH0AAAKgAMIABQAAAqAAwgAFAAACoAD6AAU///KgAMIABQAAAqAAwgAFAAACoAD6AAU///KgAMIABQAAAqAA+gAFAAACr/7CAAUAAAKoAv///TP/8qqq6qqtMAACqALVVVU3+AKqquqqrQAAAqgC///9MAACqqr///0wAACoAsAARTbwAKqqwABFAAACqALAAEUwAAaqqsAARTAADKgCwAPFNgAYqqrABgUAADKoAsD8BQAQYq/+/5gFABjCoADAMAUAD46//8AgBQAHEp//wD/9AAAkgAAAAAUAACiAAAAABf//8IAAAAAEAAAAgAAAAAf///+AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
-- 图标自绘: blit_app_icon(x, y[, inv=true])
-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP///////8AB////////4ANVVVVVVVVwA6qqqqqqqrADf///////cAPAAAAAAAGwA1sAAAAAAXAD2wADwAADsANAAAwgD/1wA9sAMBAwAb//+wBAEAABeAAgAEAQAAG4ADsAMeA/AXkAOwAOAGGBuAAgAAD/zP/YADsAAYIMEGn/v58BfgAfqAAw4fNB/+CoAD7AH0AAAKgAMIABQAAAqAAwgAFAAACoAD6AAU///KgAMIABQAAAqAAwgAFAAACoAD6AAU///KgAMIABQAAAqAA+gAFAAACr/7CAAUAAAKoAv///TP/8qqq6qqtMAACqALVVVU3+AKqquqqrQAAAqgC///9MAACqqr///0wAACoAsAARTbwAKqqwABFAAACqALAAEUwAAaqqsAARTAADKgCwAPFNgAYqqrABgUAADKoAsD8BQAQYq/+/5gFABjCoADAMAUAD46//8AgBQAHEp//wD/9AAAkgAAAAAUAACiAAAAABf//8IAAAAAEAAAAgAAAAAf///+AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
local function blit_app_icon(x, y, inv)
  local d = _APPICON
  local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local buf, acc, nbits = {}, 0, 0
  for i = 1, #d do
    local ch = d:sub(i, i)
    if ch == "=" then break end
    local v = B64:find(ch, 1, true)
    if v then
      acc = acc * 64 + (v - 1); nbits = nbits + 6
      if nbits >= 8 then
        nbits = nbits - 8
        buf[#buf + 1] = math.floor(acc / (2 ^ nbits)) % 256
        acc = acc % (2 ^ nbits)
      end
    end
  end
  for row = 0, 63 do
    for col = 0, 63 do
      local idx = row * 8 + (col >> 3) + 1
      local byte = buf[idx] or 0
      local on = (math.floor(byte / (2 ^ (7 - (col % 8)))) % 2) == 1
      if on ~= (inv == true) then app.px(x + col, y + row, inv and app.WHITE or app.BLACK) end
    end
  end
end
-- ── 以上为注入前奏, 以下为应用原始代码 ──
-- biossim.lua — BIOS模拟器（掌机版 · 纯黑白 1bpp · 400x300）
--
-- 为什么做: 在电脑上改 BIOS 之前先在掌机上练一遍，把"装 Win11 要改哪几项"变成肌肉记忆。
-- 数据来源: 与 apps/biossim/biossim.html 的 @DATA 段**同一份数据**，由
--           tools/biossim_gen_lua.py 生成下面那一段。改选项/任务请改 HTML 再重跑脚本，
--           不要手改数据段（跑 tools/biossim_gen_lua.py --check 能查出是否不同步）。
--
-- 掌机按键优化（本文件的核心约束）: 掌机只有 ↑↓←→ + OK + BACK 六个键。
--   真机上的 F1(说明) / F8(检查) / F9(默认) / F10(保存) / ±(改值) 统统按不出来，所以:
--     · 改值   = OK（枚举项循环到下一个值）—— 不需要 ±
--     · 说明   = 每页末尾的普通行「说明」，不需要 F1
--     · 检查   = 每页末尾的普通行「检查结果 / 装机结论」，不需要 F8
--     · 默认值 = 每页末尾的普通行「载入默认值」，不需要 F9
--     · 保存   = 页面里的 Save Changes and Exit 行，不需要 F10
--     · 换菜单 = ← → ；退回上级 = BACK ；进子菜单 = OK
--   屏上**不显示按键提示条**，只有内容；要看键位进「说明」行。
--
-- 字体与配色: app.texts (ASCII 8x12) + app.zhs (汉字 12x12) → 一行 50 列、整屏 25 行。
--   反射式 1bpp 屏是**白纸黑字**（app.clear() 默认白底）：
--     正文 = 黑字白底；选中行 = 整行填黑 + 白字(inv=true)；标题/底栏 = 填黑 + 白字。
--   所有字符串只用 ASCII + 汉字；固件字库没有的符号已由生成脚本换成 ASCII。

-- @@DATA-BEGIN@@
-- ⚠ 本段由 tools/biossim_gen_lua.py 从 apps/biossim/biossim.html 的 @DATA 段生成，**不要手改**。
--    改选项/任务/菜单请改 HTML 那一段，然后重跑脚本；两边永远同源。
-- 显示顺序：照 HTML 里的书写顺序（第一个就是默认项）。
local BRAND_ORDER = {"award", "ami", "asus", "dell", "hp", "ibm", "huawei"}
local TASK_ORDER = {"win7-legacy", "win7-uefi", "win10", "win11", "ubuntu", "rst-to-ahci", "ide-to-ahci", "raid0-speed", "raid1-mirror", "raid5-3disk", "free"}
local ITEMS = {
    sataMode = {
      name = "SATA Mode",
      vals = {"IDE Mode", "AHCI Mode", "RAID Mode"},
      def = 1,
      help = "硬盘控制器工作模式。AHCI=标准 SATA 原生指令（装机首选）；IDE=老兼容模式（XP/老 Win7 免驱）；RAID (Intel RST)=Intel 快速存储，多用于 RAID/VMD，装 Win11 与 Linux 常需先改回 AHCI 或加载驱动。",
    },
    sataPort0 = {
      name = "SATA Port 0",
      vals = {"Not Present", "WDC WD10EZEX-08WN4A0", "Samsung SSD 860 EVO 500G", "Kingston A400 240G"},
      def = 2,
      help = "SATA 口识别到的盘。切 SATA Mode 后这里显示会变（RAID/VMD 下标准 AHCI 驱动看不到这块盘）。",
    },
    sataPort1 = {
      name = "SATA Port 1",
      vals = {"Not Present", "HL-DT-ST DVDRAM GH24NSD1"},
      def = 2,
      help = "第二个 SATA 口。",
    },
    nvme0 = {
      name = "NVMe SSD 0",
      vals = {"Not Present", "Samsung SSD 970 EVO Plus 1TB"},
      def = 2,
      help = "NVMe 盘。开了 VMD/RST 时 Windows 安装器需要加载对应驱动才能看到它。",
    },
    csm = {
      name = "CSM Support",
      vals = {"Enabled", "Disabled"},
      def = 2,
      help = "兼容支持模块。Enabled=兼容老系统的 Legacy 引导；装 Win7、老 PE、多引导常要开；装 Win10/11 纯 UEFI 一般关。",
    },
    bootFilter = {
      name = "Boot option filter",
      vals = {"UEFI and Legacy", "UEFI only", "Legacy only"},
      def = 1,
      help = "从哪类设备引导。装 Win7 传统方式选 Legacy only；纯 UEFI 装 Win10/11 选 UEFI only。",
    },
    storageOprom = {
      name = "Storage OpROM policy",
      vals = {"Legacy only", "UEFI only", "Do not launch"},
      def = 1,
      help = "存储扩展 ROM 用哪种方式加载。选了 Legacy only + CSM 关闭，会出现\"看不到硬盘/装不上\"。",
    },
    videoOprom = {
      name = "Video OpROM policy",
      vals = {"Legacy only", "UEFI only"},
      def = 1,
      help = "显卡 ROM 加载方式。",
    },
    secureBoot = {
      name = "Secure Boot",
      vals = {"Disabled", "Enabled"},
      def = 1,
      help = "安全启动。Win11 强制要求 Enabled；装 Win7（无签名驱动）必须 Disabled；Ubuntu 关掉最省事。",
    },
    secureBootMode = {
      name = "Secure Boot Mode",
      vals = {"Standard", "Custom"},
      def = 1,
      help = "Standard=用厂商内置密钥；Custom=可自行管理 PK/KEK/db（给自签名驱动用）。",
    },
    keyMgmt = {
      name = "Key Management",
      vals = {"Read Only", "Editable"},
      def = 1,
      help = "自定义密钥开关。",
    },
    tpm = {
      name = "Security Device Support",
      vals = {"Disabled", "Enabled"},
      def = 2,
      help = "TPM 总开关。Win11 要求 TPM 2.0 可用；Windows 的 BitLocker 与 TPM 绑定。",
    },
    tpmDevice = {
      name = "TPM Device Selection",
      vals = {"dTPM 2.0", "PTT (fTPM)"},
      def = 2,
      help = "用独立 TPM 芯片（dTPM）还是 Intel 平台信任技术（PTT/fTPM）。新主板多为 PTT。",
    },
    tpmState = {
      name = "TPM State",
      vals = {"Disabled", "Enabled"},
      def = 2,
      help = "本机 TPM 状态；改这里等于清/启用 TPM（可能让 BitLocker 进恢复模式）。",
    },
    amt = {
      name = "Intel(R) AMT",
      vals = {"Disabled", "Enabled"},
      def = 1,
      help = "Intel 主动管理（商用机带外管理）。开后可远程开关机/装系统（需配 MEBx），家用机没有这项。",
    },
    amtSol = {
      name = "AMT SOL / IDE-R",
      vals = {"Disabled", "Enabled"},
      def = 1,
      help = "AMT 的串口重定向（SOL）与远程挂载光驱（IDE-R），配合 AMT 用。",
    },
    amtPwd = {
      name = "Mebx Password",
      vals = {"Not Set", "Set"},
      def = 1,
      help = "进入 MEBx（Ctrl+P）要用的密码。",
    },
    vtd = {
      name = "VT-d",
      vals = {"Disabled", "Enabled"},
      def = 1,
      help = "直接 I/O 虚拟化，虚拟机直通设备用。",
    },
    vtx = {
      name = "Virtualization (VT-x / SVM Mode)",
      vals = {"Disabled", "Enabled"},
      def = 1,
      help = "VT-x。装 VMware/WSL2/Hyper-V、安卓模拟器都要开，否则 64 位虚拟机起不来。",
    },
    hyperThreading = {
      name = "Hyper-Threading Technology",
      vals = {"Disabled", "Enabled"},
      def = 2,
      help = "超线程。",
    },
    fastBoot = {
      name = "Fast Boot",
      vals = {"Disabled", "Enabled"},
      def = 2,
      help = "快速启动（跳过自检）。装系统/进 PE/U 盘启动失败时先把它关掉。",
    },
    quietBoot = {
      name = "Quiet Boot",
      vals = {"Disabled", "Enabled"},
      def = 2,
      help = "隐藏开机自检画面，只显示 Logo。",
    },
    netStack = {
      name = "Network Stack",
      vals = {"Disabled", "Enabled"},
      def = 1,
      help = "UEFI 网络栈（网卡 PXE/HTTP 引导的前提）。",
    },
    pxe = {
      name = "PXE Boot",
      vals = {"Disabled", "Enabled"},
      def = 1,
      help = "网卡引导（无盘工作站/网络装机用它）。",
    },
    legacyUsb = {
      name = "Legacy USB Support",
      vals = {"Auto", "Enabled", "Disabled"},
      def = 1,
      help = "DOS/PE 下能不能用 USB 键鼠。Disabled 时老 U 盘启动会出现\"键盘失灵\"。",
    },
    xhciHandoff = {
      name = "XHCI Hand-off",
      vals = {"Enabled", "Disabled"},
      def = 1,
      help = "把 USB3 控制权交给系统。装 Win7 必须 Enabled。",
    },
    wakeUsb = {
      name = "Wake on USB",
      vals = {"Disabled", "Enabled"},
      def = 1,
      help = "USB 唤醒。",
    },
    numLock = {
      name = "NumLock at boot",
      vals = {"On", "Off"},
      def = 2,
      help = "开机小键盘状态。",
    },
    chassisIntrusion = {
      name = "Chassis Intrusion",
      vals = {"Disabled", "Enabled"},
      def = 1,
      help = "机箱开盖检测（商用机）。",
    },
    bootMode = {
      name = "Boot Mode",
      vals = {"UEFI", "Legacy (CSM)"},
      def = 1,
      help = "整机引导模式，决定分区表（GPT/MBR）与引导文件位置。",
    },
    biosVer = {
      name = "BIOS Version",
      vals = {"2.19.1268 (08/12/2021)"},
      def = 1,
      ro = true,
    },
    ecVer = {
      name = "EC Firmware Ver",
      vals = {"1.14"},
      def = 1,
      ro = true,
    },
    cpuType = {
      name = "CPU Type",
      vals = {"Intel(R) Core(TM) i5-10400 CPU @ 2.90GHz"},
      def = 1,
      ro = true,
    },
    cpuSpeed = {
      name = "CPU Speed",
      vals = {"2900 MHz"},
      def = 1,
      ro = true,
    },
    memory = {
      name = "Total Memory",
      vals = {"16384 MB (DDR4-2666)"},
      def = 1,
      ro = true,
    },
    sysDate = {
      name = "System Date",
      vals = {"Fri 10/02/2026"},
      def = 1,
    },
    sysTime = {
      name = "System Time",
      vals = {"11:35:00"},
      def = 1,
    },
    prodName = {
      name = "Product Name",
      cn = "产品名称",
      vals = {"HP EliteBook 840 G8", "HUAWEI MateBook D15"},
      def = 1,
      ro = true,
    },
    prodNumber = {
      name = "Product Number",
      cn = "产品编号",
      vals = {"5F1A2PC#AB2"},
      def = 1,
      ro = true,
    },
    sysBoardId = {
      name = "System Board ID",
      cn = "主板识别码",
      vals = {"8823"},
      def = 1,
      ro = true,
    },
    serialNo = {
      name = "Serial Number",
      cn = "序列号",
      vals = {"5CG1234ABC"},
      def = 1,
      ro = true,
    },
    uuid = {
      name = "UUID Number",
      cn = "UUID 编号",
      vals = {"4C4C4544-0037-5110-8050-B5C04F4E4332"},
      def = 1,
      ro = true,
    },
    factoryOS = {
      name = "Factory Installed OS",
      cn = "出厂预装系统",
      vals = {"Windows 11 Home"},
      def = 1,
      ro = true,
    },
    batterySN = {
      name = "Primary Battery SN",
      cn = "主电池序列号",
      vals = {"01234 2024/06/11"},
      def = 1,
      ro = true,
    },
    sysLog = {
      name = "System Log",
      cn = "系统记录",
      vals = {"[Enter] 查看"},
      def = 1,
      act = "noop",
    },
    language = {
      name = "Language Support",
      cn = "语言",
      vals = {"English", "简体中文", "日文"},
      def = 1,
    },
    adminPwd = {
      name = "Administrator Password",
      cn = "管理员密码",
      vals = {"Not Set", "Set"},
      def = 1,
      help = "设了之后进 BIOS 要密码；忘记只能清 CMOS。",
    },
    powerOnPwd = {
      name = "Power-On Password",
      cn = "开机密码",
      vals = {"Not Set", "Set"},
      def = 1,
    },
    clearTpm = {
      name = "Clear TPM",
      cn = "清除 TPM",
      vals = {"[Enter] 清除"},
      def = 1,
      act = "noop",
      help = "清 TPM 会让 BitLocker 进入恢复模式；换主板/卖机器前才用。",
    },
    restoreSecDefaults = {
      name = "Restore Security Settings to Factory Defaults",
      cn = "安全设置恢复出厂",
      vals = {"[Enter] 恢复"},
      def = 1,
      act = "noop",
    },
    drivelock = {
      name = "DriveLock / Automatic DriveLock",
      cn = "硬盘锁",
      vals = {"Disabled", "Enabled"},
      def = 1,
    },
    sgx = {
      name = "Intel Software Guard Extensions (SGX)",
      cn = "Intel SGX",
      vals = {"Disabled", "Enabled"},
      def = 1,
    },
    sureStart = {
      name = "BIOS Sure Start",
      cn = "BIOS 自校验",
      vals = {"Enabled", "Disabled"},
      def = 1,
      help = "BIOS 被篡改时自动回滚到可信版本（HP 商用机）。",
    },
    sureRecover = {
      name = "HP Sure Recover",
      cn = "网络恢复",
      vals = {"Disabled", "Enabled"},
      def = 1,
    },
    thunderbolt = {
      name = "Thunderbolt Options",
      cn = "雷电选项",
      vals = {"[Enter]"},
      def = 1,
      act = "noop",
    },
    wlanRadio = {
      name = "Embedded WLAN Device Radio",
      cn = "内置无线网卡",
      vals = {"Enabled", "Disabled"},
      def = 1,
    },
    btRadio = {
      name = "Embedded Bluetooth Device",
      cn = "内置蓝牙",
      vals = {"Enabled", "Disabled"},
      def = 1,
    },
    fnLock = {
      name = "Fn Lock",
      cn = "Fn 键锁定",
      vals = {"Enabled", "Disabled"},
      def = 1,
    },
    usbCharging = {
      name = "USB Charging Port",
      cn = "USB 充电口",
      vals = {"Enabled", "Disabled"},
      def = 1,
    },
    batteryCare = {
      name = "Battery Care Function",
      cn = "电池养护",
      vals = {"50%", "80%", "100%"},
      def = 3,
    },
    f10F12Delay = {
      name = "F10 and f12 Delay (sec)",
      cn = "F10/f12 延时",
      vals = {"0", "5", "10", "15", "20"},
      def = 1,
    },
    cdromBoot = {
      name = "CD-ROM boot",
      cn = "光驱引导",
      vals = {"Enabled", "Disabled"},
      def = 1,
    },
    floppyBoot = {
      name = "Floppy boot",
      cn = "软驱引导",
      vals = {"Enabled", "Disabled"},
      def = 2,
    },
    netAdapterBoot = {
      name = "Internal Network Adapter boot",
      cn = "网卡引导",
      vals = {"Enabled", "Disabled"},
      def = 2,
    },
    uefiDriversNote = {
      name = "UEFI Drivers",
      cn = "UEFI 驱动",
      vals = {"[Enter] 管理"},
      def = 1,
      act = "noop",
    },
    aiOverclock = {
      name = "Ai Overclock Tuner",
      cn = "超频调优",
      vals = {"Auto", "Manual", "XMP I", "XMP II"},
      def = 1,
    },
    memFreq = {
      name = "Memory Frequency",
      cn = "内存频率",
      vals = {"Auto", "DDR4-2666MHz", "DDR4-3200MHz"},
      def = 1,
    },
    cpuRatio = {
      name = "CPU Core Ratio",
      cn = "CPU 倍频",
      vals = {"Auto", "Per Core", "Sync All Cores"},
      def = 1,
    },
    cpuTemp = {
      name = "CPU Temperature",
      cn = "CPU 温度",
      vals = {"45C / 113F"},
      def = 1,
      ro = true,
    },
    mbTemp = {
      name = "Motherboard Temperature",
      cn = "主板温度",
      vals = {"36C / 97F"},
      def = 1,
      ro = true,
    },
    fanSpeed = {
      name = "CPU Fan Speed",
      cn = "CPU 风扇转速",
      vals = {"1245 RPM"},
      def = 1,
      ro = true,
    },
    asusProfile = {
      name = "ASUS User Profile",
      cn = "用户配置档",
      vals = {"[Enter] 载入/保存"},
      def = 1,
      act = "noop",
    },
    spdInfo = {
      name = "ASUS SPD Information",
      cn = "SPD 信息",
      vals = {"[Enter] 查看"},
      def = 1,
      act = "noop",
    },
    sataModeRaid = {
      name = "SATA Mode",
      cn = "SATA 模式",
      vals = {"AHCI", "RAID"},
      def = 1,
    },
    nvmeRaidMode = {
      name = "NVMe RAID mode",
      cn = "NVMe RAID 模式",
      vals = {"Disabled", "Enabled"},
      def = 1,
    },
    raidType = {
      name = "RAID Type",
      cn = "RAID 类型",
      vals = {"RAID", "RAID5"},
      def = 1,
    },
    raidLevel = {
      name = "RAID Level",
      cn = "阵列级别",
      vals = {"RAID0", "RAID1", "RAID5", "RAID10"},
      def = 1,
      help = "阵列级别。RAID0=条带: 速度翻倍、容量全用、零冗余(坏一块全丢); RAID1=镜像: 两块互为副本, 容错一块, 容量减半; RAID5=分布式校验: 至少 3 块, 容错一块, 可用容量 n-1; RAID10=先镜像再条带, 至少 4 块, 兼顾速度与容错。",
    },
    raidStripe = {
      name = "Stripe Size",
      cn = "条带大小",
      vals = {"16K", "64K", "128K"},
      def = 2,
      help = "条带大小 = 每块盘上连续写多少数据后换下一块。大文件顺序读写(视频剪辑/备份)用 128K; 数据库、虚拟化这类随机小 IO 用 16K~64K。",
    },
    raidDisks = {
      name = "Select Disks",
      cn = "选择硬盘",
      vals = {"未选择", "已选 2 块", "已选 3 块"},
      def = 1,
      help = "把要组阵列的盘勾上。RAID0/RAID1 选 2 块, RAID5 选 3 块; 容量不一致时按**最小**那块算。",
    },
    raidCreate = {
      name = "Create Volume",
      cn = "创建阵列",
      vals = {"未创建", "已创建"},
      def = 1,
      help = "确认创建阵列。!! 创建会**清空**所选硬盘上的全部数据, 先备份 !!",
    },
    raidBootable = {
      name = "Mark Bootable",
      cn = "设为可引导",
      vals = {"否", "是"},
      def = 1,
      help = "把阵列标记为可引导设备, 否则装系统时在启动项里找不到它。",
    },
    raidCache = {
      name = "Write-back Cache",
      cn = "写回缓存",
      vals = {"Disabled", "Enabled"},
      def = 2,
      help = "阵列写缓存。开了性能好但掉电可能丢最后一小段数据; 有 UPS 或只做临时盘可以开。",
    },
    storageHierarchy = {
      name = "Storage Hierarchy",
      cn = "存储层次",
      vals = {"Disabled", "Enabled"},
      def = 1,
    },
    tpmSpec = {
      name = "TPM 2.0 UEFI Spec Version",
      cn = "TPM 2.0 规范版本",
      vals = {"TCG_1_2", "TCG_2"},
      def = 2,
      help = "Win11 要求 TCG_2（即 TPM 2.0）。设成 TCG_1_2 会被 Win11 判为不满足。",
    },
    tpmIface = {
      name = "TPM 2.0 InterfaceType",
      cn = "TPM 接口类型",
      vals = {"CRB", "TIS"},
      def = 1,
    },
    ftpmSwitch = {
      name = "AMD fTPM switch",
      cn = "AMD fTPM 开关",
      vals = {"Enabled", "Disabled"},
      def = 1,
    },
    vtxLabel = {
      name = "SVM Mode (AMD) / VT-x (Intel)",
      cn = "虚拟化 (SVM/VT-x)",
      vals = {"Disabled", "Enabled"},
      def = 1,
    },
    netStackCfg = {
      name = "Network Stack",
      cn = "网络栈",
      vals = {"Disabled", "Enabled"},
      def = 1,
    },
    noteFav = {
      name = "(F3 可把常用项加到这里)",
      cn = "(F3 添加常用项)",
      vals = {""},
      def = 1,
      ro = true,
    },
    noteOc = {
      name = "(超频有风险: 改完先跑稳定性测试)",
      cn = "(超频有风险)",
      vals = {""},
      def = 1,
      ro = true,
    },
    supervisorPwd = {
      name = "Supervisor Password",
      vals = {"Not Installed", "Installed"},
      def = 1,
      help = "管理员密码。设了之后进 BIOS 要输密码。",
    },
    userPwd = {
      name = "User Password",
      vals = {"Not Installed", "Installed"},
      def = 1,
      help = "普通用户密码。",
    },
    vmd = {
      name = "VMD Controller",
      vals = {"Disabled", "Enabled"},
      def = 1,
      help = "Volume Management Device（Intel 11 代以后）。开了 Linux/旧 Win 看不到盘。",
    },
    loadDefaults = {
      name = "Load Optimal Defaults",
      vals = {"[Enter] 载入出厂默认"},
      def = 1,
      act = "defaults",
      help = "把整页设置恢复出厂。装系统卡住时先恢复默认再重设。",
    },
    safeBoot = {
      name = "（流程项）先以安全模式启动一次",
      vals = {"未做", "已做"},
      def = 1,
      hidden = true,
    },
    regFix = {
      name = "（流程项）先改注册表 iaStorV/iaStorAC Start=0",
      vals = {"未做", "已做"},
      def = 1,
      hidden = true,
    },
    msahci = {
      name = "（流程项）先改注册表 msahci Start=0",
      vals = {"未做", "已做"},
      def = 1,
      hidden = true,
    },
    iastor = {
      name = "（流程项）先改注册表 iaStorV Start=0",
      vals = {"未做", "已做"},
      def = 1,
      hidden = true,
    },
    rebootFirst = {
      name = "（流程项）先正常重启一次",
      vals = {"未做", "已做"},
      def = 1,
      hidden = true,
    },
  }
local BOOT_DEVICES = {
    "Windows Boot Manager (Samsung SSD 860 EVO)",
    "UEFI: Kingston DataTraveler 3.0 (U盘)",
    "SATA0: Samsung SSD 860 EVO 500G",
    "SATA1: HL-DT-ST DVDRAM GH24NSD1",
    "Network: UEFI PXE IPv4",
  }
local BRANDS = {
    award = {
      label = "Award / Phoenix（90 年代台式机-蓝底红选）",
      theme = "mono",
      layout = "leftmenu",
      short = "Award",
      home = "Standard CMOS Features",
      ov = { prodName = "Generic ATX i815EP System", serialNo = "AWARD0001234" },
      title = "CMOS SETUP UTILITY - AWARD SOFTWARE, INC.",
      thint = "ESC:Quit  F10:Save",
      footer = "ESC:Quit   ^v-><-:Select Item   F10:Save & Exit   F9:Load Defaults   F1:Help",
      menus = {
        "Standard CMOS Features",
        "Advanced BIOS Features",
        "Advanced Chipset Features",
        "Integrated Peripherals",
        "Power Management Setup",
        "PnP/PCI Configurations",
        "PC Health Status",
        "Load Fail-Safe Defaults",
        "Save & Exit Setup",
      },
    },
    ami = {
      label = "AMI Aptio V（通用台机主板-灰底深蓝）",
      theme = "mono",
      layout = "topmenu",
      short = "AMI",
      home = "Main",
      ov = {
        biosVer = "2.21.1203 (03/15/2024)",
        prodName = "AMI Aptio V Reference Board",
        serialNo = "AMIA0001234",
      },
      title = "Aptio Setup Utility - Copyright (C) 2024 American Megatrends",
      thint = "F10:Save & Exit",
      footer = "F1 Help   ^v Select   <--> Screen   +/- Change   Enter Select   F9 Defaults   F10 Save & Exit   ESC Exit",
      menus = {"Main", "Advanced", "Security", "Boot", "Save & Exit"},
    },
    asus = {
      label = "华硕 ASUS（Aptio V - Ai Tweaker）",
      theme = "mono",
      layout = "leftmenu",
      short = "华硕",
      home = "Main",
      ov = {
        biosVer = "3004 (07/12/2024)",
        prodName = "ASUS PRIME Z590-A",
        serialNo = "M3M0AS012345",
      },
      title = "ASUS UEFI BIOS Utility - Advanced Mode",
      thint = "(C) ASUSTeK COMPUTER INC.",
      footer = "F1 Setup   F5 Reset   F7 EZ Mode   F10 Save & Exit   ^v Select   Enter Select",
      menus = {"My Favorites", "Main", "Ai Tweaker", "Advanced", "Monitor", "Boot", "Tool", "Exit"},
    },
    dell = {
      label = "戴尔 Dell（OptiPlex / Latitude）",
      theme = "mono",
      layout = "leftmenu",
      short = "戴尔",
      home = "General",
      ov = {
        biosVer = "1.28.0 (06/11/2024)",
        prodName = "OptiPlex 7090",
        serialNo = "J4K2L83",
        ecVer = "1.09",
      },
      ovn = {
        sataMode = "SATA Operation",
        secureBoot = "Secure Boot Enable",
        vtx = "Virtualization",
        loadDefaults = "Restore Settings",
        prodName = "System Model",
        serialNo = "Service Tag",
      },
      title = "Dell BIOS Setup   |   Settings   System Information",
      thint = "F10:Save",
      footer = "^v Select   Enter Select   +/- Change   Esc Exit   F9 Defaults   F10 Save & Exit",
      menus = {
        "General",
        "System Configuration",
        "Video",
        "Security",
        "Secure Boot",
        "Performance",
        "Power Management",
        "POST Behavior",
        "Virtualization Support",
        "Maintenance",
        "System Logs",
        "Exit",
      },
    },
    hp = {
      label = "惠普 HP（F10 Setup Utility）",
      theme = "mono",
      layout = "topmenu",
      short = "惠普",
      home = "Main",
      ov = {
        biosVer = "2.19.1268 (08/12/2021)",
        prodName = "HP EliteBook 840 G8",
        serialNo = "5CG1234ABC",
        prodNumber = "5F1A2PC#AB2",
        uuid = "4C4C4544-0037-5110-8050",
        factoryOS = "Windows 11 Home",
        sysBoardId = "8823",
        batterySN = "01234 2024/06/11",
      },
      ovn = {
        sataMode = "SATA Emulation",
        tpm = "Embedded Security Device",
        loadDefaults = "Apply Factory Defaults",
      },
      title = "HP BIOS Setup Utility",
      thint = "(C) Copyright 1985-2024 HP Development Company, L.P.",
      footer = "F1 Help   Esc Exit   ^v Select   Enter Select   F9 Defaults   F10 Save & Exit",
      menus = {"Main", "Security", "Advanced", "UEFI Drivers", "Exit"},
    },
    ibm = {
      label = "联想 ThinkPad（IBM 风格-黑底蓝条）",
      theme = "mono",
      layout = "leftmenu",
      short = "联想",
      home = "Config",
      ov = {
        biosVer = "N2HET57W (1.44)",
        prodName = "ThinkPad X1 Carbon Gen 9",
        serialNo = "PF2ABCDE",
        ecVer = "1.14",
      },
      ovn = {
        sataMode = "SATA Mode",
        loadDefaults = "Setup Defaults",
        secureBoot = "Secure Boot",
        bootMode = "Boot Mode",
        cpuType = "CPU Type",
      },
      title = "ThinkPad Setup",
      thint = "(C) Copyright 1985-2024 Lenovo",
      footer = "^v Select   Enter Select   +/- Change   Esc Exit   F9 Defaults   F10 Save & Exit",
      menus = {"Config", "Date/Time", "Security", "Startup", "Restart", "Network", "Display"},
    },
    huawei = {
      label = "华为 HUAWEI（MateBook - 中文界面）",
      theme = "mono",
      layout = "topmenu",
      cn = true,
      short = "华为",
      home = "信息概览",
      ov = {
        biosVer = "1.32 (06/20/2024)",
        prodName = "HUAWEI MateBook X Pro",
        serialNo = "H3XPMB0123456",
        ecVer = "1.21",
        cpuType = "Intel(R) Core(TM) i7-1165G7 @ 2.80GHz",
        cpuSpeed = "2800 MHz",
        memory = "16384 MB (LPDDR4X-4266)",
      },
      title = "HUAWEI MateBook UEFI Setup Utility",
      thint = "F10 保存并退出",
      footer = "F1 帮助   Esc 退出   ^v 选择   Enter 确认   F9 默认   F10 保存并退出",
      menus = {"信息概览", "安全设置", "启动配置", "性能调节", "退出"},
    },
  }
local PAGES = {
    award = {
      ["Standard CMOS Features"] = {"sysDate", "sysTime", "sataPort0", "sataPort1", "nvme0", "memory"},
      ["Advanced BIOS Features"] = {"bootMode", "bootFilter", "fastBoot", "quietBoot", "legacyUsb", "xhciHandoff", "numLock"},
      ["Advanced Chipset Features"] = {"vtx", "vtd", "hyperThreading", "@Serial ATA Controller 0 Settings"},
      ["Integrated Peripherals"] = {"sataMode", "legacyUsb", "xhciHandoff", "wakeUsb", "@RAID Configuration"},
      ["Power Management Setup"] = {"wakeUsb", "chassisIntrusion"},
      ["PnP/PCI Configurations"] = {"vtd", "storageOprom", "videoOprom"},
      ["PC Health Status"] = {"cpuType", "memory"},
      ["Load Fail-Safe Defaults"] = {"loadDefaults"},
      ["Save & Exit Setup"] = {"*bootOrder", "*saveExit", "*discardExit"},
    },
    ami = {
      Main = {"biosVer", "ecVer", "cpuType", "cpuSpeed", "memory", "sysDate", "sysTime"},
      Advanced = {
        "@SATA Configuration",
        "@USB Configuration",
        "@CSM Configuration",
        "@Intel(R) AMT Configuration",
        "@Trusted Computing",
        "@CPU Configuration",
      },
      Security = {
        "supervisorPwd",
        "userPwd",
        "secureBoot",
        "secureBootMode",
        "keyMgmt",
        "tpm",
        "tpmDevice",
        "tpmState",
        "chassisIntrusion",
      },
      Boot = {"bootMode", "bootFilter", "fastBoot", "quietBoot", "*bootOrder", "netStack", "pxe"},
      ["Save & Exit"] = {"*saveExit", "*discardExit", "loadDefaults"},
    },
    asus = {
      ["My Favorites"] = {"biosVer", "cpuType", "memory", "noteFav"},
      Main = {
        "biosVer",
        "sysDate",
        "sysTime",
        "language",
        "cpuType",
        "cpuSpeed",
        "memory",
        "sataPort0",
        "nvme0",
      },
      ["Ai Tweaker"] = {"aiOverclock", "memFreq", "cpuRatio", "noteOc"},
      Advanced = {
        "@Serial ATA Controller 0 Settings",
        "@SATA Configuration",
        "@USB Configuration",
        "@CSM (Compatibility Support Module)",
        "@Trusted Computing",
        "@AMD fTPM configuration",
        "@CPU Configuration",
        "@Network Stack Configuration",
      },
      Monitor = {"cpuTemp", "mbTemp", "fanSpeed"},
      Boot = {"bootMode", "fastBoot", "@RAID Configuration", "*bootOrder", "storageOprom"},
      Tool = {"asusProfile", "spdInfo"},
      Exit = {"*saveExit", "*discardExit", "loadDefaults"},
    },
    dell = {
      General = {"biosVer", "ecVer", "sysDate", "sysTime", "memory", "prodName", "serialNo"},
      ["System Configuration"] = {
        "sataMode",
        "@RAID Configuration",
        "sataPort0",
        "sataPort1",
        "nvme0",
        "legacyUsb",
        "fastBoot",
      },
      Video = {"videoOprom"},
      Security = {"adminPwd", "powerOnPwd", "tpm", "tpmState", "clearTpm", "chassisIntrusion", "amt"},
      ["Secure Boot"] = {"secureBoot", "secureBootMode", "keyMgmt"},
      Performance = {"vtx", "vtd", "hyperThreading"},
      ["Power Management"] = {"wakeUsb", "batteryCare"},
      ["POST Behavior"] = {"quietBoot", "numLock", "fastBoot"},
      ["Virtualization Support"] = {"vtx", "vtd"},
      Maintenance = {"loadDefaults", "sysLog"},
      ["System Logs"] = {"sysLog"},
      Exit = {"*saveExit", "*discardExit"},
    },
    hp = {
      Main = {
        "sysTime",
        "sysDate",
        "prodName",
        "prodNumber",
        "sysBoardId",
        "cpuType",
        "memory",
        "biosVer",
        "serialNo",
        "uuid",
        "factoryOS",
        "batterySN",
        "sysLog",
      },
      Security = {
        "adminPwd",
        "powerOnPwd",
        "sgx",
        "tpm",
        "tpmState",
        "clearTpm",
        "restoreSecDefaults",
        "secureBoot",
        "secureBootMode",
        "drivelock",
      },
      Advanced = {
        "language",
        "@Boot Options",
        "@System Options",
        "@Built-in Device Options",
        "@Port Options",
        "@Power Management Options",
        "@Remote Management Options",
        "@RAID Configuration",
        "sureStart",
        "sureRecover",
        "thunderbolt",
      },
      ["UEFI Drivers"] = {"uefiDriversNote"},
      Exit = {"*saveExit", "*discardExit", "loadDefaults"},
    },
    ibm = {
      Config = {
        "@Network",
        "@Display",
        "@RAID Configuration",
        "nvme0",
        "sataMode",
        "legacyUsb",
        "xhciHandoff",
        "fastBoot",
        "fnLock",
        "usbCharging",
      },
      ["Date/Time"] = {"sysDate", "sysTime"},
      Security = {
        "supervisorPwd",
        "userPwd",
        "secureBoot",
        "secureBootMode",
        "tpm",
        "tpmDevice",
        "tpmState",
        "clearTpm",
        "drivelock",
        "amt",
        "amtSol",
        "amtPwd",
        "vtx",
        "vtd",
      },
      Startup = {"bootMode", "bootFilter", "*bootOrder", "pxe", "fastBoot"},
      Restart = {"loadDefaults", "*saveExit", "*discardExit"},
      Network = {"wlanRadio", "btRadio", "wakeUsb", "netStack"},
      Display = {"videoOprom", "quietBoot"},
    },
    huawei = {
      ["信息概览"] = {
        "prodName",
        "biosVer",
        "cpuType",
        "cpuSpeed",
        "memory",
        "nvme0",
        "serialNo",
        "sysDate",
        "sysTime",
      },
      ["安全设置"] = {
        "secureBoot",
        "secureBootMode",
        "tpm",
        "tpmDevice",
        "tpmState",
        "clearTpm",
        "supervisorPwd",
        "drivelock",
        "chassisIntrusion",
      },
      ["启动配置"] = {
        "bootMode",
        "bootFilter",
        "fastBoot",
        "@RAID Configuration",
        "*bootOrder",
        "netStack",
        "pxe",
      },
      ["性能调节"] = {"vtx", "hyperThreading", "quietBoot"},
      ["退出"] = {"*saveExit", "*discardExit", "loadDefaults"},
    },
  }
local SUBPAGES = {
    ["Serial ATA Controller 0 Settings"] = {"sataMode", "sataPort0", "sataPort1", "@SATA Configuration"},
    ["SATA Configuration"] = {
      "sataModeRaid",
      "nvmeRaidMode",
      "raidType",
      "@RAID Configuration",
      "sataPort0",
      "sataPort1",
      "nvme0",
    },
    ["RAID Configuration"] = {
      "sataModeRaid",
      "raidLevel",
      "raidDisks",
      "raidStripe",
      "raidCache",
      "raidCreate",
      "raidBootable",
      "vmd",
    },
    ["USB Configuration"] = {"legacyUsb", "xhciHandoff", "wakeUsb"},
    ["CSM Configuration"] = {"csm", "bootFilter", "storageOprom", "videoOprom"},
    ["CSM (Compatibility Support Module)"] = {"csm", "bootFilter", "storageOprom", "videoOprom"},
    ["Intel(R) AMT Configuration"] = {"amt", "amtSol", "amtPwd", "netStack"},
    ["Trusted Computing"] = {"tpm", "tpmDevice", "tpmState", "tpmSpec", "tpmIface", "storageHierarchy", "clearTpm"},
    ["AMD fTPM configuration"] = {"ftpmSwitch", "tpmState", "clearTpm"},
    ["CPU Configuration"] = {"cpuType", "cpuSpeed", "vtx", "vtd", "hyperThreading"},
    ["Network Stack Configuration"] = {"netStackCfg", "pxe", "netAdapterBoot"},
    ["Boot Options"] = {"f10F12Delay", "cdromBoot", "floppyBoot", "netAdapterBoot", "*bootOrder"},
    ["System Options"] = {"vtx", "vtd", "hyperThreading", "fastBoot", "chassisIntrusion"},
    ["Built-in Device Options"] = {"wlanRadio", "btRadio", "wakeUsb", "fnLock"},
    ["Port Options"] = {"legacyUsb", "xhciHandoff", "usbCharging"},
    ["Power Management Options"] = {"wakeUsb", "batteryCare"},
    ["Remote Management Options"] = {"amt", "amtSol", "amtPwd"},
    Network = {"wlanRadio", "btRadio", "wakeUsb", "netStack"},
    Display = {"videoOprom", "quietBoot"},
  }
local TASKS = {
    ["win7-legacy"] = {
      mode = "os",
      label = "给老机器装 Windows 7（传统 Legacy 方式，免驱最省心）",
      brief = "目标：让安装 U 盘以 Legacy 方式引导，且 SATA 用安装光盘自带驱动的模式；Win7 没有 Secure Boot 签名。",
      rules = {
        {"bootMode", 2, "Boot Mode 要 Legacy（Win7 传统装法用 MBR 分区表）"},
        {"bootFilter", 3, "Boot option filter 选 Legacy only，才认 U 盘的传统引导"},
        {"secureBoot", 1, "Secure Boot 必须关（Win7 的驱动无签名）"},
        {"sataMode", 1, "SATA 用 IDE：Win7 原版不带 AHCI 驱动，不加载驱动会 0x0000007B 蓝屏"},
        {"legacyUsb", 2, "Legacy USB Support 开：PE 里 USB 键鼠才动得了"},
        {"xhciHandoff", 1, "XHCI Hand-off 开：否则 Win7 装完 USB 口全废"},
      },
      extra = {"装完想升 AHCI：先在系统里改注册表 msahci/iaStorV 的 Start=0，重启后再把 BIOS 改 AHCI（见任务\"IDE->AHCI 不蓝屏\"）。"},
    },
    ["win7-uefi"] = {
      mode = "os",
      label = "给新主板装 Windows 7（UEFI + CSM 方式，GPT 分区）",
      brief = "目标：UEFI 引导但兼容 Legacy 设备；Secure Boot 关；SATA 用 AHCI 但需注入驱动或先用 IDE。",
      rules = {
        {"bootMode", 1, "Boot Mode 保持 UEFI（GPT 分区表）"},
        {"csm", 1, "CSM Support 开：Win7 需要 Legacy 视频/存储 ROM 兼容层"},
        {"storageOprom", 1, "Storage OpROM policy 要 Legacy only（配 CSM 用）"},
        {"secureBoot", 1, "Secure Boot 关"},
        {"sataMode", 2, "SATA 用 AHCI（要先给镜像注入 AHCI 驱动，否则 7B 蓝屏）"},
        {"vtx", 2, "VT-x 开：装完跑虚拟机/沙盘用得上"},
      },
      extra = {"UEFI 装 Win7 还要在硬盘上留一个 FAT32 的 ESP 分区，并把 U 盘做成 UEFI:NTFS 或先用 WinPE 分区。"},
    },
    win10 = {
      mode = "os",
      label = "装 Windows 10（纯 UEFI，推荐路线）",
      brief = "目标：纯 UEFI + GPT；Secure Boot 可开可关；SATA 必须 AHCI。",
      rules = {
        {"bootMode", 1, "Boot Mode = UEFI"},
        {"csm", 2, "CSM Support 关（纯 UEFI，避免装到 MBR 盘上）"},
        {"bootFilter", 2, "Boot option filter = UEFI only"},
        {"sataMode", 2, "SATA = AHCI（Win10 原生支持）"},
        {"secureBoot", 2, "Secure Boot 建议开（Win10 支持良好）"},
        {"tpm", 2, "TPM 开：BitLocker 与后续升 Win11 都要"},
      },
      extra = {"Win10 也能用 Legacy+MBR 装，但升级 Win11 时会被 GPT/UEFI 卡住，新机直接纯 UEFI。"},
    },
    win11 = {
      mode = "os",
      label = "装 Windows 11（硬性要求最多）",
      brief = "目标：UEFI + Secure Boot 开 + TPM 2.0 开 + SATA 必须是 AHCI（RST/VMD 会被\"找不到磁盘\"卡住）。",
      rules = {
        {"bootMode", 1, "Boot Mode 必须 UEFI（Win11 不支持 Legacy）"},
        {"csm", 2, "CSM 关（纯 UEFI）"},
        {"secureBoot", 2, "Secure Boot 必须 Enabled（Win11 硬性要求）"},
        {"tpm", 2, "Security Device Support = Enabled"},
        {"tpmDevice", 2, "选 PTT/fTPM 或 dTPM 2.0 -- Win11 要 TPM 2.0"},
        {"tpmState", 2, "TPM State 要 Enabled"},
        {"sataMode", 2, "SATA 必须 AHCI：RAID/RST/VMD 会让安装器看不到盘"},
        {"fastBoot", 2, "装完可开 Fast Boot（装的时候建议先关）"},
      },
      extra = {"本机是 RAID/VMD 又不想改：安装时用\"加载驱动程序\"喂 Intel RST/VMD 驱动，或在 BIOS 里把 VMD 关掉、改用 SATA AHCI。"},
    },
    ubuntu = {
      mode = "os",
      label = "装 Linux（Ubuntu / Debian 等）",
      brief = "目标：AHCI + 关 RST/VMD（否则安装器看不到硬盘）；Secure Boot 可关最省事。",
      rules = {
        {"sataMode", 2, "SATA 必须 AHCI：开了 RST/VMD，Linux 安装器会\"没有可用磁盘\""},
        {"vmd", 1, "有 VMD 选项时关掉（Intel 11 代以后常见）"},
        {"secureBoot", 1, "Secure Boot 建议关：装第三方驱动（NVIDIA/WiFi）少一次签名折腾"},
        {"bootMode", 1, "UEFI 安装（Ubuntu 的 UEFI 引导更稳）"},
        {"csm", 2, "CSM 关"},
        {"vtx", 2, "VT-x 开：Docker/虚拟机要用"},
      },
      extra = {"要在 Win 旁边双系统：先在 Windows 里关\"快速启动\"与\"休眠\"，否则 Linux 只能只读挂载 NTFS。"},
    },
    ["rst-to-ahci"] = {
      mode = "os",
      label = "已装好系统：把 RST/RAID 改成 AHCI（不重装）",
      brief = "目标：机器用了 Intel RST/VMD，想换成标准 AHCI。直接改 BIOS 会 INACCESSIBLE_BOOT_DEVICE 蓝屏。",
      rules = {
        {"sataMode", 2, "最终目标：SATA = AHCI"},
        {"safeBoot", 2, "先在 Windows 里以\"安全模式\"启动一次（让系统加载标准 AHCI 驱动）"},
        {"regFix", 2, "或先在注册表把 iaStorV/iaStorAC 的 Start 改为 0，再重启进 BIOS 改 AHCI"},
        {"vmd", 1, "如有 VMD 项，一并 Disabled"},
      },
      extra = {"顺序很重要：先让 Windows 认识 AHCI 驱动 -> 再改 BIOS。反过来就是蓝屏开不了机。"},
    },
    ["ide-to-ahci"] = {
      mode = "os",
      label = "已装好 Win7/XP：IDE 改 AHCI（不蓝屏的做法）",
      brief = "目标：老系统从 IDE 升到 AHCI，拿到 NCQ 与热插拔；关键是先让系统里 AHCI 驱动处于启用状态。",
      rules = {
        {"sataMode", 2, "最终目标：SATA = AHCI"},
        {"msahci", 2, "注册表 HKLM\\SYSTEM\\...\\Services\\msahci 的 Start 改为 0"},
        {"iastor", 2, "Intel 平台同时把 iaStorV 的 Start 改为 0（更保险）"},
        {"rebootFirst", 2, "改完注册表先正常重启一次，再进 BIOS 改 AHCI"},
      },
      extra = {"XP 没有 msahci，要用\"合并驱动\"的方式先在设备管理器里装 Intel AHCI 驱动，再改 BIOS。"},
    },
    ["raid0-speed"] = {
      label = "阵列-双盘 RAID0 提速",
      mode = "raid",
      brief = "方案要求: 两块同容量 SSD 组 RAID0 当剪辑临时盘 -- 要最快的顺序读写; 不要冗余(素材另有备份)。",
      rules = {
        {"sataModeRaid", 2, "SATA Mode 必须 = RAID (留在 AHCI 就根本看不到阵列菜单)"},
        {"raidLevel", 1, "RAID Level 选 RAID0: 条带, 速度翻倍, 容量不损失"},
        {"raidDisks", 2, "选中 2 块盘"},
        {"raidStripe", 3, "大文件顺序读写 -> Stripe Size 用 128K"},
        {"raidCache", 2, "临时盘可以开 Write-back Cache 换性能"},
        {"raidCreate", 2, "最后必须 Create Volume 才真正生效"},
        {"raidBootable", 2, "这个阵列要装系统 -> 标记为可引导"},
      },
      extra = {"RAID0 零冗余: 任意一块盘坏 = 整个阵列数据全丢。当临时盘可以, 别放唯一副本。"},
    },
    ["raid1-mirror"] = {
      label = "阵列-双盘 RAID1 镜像",
      mode = "raid",
      brief = "方案要求: 两块盘互为镜像, 坏一块还能开机进系统; 不要求容量大。",
      rules = {
        {"sataModeRaid", 2, "SATA Mode = RAID"},
        {"raidLevel", 2, "RAID Level 选 RAID1: 镜像, 容错一块"},
        {"raidDisks", 2, "选中 2 块盘"},
        {"raidCreate", 2, "Create Volume"},
        {"raidBootable", 2, "要装系统 -> 标记为可引导"},
      },
      extra = {"RAID1 可用容量 = 单盘容量(两块 1T 只有 1T), 换来\"坏一块不丢数据、能直接开机\"。"},
    },
    ["raid5-3disk"] = {
      label = "阵列-三盘 RAID5 容错",
      mode = "raid",
      brief = "方案要求: 三块盘, 既要容量利用率高, 又要能容错一块。",
      rules = {
        {"sataModeRaid", 2, "SATA Mode = RAID"},
        {"raidLevel", 3, "RAID Level 选 RAID5: 分布式校验, 至少 3 块"},
        {"raidDisks", 3, "选中 3 块盘"},
        {"raidStripe", 2, "混合用途 -> Stripe Size 64K 比较稳"},
        {"raidCreate", 2, "Create Volume"},
        {"raidBootable", 2, "要装系统 -> 标记为可引导"},
      },
      extra = {"RAID5 可用容量 = (n-1) x 单盘容量; 只容错**一块** -- 重建期间再坏一块就全丢。"},
    },
    free = {
      label = "自由模式 - 随便改",
      mode = "free",
      brief = "自由模式: 不判定任何规则。随便翻每一页、随便改每一项, 用来熟悉\"这一项在哪、改完会变成什么样\"。",
      rules = {},
      extra = {"想验证某个装机场景时, 按 BACK 回上一级换成\"系统安装\"或\"阵列配置\"模式。"},
    },
  }
local ACTIONS = {
    ["*saveExit"] = {
      name = "Save Changes and Exit",
      act = "saveexit",
      help = "保存并重启（真机是 F10）。",
    },
    ["*discardExit"] = {
      name = "Discard Changes and Exit",
      act = "discardexit",
      help = "不保存退出（真机是 Esc）。",
    },
    ["*bootOrder"] = {
      name = "Boot Priority Order",
      act = "bootorder",
      help = "进编辑后用 ^v 选设备，<- -> 把它上移/下移。",
    },
    ["*check"] = {
      name = "检查结果 / 装机结论",
      act = "check",
      help = "按当前设置逐条判定：符合几项、差哪几项、每一项为什么重要。",
    },
    ["*help"] = {
      name = "说明（当前选项 / 本页）",
      act = "help",
      help = "看当前光标所在选项到底影响什么。真机是 F1。",
    },
    ["*defaults"] = {
      name = "载入默认值 Load Defaults",
      act = "defaults",
      help = "恢复出厂默认设置。真机是 F9 / Load Setup Defaults。",
    },
  }
-- 品牌图标 64x64 1bpp（来自 apps/biossim/brand_icons/*.pbm, 可替换）
-- @@OFFICIAL-BEGIN@@
-- 由 tools/biossim_gen_official.py 从联想官方模拟器的数据文件生成，**不要手改**。
-- 改数据请改 apps/biossim/ref/lenovo_official/*.json 或本脚本，然后重跑。
local OFFICIAL_MODELS = {
  {
    key = "v530", short = "V530", icon = "ibm",
    label = "联想 V530-15ARR（台式机 文本版）",
    title = "Lenovo BIOS Setup  Lenovo V530-15ARR (10Y3,10V3)", thint = "",
    menus = {"Main", "Devices", "Advanced", "Power", "Security", "Startup", "Exit"},
    pages = {
      ["Main"] = {"v530_0001", "v530_0002", "v530_0003", "v530_0004", "v530_0005", "v530_0006", "v530_0007", "v530_0008", "v530_0009", "v530_0010", "v530_0011", "v530_0012", "v530_0013", "v530_0014", "v530_0015", "@System Summary", "v530_0016", "v530_0017", "@System Time && Date", "v530_0018", "v530_0019", "v530_0020", "v530_0021", "v530_0022", "v530_0023", "v530_0024", "v530_0025", "v530_0026", "v530_0027", "v530_0028", "v530_0029", "v530_0030", "v530_0031", "v530_0032"},
      ["Devices"] = {"v530_0033", "v530_0034", "@Serial Port Setup", "v530_0035", "v530_0036", "v530_0037", "v530_0038", "v530_0039", "@Parallel Port Setup", "v530_0040", "v530_0041", "v530_0042", "v530_0043", "v530_0044", "@USB Setup", "v530_0045", "v530_0046", "v530_0047", "v530_0048", "v530_0049", "v530_0050", "v530_0051", "@ATA Drive Setup", "v530_0052", "v530_0053", "v530_0054", "v530_0055", "@Video Setup", "v530_0056", "v530_0057", "@Audio Setup", "v530_0058", "v530_0059", "v530_0060", "v530_0061", "v530_0062", "v530_0063", "v530_0064", "@Network Setup", "v530_0065", "v530_0066", "v530_0067", "@PCI Express Configuration", "v530_0068", "v530_0069", "v530_0070"},
      ["Advanced"] = {"v530_0071", "v530_0072", "v530_0073", "v530_0074", "v530_0075", "v530_0076", "v530_0077", "@CPU Setup", "v530_0078", "v530_0079"},
      ["Power"] = {"v530_0080", "v530_0081", "v530_0082", "v530_0083", "v530_0084", "v530_0085", "@Intelligent Cooling Engine(ICE)", "v530_0086", "v530_0087", "v530_0088", "v530_0089", "v530_0090", "v530_0091", "v530_0092", "v530_0093", "v530_0094", "v530_0095", "v530_0096", "v530_0097", "v530_0098", "v530_0099", "v530_0100", "@User Defined Alarm", "@Automatic Power On"},
      ["Security"] = {"v530_0101", "v530_0102", "v530_0103", "v530_0104", "v530_0105", "v530_0106", "v530_0107", "v530_0108", "v530_0109", "v530_0110", "v530_0111", "v530_0112", "v530_0113", "v530_0114", "v530_0115", "v530_0116", "v530_0117", "v530_0118", "v530_0119", "v530_0120", "v530_0121", "v530_0122", "v530_0123", "v530_0124", "@Security Erase HDD Data", "@Hard Disk Password", "v530_0125", "v530_0126", "v530_0127", "v530_0128", "v530_0129", "@TCG Feature Setup", "v530_0130", "v530_0131", "v530_0132", "v530_0133", "v530_0134", "@View System Event Log", "v530_0135", "@System Event Log", "v530_0136", "v530_0137", "v530_0138", "v530_0139", "v530_0140", "v530_0141", "v530_0142", "v530_0143", "v530_0144", "@Secure Boot", "v530_0145", "v530_0146", "v530_0147", "v530_0148", "v530_0149"},
      ["Startup"] = {"v530_0150", "v530_0151", "v530_0152", "v530_0153", "v530_0154", "v530_0155", "v530_0156", "v530_0157", "v530_0158", "v530_0159", "@Primary Boot Sequence", "v530_0160", "v530_0161", "v530_0162", "v530_0163", "v530_0164", "v530_0165", "v530_0166", "v530_0167", "v530_0168", "v530_0169", "@Automatic Boot Sequence", "v530_0170", "v530_0171", "v530_0172", "v530_0173", "v530_0174", "v530_0175", "v530_0176", "v530_0177", "v530_0178", "v530_0179", "@Error Boot Sequence", "v530_0180", "v530_0181", "v530_0182", "v530_0183", "v530_0184", "v530_0185", "v530_0186", "v530_0187", "v530_0188", "v530_0189", "v530_0190", "@Manufacturing Boot Sequence", "v530_0191", "v530_0192", "v530_0193", "v530_0194", "v530_0195", "v530_0196", "v530_0197"},
      ["Exit"] = {"v530_0198", "v530_0199", "v530_0200", "v530_0201", "v530_0202"},
    },
    subpages = {
      ["System Summary"] = {},
      ["System Time && Date"] = {},
      ["Serial Port Setup"] = {},
      ["Parallel Port Setup"] = {},
      ["USB Setup"] = {},
      ["ATA Drive Setup"] = {},
      ["Video Setup"] = {},
      ["Audio Setup"] = {},
      ["Network Setup"] = {},
      ["PCI Express Configuration"] = {},
      ["CPU Setup"] = {},
      ["Intelligent Cooling Engine(ICE)"] = {},
      ["Automatic Power On"] = {},
      ["User Defined Alarm"] = {},
      ["Hard Disk Password"] = {},
      ["Security Erase HDD Data"] = {},
      ["TCG Feature Setup"] = {},
      ["System Event Log"] = {},
      ["View System Event Log"] = {},
      ["Secure Boot"] = {},
      ["Primary Boot Sequence"] = {},
      ["Automatic Boot Sequence"] = {},
      ["Error Boot Sequence"] = {},
      ["Manufacturing Boot Sequence"] = {},
    },
    items = {
      ["v530_0001"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0002"] = {name = "CPU Type", vals = {"CPU Brand String"}, def = 1, ro = true, help = ""},
      ["v530_0003"] = {name = "CPU Speed", vals = {"xx0GHz"}, def = 1, ro = true, help = ""},
      ["v530_0004"] = {name = "CPU Core Count", vals = {"x"}, def = 1, ro = true, help = ""},
      ["v530_0005"] = {name = "Installed Memory", vals = {"xxxx MB"}, def = 1, ro = true, help = ""},
      ["v530_0006"] = {name = "Memory Bus Speed", vals = {"XX MHz"}, def = 1, ro = true, help = ""},
      ["v530_0007"] = {name = "Active Video", vals = {"IGD/PEG/PCI"}, def = 1, ro = true, help = ""},
      ["v530_0008"] = {name = "Onboard Audio", vals = {"Enabled"}, def = 1, ro = true, help = ""},
      ["v530_0009"] = {name = "Onboard Ethernet", vals = {"Enabled"}, def = 1, ro = true, help = ""},
      ["v530_0010"] = {name = "CPU Fan", vals = {"Operating"}, def = 1, ro = true, help = ""},
      ["v530_0011"] = {name = "System Fan", vals = {"Operating"}, def = 1, ro = true, help = ""},
      ["v530_0012"] = {name = "Aux Fan", vals = {"Operating"}, def = 1, ro = true, help = ""},
      ["v530_0013"] = {name = "M.2 Drive 1", vals = {"None"}, def = 1, ro = true, help = ""},
      ["v530_0014"] = {name = "SATA Drive 1", vals = {"None"}, def = 1, ro = true, help = ""},
      ["v530_0015"] = {name = "SATA Drive 2", vals = {"None"}, def = 1, ro = true, help = ""},
      ["v530_0016"] = {name = "System Time(HH:MM:SS)", vals = {""}, def = 1, ro = true, help = "Set the Time.\nUse Tab key to switch between Time fields."},
      ["v530_0017"] = {name = "System Date(YYYY:MM:DD)", vals = {""}, def = 1, ro = true, help = "Set the Date.\nUse Tab key to switch between Date fields."},
      ["v530_0018"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0019"] = {name = "Machine Type and Model", vals = {"10Y3"}, def = 1, ro = true, help = ""},
      ["v530_0020"] = {name = "System Brand ID", vals = {"Lenovo V530-15ARR"}, def = 1, ro = true, help = ""},
      ["v530_0021"] = {name = "System Serial Number", vals = {"INVALID"}, def = 1, ro = true, help = ""},
      ["v530_0022"] = {name = "Asset tag", vals = {" "}, def = 1, ro = true, help = ""},
      ["v530_0023"] = {name = "System UUID", vals = {"666DE75C-1234-1234-1234-1234567890AB"}, def = 1, ro = true, help = ""},
      ["v530_0024"] = {name = "Ethernet MAC Address", vals = {"XX-XX-XX-XX-XX-XX"}, def = 1, ro = true, help = ""},
      ["v530_0025"] = {name = "Embedded Controller Version", vals = {"O4DCT"}, def = 1, ro = true, help = ""},
      ["v530_0026"] = {name = "BIOS Revision Level", vals = {"O4DKT"}, def = 1, ro = true, help = ""},
      ["v530_0027"] = {name = "Boot Block Revision Level", vals = {"x.xx"}, def = 1, ro = true, help = ""},
      ["v530_0028"] = {name = "BIOS Date (MM/DD/YYYY)", vals = {"xx/xx/xxxx"}, def = 1, ro = true, help = ""},
      ["v530_0029"] = {name = "Preinstalled OS License", vals = {"Not Defined"}, def = 1, ro = true, help = ""},
      ["v530_0030"] = {name = "OA3 License Key ID", vals = {"NO DPK"}, def = 1, ro = true, help = ""},
      ["v530_0031"] = {name = "OA2.1 Marker", vals = {"N"}, def = 1, ro = true, help = ""},
      ["v530_0032"] = {name = "Language", vals = {"English", "Francais", "中文"}, def = 1, ro = false, help = ""},
      ["v530_0033"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0034"] = {name = "Serial Port1 Address", vals = {"3F8/IRQ4", "2F8/IRQ3", "Disabled", "3E8/IRQ4", "2E8/IRQ3"}, def = 1, ro = false, help = "Select settings for the Serial Port 1. If set to \"Disabled\", the Serial Port will not be seen by the OS."},
      ["v530_0035"] = {name = "Parallel Port Address", vals = {"378", "278", "3BC", "Disabled"}, def = 1, ro = false, help = "Select settings for the Parallel Port. If set to \"Disabled\", the Parallel Port will not be seen by the OS."},
      ["v530_0036"] = {name = "Parallel Port Mode", vals = {"ECP+EPP", "ECP", "EPP"}, def = 1, ro = false, help = "Change the Parallel Port Mode. \nEPP (Enhanced Parallel Port) and ECP (Extended Capabilities Port) are two modes to transmit data by Parallel Port. EPP is more commonly used than ECP."},
      ["v530_0037"] = {name = "EPP Version", vals = {"1.9", "1.7"}, def = 1, ro = false, help = "There are differences between the two standards (1.9 and 1.7 version) which may affect the operation of devices."},
      ["v530_0038"] = {name = "ECP Mode DMA Channel", vals = {"DMA3", "DMA1"}, def = 1, ro = false, help = "DMA (Direct Memory Access), a technique for transferring data from main memory to the parallel port devices without passing it through the CPU. Computers that have DMA channels can transfer data to and from devices while the CPU works on other tasks. \nSelect DMA3 or DMA1 channel."},
      ["v530_0039"] = {name = "Parallel Port IRQ", vals = {"IRQ7", "IRQ5"}, def = 1, ro = false, help = "An IRQ (interrupt request) value is an assigned location where the computer can expect the parallel port controller to interrupt it when the device sends the computer signals about its operation."},
      ["v530_0040"] = {name = "USB Support", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable USB (Universal Seral Bus) functions.\nIf the functions are disabled, no USB device can be used."},
      ["v530_0041"] = {name = "USB Legacy Support", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "If it is disabled, the USB keyboard and/or USB mouse may be not able to be used without device driver support."},
      ["v530_0042"] = {name = "USB Enumeration Delay", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "If it is enabled, extra delay will be added when host performs USB enumeration. This may improve the compatibility of USB device detection but increase POST time."},
      ["v530_0043"] = {name = "Front USB Ports", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable the side USB ports."},
      ["v530_0044"] = {name = "Rear USB Ports", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable the rear USB ports."},
      ["v530_0045"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0046"] = {name = "SATA Controller", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable SATA controller."},
      ["v530_0047"] = {name = "SATA Drive 1", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable SATA Drive 1."},
      ["v530_0048"] = {name = "SATA Drive 2", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable SATA Drive 2."},
      ["v530_0049"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0050"] = {name = "Configure SATA as", vals = {"AHCI"}, def = 1, ro = false, help = "Select AHCI Mode.\nNOTE:\nDevice driver support is required  for AHCI.\nDepending on how the hard disk image was installed, changing this setting may prevent the system from booting."},
      ["v530_0051"] = {name = "Hard Disk Pre-delay", vals = {"Disabled", "3 Seconds", "6 Seconds", "9 Seconds", "12 Seconds", "15 Seconds", "21 Seconds", "30 Seconds"}, def = 1, ro = false, help = "Adds a delay before the first access of a hard disk by the system software. Some hard disks hang if accessed before they have initialized themselves. This delay ensures the hard disk has initialized after power up, prior to being accessed."},
      ["v530_0052"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0053"] = {name = "UMA Frame Buffer Size", vals = {"Auto", "256M", "512M", "1024M"}, def = 1, ro = false, help = "Configure the memory size for internal graphic."},
      ["v530_0054"] = {name = "Primary Video Controller", vals = {"Auto", "IGD", "PEG"}, def = 1, ro = false, help = "Select which graphic controller to be used as the primary display device."},
      ["v530_0055"] = {name = "Surround View", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "If PCIE is primary display,allow IGFX(Internal Graphic) to be secondary display."},
      ["v530_0056"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0057"] = {name = "Onboard Audio Controller", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable Onboard Audio Controller"},
      ["v530_0058"] = {name = "Onboard Ethernet Controller", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable Onboard Ethernet Controller.\nNOTE: If this option is changed to \"Disabled\", the Intel(R) AMT related functions will be disabled."},
      ["v530_0059"] = {name = "PXE Option ROM", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to disable, load onboard PXE (Preboot Execution Environment).  This feature will allow the computer to boot from a server image."},
      ["v530_0060"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0061"] = {name = "PXE IPV4 network stack", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable PXE IPV4 network stack"},
      ["v530_0062"] = {name = "PXE IPV6 network stack", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable PXE IPV6 network stack"},
      ["v530_0063"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0064"] = {name = "Wireless LAN", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable Wireless LAN.\n<Enabled> Enables use of Wireless LAN.\n<Disabled> Disables use of Wireless LAN. Wireless LAN will not be available is OS."},
      ["v530_0065"] = {name = "ASPM Support", vals = {"Auto", "Disabled"}, def = 1, ro = false, help = "<Auto> Configure ASPM automatically according to what the attached device supports in each PCI Express port.\n<Disabled> Disable ASPM support of all PCI Express ports."},
      ["v530_0066"] = {name = "PCIe 16x Slot Speed", vals = {"Auto", "Gen 1", "Gen 2", "Gen 3"}, def = 1, ro = false, help = "Select PCI Express port speed."},
      ["v530_0067"] = {name = "PCIe 1x Slot 1 Speed", vals = {"Auto", "Gen 1", "Gen 2", "Gen 3"}, def = 1, ro = false, help = "Select PCI Express port speed."},
      ["v530_0068"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0069"] = {name = "Card Reader", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable Card Reader.\n<Enabled> Enables use of Card Reader.\n<Disabled> Disables use of Card Reader.\nCard reader will not be available in OS."},
      ["v530_0070"] = {name = "Bluetooth", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable Bluetooth.\n<Enabled> Enables use of Bluetooth.\n<Disabled> Disables use of Bluetooth.\nBluetooth will not be available in OS."},
      ["v530_0071"] = {name = "AMD Secure Virtual Machine", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "AMD Virtualization technology is a set of unique on-chip features that enables AMD PRO-based clients to run multiple operating systems and applications on a single machine.\nSelect whether to enable or disable AMD Virtualization technology."},
      ["v530_0072"] = {name = "CPB Mode", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Core Performance Boost allows the processor to dynamically control and adjust its operating frequency to increase performance when needed and maintain lower power and thermal characteristics during normal operation."},
      ["v530_0073"] = {name = "C6 Support", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select whether to enable or disable the C6 power management state to minimize idle power consumption of the processor."},
      ["v530_0074"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0075"] = {name = "CPU ID", vals = {"xxxxx"}, def = 1, ro = true, help = ""},
      ["v530_0076"] = {name = "Microcode Revision", vals = {"000000xx"}, def = 1, ro = true, help = ""},
      ["v530_0077"] = {name = "Chipset", vals = {"xxx"}, def = 1, ro = true, help = ""},
      ["v530_0078"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0079"] = {name = "Dust Shield Alert", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "<Enabled> The user is reminded to clean the Dust Shield. \n<Disabled> The user is not reminded to clean the Dust Shield. \nNote that if the Dust Shield is not cleaned then it will lose its effectiveness and the machine may overheat."},
      ["v530_0080"] = {name = "After Power Loss", vals = {"Power Off", "Last State", "Power On"}, def = 1, ro = false, help = "This field controls whether the system will stay on after AC power is removed and then restored.\nSelect \"Power On\" if you use a power strip to turn the system on.\nSelect \"Power Off\" if you want the system to remain off after power restored.\nSelect \"Last State\" to have the system return to the previous state."},
      ["v530_0081"] = {name = "Enhanced Power Saving Mode", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Select whether to enable Enhanced Power Saving Mode.\n<Enabled> The system will enter to Enhanced Power Saving Mode during power off. The total power consumption will be lower during power off.\n<Disabled> Disables function of Enhanced Power Save Mode.\nNote: When enabled Enhanced Power Saving Mode, only the Wake Up on Alarm function is supported. Other wake up functions are not supported. System will not enter to Enhanced Power Saving Mode if Intel ME is required to be active in Sx states and host is in AC mode."},
      ["v530_0082"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0083"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0084"] = {name = "ICE Performance Mode", vals = {"Better Acoustic Performance", "Better Thermal Performance", "Full Speed"}, def = 1, ro = false, help = "<Better Acoustic Performance> The system will run with less noise at a normal thermal level.\n<Better Thermal Performance> The system will run at a better thermal level with normal acoustic performance.\n<Full Speed> All fans in the system will run at full speed."},
      ["v530_0085"] = {name = "ICE Thermal Alert", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Thermal monitoring & alert features to protect system under critical thermal situations."},
      ["v530_0086"] = {name = "Wake on LAN", vals = {"Automatic", "Primary", "Disabled"}, def = 1, ro = false, help = "Select whether to enable Wake on LAN, and/or which Startup Sequence to use after a Wake on LAN event.\nNOTE: This item controls the wake up event from onboard LAN and PCI LAN."},
      ["v530_0087"] = {name = "Wake from Serial Port Ring", vals = {"Primary", "Automatic", "Disabled"}, def = 1, ro = false, help = "Select whether to enable Wake from Serial Port Ring, and/or which Startup Sequence to use after a serial port wake event."},
      ["v530_0088"] = {name = "Wake Up on Alarm", vals = {"Disabled", "Single Event", "Daily Event", "Weekly Event", "User Defined"}, def = 1, ro = false, help = "Select whether to enable Wake Up on Alarm, totun on your system on a special day of the month, special day of the week or daily.\nNOTE: Values in these fields may be overwritten by the operating system."},
      ["v530_0089"] = {name = "Startup Sequence", vals = {"Primary", "Automatic"}, def = 1, ro = true, help = "Select the Startup Sequence to use after a Wake Up on Alarm event."},
      ["v530_0090"] = {name = "Alarm Time (HH:MM:SS)", vals = {"00:00:00"}, def = 1, ro = true, help = "Specify the time when the system is to wake up."},
      ["v530_0091"] = {name = "Alarm Date (MM/DD/YYYY)", vals = {"01/01/1999"}, def = 1, ro = true, help = "Specify the date when the system is to wake up."},
      ["v530_0092"] = {name = "Alarm Day of Week", vals = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"}, def = 1, ro = true, help = "Select the day of the week when the system is to wake up."},
      ["v530_0093"] = {name = "Sunday", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Select the day of the week when the system is to wake up."},
      ["v530_0094"] = {name = "Monday", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Select the day of the week when the system is to wake up."},
      ["v530_0095"] = {name = "Tuesday", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Select the day of the week when the system is to wake up."},
      ["v530_0096"] = {name = "Wednesday", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Select the day of the week when the system is to wake up."},
      ["v530_0097"] = {name = "Thursday", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Select the day of the week when the system is to wake up."},
      ["v530_0098"] = {name = "Friday", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Select the day of the week when the system is to wake up."},
      ["v530_0099"] = {name = "Saturday", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Select the day of the week when the system is to wake up."},
      ["v530_0100"] = {name = "User Defined Alarm Time (HH:MM:SS)", vals = {"00:00:00"}, def = 1, ro = true, help = "Specify the time when the system is to wake up."},
      ["v530_0101"] = {name = "Administrator Password", vals = {"Not Installed", "Installed"}, def = 1, ro = true, help = ""},
      ["v530_0102"] = {name = "Power-On Password", vals = {"Not Installed", "Installed"}, def = 1, ro = true, help = ""},
      ["v530_0103"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0104"] = {name = "Set Administrator Password", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0105"] = {name = "Set Power-On Password", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0106"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0107"] = {name = "Allow Flashing BIOS to a Previous Version", vals = {"Yes", "No"}, def = 1, ro = false, help = "<Yes> - Flash BIOS to a previous  or current version is allowed. This is the default setting.\n<No> - Flash BIOS to a previous  or current version is not allowed."},
      ["v530_0108"] = {name = "Require Admin. Pass. when Flashing", vals = {"No", "Yes"}, def = 1, ro = false, help = "If \"No\" is selected, the Adiministrator password will not be required when updating the system software."},
      ["v530_0109"] = {name = "Windows UEFI Firmware Update", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "This option enables or disables windows UEFI firmware update feature.\n<Enabled> Allow windows UEFI firmware update.\n<Disabled> BIOS will skip windows UEFI firmware update."},
      ["v530_0110"] = {name = "Require POP on System Boot", vals = {"Yes", "No"}, def = 1, ro = false, help = "<Yes> The system prompts for passwords when the system starts from the full off or hibernate state.\n<No> Passwords are not prompted and continue to boot the OS. To prevent unauthorized access to the system, recommend to set user authentication on the OS."},
      ["v530_0111"] = {name = "Require POP on Restart", vals = {"No", "Yes"}, def = 1, ro = false, help = "If \"No\" is selected, the Power-On Password will only be required when the system is started with a cold boot."},
      ["v530_0112"] = {name = "POP Changeable by User", vals = {"Yes", "No"}, def = 1, ro = false, help = "If \"No\" is selected, the Power-On Password can only be changed by the Supervisor password."},
      ["v530_0113"] = {name = "Require Admin. Pass. For F12 Boot", vals = {"No", "Yes"}, def = 1, ro = false, help = "If Yes and an Administrator Password is aet, the user will be prompted for a password when F12 is pressed during POST."},
      ["v530_0114"] = {name = "Smart USB Protection", vals = {"Disabled", "Read Only", "No Access"}, def = 1, ro = false, help = "Smart USB Protection could block copying data from the computer to the USB storage device in windows.\nRead Only mode: the user can copy data from USB storage device to the Computer but cannot copy data from the computer to USB storage device.\nNo Access mode: the user cannot use USB storage device in windows"},
      ["v530_0115"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0116"] = {name = "M.2 Drive 1 Password", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0117"] = {name = "SATA Drive 1 Password", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0118"] = {name = "SATA Drive 2 Password", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0119"] = {name = "Require HDP on System Boot", vals = {"Auto", "No"}, def = 1, ro = false, help = "<Auto> - Hard Disk Password (HDP) will be required if the Hard Disk is in lock status when the system starts from the full off, hibernate or restart state.\n<No> - HDP will not be required. However, HDP will be required when the hard disk is attached to a different system."},
      ["v530_0120"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0121"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0122"] = {name = "Erase M.2 Drive 1 Data", vals = {"Enter"}, def = 1, ro = true, help = ""},
      ["v530_0123"] = {name = "Erase SATA Drive 1 Data", vals = {"Enter"}, def = 1, ro = true, help = ""},
      ["v530_0124"] = {name = "Erase SATA Drive 2 Data", vals = {"Enter"}, def = 1, ro = true, help = ""},
      ["v530_0125"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0126"] = {name = "TCG Security Device", vals = {"Firmware TPM"}, def = 1, ro = true, help = "Contains TCG security features."},
      ["v530_0127"] = {name = "  Security Chip 2.0", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "<Enabled> When item enabled that TCG security feature is full function.\n<Disabled> When item disabled, TCG security feature is no functional.\nNOTE:\nIf set to  \"Disabled\", the TxT will be set to \"Disabled\" automatically."},
      ["v530_0128"] = {name = "  Clear TCG Security Feature", vals = {"NO", "YES"}, def = 1, ro = false, help = "Select whether to clear TCG Security Feature.\nWarning: Any data in TPM will be cleared."},
      ["v530_0129"] = {name = "  Physical Presence for Clear", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "This option enables or disables confirmation of a user's physical presence when Clearing the security chip. \n<Enabled> Display user confirmation screen when Clearing. \n<Disabled> No user configuration screen when Clearing."},
      ["v530_0130"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0131"] = {name = "DATE       TIME       Events", vals = {" "}, def = 1, ro = true, help = ""},
      ["v530_0132"] = {name = "09/14/13   08:47:27   System Event Log Cleared", vals = {" "}, def = 1, ro = true, help = ""},
      ["v530_0133"] = {name = "09/14/13   08:48:38   BIOS updated", vals = {" "}, def = 1, ro = true, help = ""},
      ["v530_0134"] = {name = "09/14/13   09:40:16   BIOS updated", vals = {" "}, def = 1, ro = true, help = ""},
      ["v530_0135"] = {name = "Clear System Event Log", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0136"] = {name = "System Mode", vals = {"User Mode"}, def = 1, ro = true, help = ""},
      ["v530_0137"] = {name = "Secure Boot", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "<Enabled> Enables Secure Boot,BIOS will prevent un-authorised OS be loaded.\n<Disabled> Disables Secure Boot. \nWhen Secure boot set to Enabled, CSM would be automatically disabled."},
      ["v530_0138"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0139"] = {name = "Restore Factory Keys", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0140"] = {name = "Reset Platform to Setup Mode", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0141"] = {name = "Enter Audit Mode", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0142"] = {name = "Enter Deployed Mode", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0143"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0144"] = {name = "Key Management", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0145"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0146"] = {name = "Device Guard", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Device Guard enables PCs to be protected against malware by introducing a collective set of restrictions on a device across several technologies.\n<Enabled> CPU Virtualization Technology be enabled, IOMMU, Such as Intel VT-d, AMD-Vi be enabled, Secure boot be enabled, TPM be enabled Ethernet, USB, CD, and other boot methods be disabled,only SATA device is allowed.\n<Disabled>Ethernet, USB, CD, and other boot methods be enabled."},
      ["v530_0147"] = {name = "Chassis Intrusion Detection", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Chassis Intrusion Detection is a utility that can tell whether someone has opened the case (intruded into the chassis).\nNOTE:\nIf chassis tamper occurs, you can only enter setup to clear this error."},
      ["v530_0148"] = {name = "Configuration Change Detection", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Select whether to enable Configuration Change Detection. If enabled, when a device is installed or remove, the system will notify the user during POST. This notice can only be cleared by entering BIOS setup, saving and then exiting."},
      ["v530_0149"] = {name = "Password Count Exceeded Error", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Select Enabled to show the POST 0199 error and prompt for password. \nSelect Disabled to hide the POST 0199 error and proceed without any user action required."},
      ["v530_0150"] = {name = "USB FDD:", vals = {""}, def = 1, ro = true, help = "This sequence is used when the system is powered up normally.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0151"] = {name = "USB KEY:", vals = {""}, def = 1, ro = true, help = "This sequence is used when the system is powered up normally.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0152"] = {name = "M.2:", vals = {""}, def = 1, ro = true, help = "This sequence is used when the system is powered up normally.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0153"] = {name = "SATA 1:", vals = {""}, def = 1, ro = true, help = "This sequence is used when the system is powered up normally.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0154"] = {name = "SATA 2:", vals = {""}, def = 1, ro = true, help = "This sequence is used when the system is powered up normally.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0155"] = {name = "SATA 3:", vals = {""}, def = 1, ro = true, help = "This sequence is used when the system is powered up normally.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0156"] = {name = "Network 1:", vals = {""}, def = 1, ro = true, help = "This sequence is used when the system is powered up normally.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0157"] = {name = "USB HDD:", vals = {""}, def = 1, ro = true, help = "This sequence is used when the system is powered up normally.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0158"] = {name = "USB CDROM:", vals = {""}, def = 1, ro = true, help = "This sequence is used when the system is powered up normally.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0159"] = {name = "Other Device:", vals = {""}, def = 1, ro = true, help = "This sequence is used when the system is powered up normally.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0160"] = {name = "Network 1:", vals = {""}, def = 1, ro = true, help = "This sequence is used when a communication device wakes the system up.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0161"] = {name = "M.2:", vals = {""}, def = 1, ro = true, help = "This sequence is used when a communication device wakes the system up.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0162"] = {name = "SATA 1:", vals = {""}, def = 1, ro = true, help = "This sequence is used when a communication device wakes the system up.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0163"] = {name = "SATA 2:", vals = {""}, def = 1, ro = true, help = "This sequence is used when a communication device wakes the system up.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0164"] = {name = "SATA 3", vals = {""}, def = 1, ro = true, help = "This sequence is used when a communication device wakes the system up.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0165"] = {name = "Other Device", vals = {""}, def = 1, ro = true, help = "This sequence is used when a communication device wakes the system up.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0166"] = {name = "USB FDD:", vals = {""}, def = 1, ro = true, help = "Excluded from boot order:\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0167"] = {name = "USB HDD:", vals = {""}, def = 1, ro = true, help = "Excluded from boot order:\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0168"] = {name = "USB CDROM:", vals = {""}, def = 1, ro = true, help = "Excluded from boot order:\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0169"] = {name = "USB KEY:", vals = {""}, def = 1, ro = true, help = "Excluded from boot order:\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0170"] = {name = "Network 1:", vals = {""}, def = 1, ro = true, help = "This sequence is used when BIOS determines that an error has occured.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0171"] = {name = "M.2:", vals = {""}, def = 1, ro = true, help = "This sequence is used when BIOS determines that an error has occured.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0172"] = {name = "SATA 1:", vals = {""}, def = 1, ro = true, help = "This sequence is used when BIOS determines that an error has occured.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0173"] = {name = "SATA 2:", vals = {""}, def = 1, ro = true, help = "This sequence is used when BIOS determines that an error has occured.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0174"] = {name = "SATA 3", vals = {""}, def = 1, ro = true, help = "This sequence is used when BIOS determines that an error has occured.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0175"] = {name = "Other Device", vals = {""}, def = 1, ro = true, help = "This sequence is used when BIOS determines that an error has occured.\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0176"] = {name = "USB FDD:", vals = {""}, def = 1, ro = true, help = "Excluded from boot order:\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0177"] = {name = "USB HDD:", vals = {""}, def = 1, ro = true, help = "Excluded from boot order:\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0178"] = {name = "USB CDROM:", vals = {""}, def = 1, ro = true, help = "Excluded from boot order:\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0179"] = {name = "USB KEY:", vals = {""}, def = 1, ro = true, help = "Excluded from boot order:\nUse Up and Down arrows to select a device.\n<+> and <-> move the device up or down.\n<x> excludes / includes the device to boot."},
      ["v530_0180"] = {name = "Network 1:", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0181"] = {name = "M.2:", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0182"] = {name = "SATA 1:", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0183"] = {name = "SATA 2:", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0184"] = {name = "SATA 3", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0185"] = {name = "Other Device", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0186"] = {name = "Excluded from boot order", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0187"] = {name = "USB FDD:", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0188"] = {name = "USB HDD:", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0189"] = {name = "USB CDROM:", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0190"] = {name = "USB KEY:", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0191"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0192"] = {name = "CSM", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Compatibility support Module. Permit the loading of a traditional operation system.\nEnable: to support Non-UEFI operation system.\nDisable: to support Win8 64-bit and later UEFI aware operation system."},
      ["v530_0193"] = {name = "Boot Mode", vals = {"Auto", "UEFI Only", "Legacy Only"}, def = 1, ro = false, help = "Select <Auto>, system will try to boot to UEFI or legacy operating system.\nSelect <UEFI Only>, system will only boot to UEFI aware operating system.\nSelect <Legacy Only>, system will only boot to legacy operating system."},
      ["v530_0194"] = {name = "Boot Priority", vals = {"UEFI First", "Legacy First"}, def = 1, ro = false, help = "Select <UEFI First>, system will try to boot to UEFI OS first and then legacy OS.\nSelect <Legacy First>, system will try to boot to legacy OS first and then UEFI OS."},
      ["v530_0195"] = {name = "Boot Up Num-Lock Status", vals = {"On", "Off"}, def = 1, ro = false, help = "This field indicates the state of the NumLock feature of the keyboard after Startup. If NumLock is \"On\", the keys on the keypad will act as numeric keys. If NumLock is \"Off\", the keys on the keypad will act as cursor keys."},
      ["v530_0196"] = {name = "Option Keys Display", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "Controls the system software option key prompts (such as F1 key) when the system is turned on. Select \"Enabled\" to display the prompts.\nNOTE: Disabling the prompts will not affect the function of a specific key."},
      ["v530_0197"] = {name = "Option Keys Display Style", vals = {"Normal", "Legacy"}, def = 1, ro = false, help = "Controls the style of prompts displayed on the POST logo screen when the system is turned on. Choosing \"Normal\" will prompt for the Enter key and display a menu. Choosing \"Legacy\" will prompt fo F1 and F12."},
      ["v530_0198"] = {name = "Save Changes and Exit", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0199"] = {name = "Discard Changes and Exit", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0200"] = {name = "", vals = {""}, def = 1, ro = true, help = ""},
      ["v530_0201"] = {name = "Load Optimal Defaults", vals = {""}, def = 1, ro = true, help = "[Enter]"},
      ["v530_0202"] = {name = "OS Optimized Defaults", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "Some settings below are changed accordingly. Select \"Enabled\" to meet Microsoft(R) Windows 10 (R) Certification Requirement.\nAffected settings are CSM Support, Boot mode, Boot Priority, Secure Boot."},
    },
  },
  {
    key = "k14", short = "K14", icon = "ibm",
    label = "联想 K14 G3（普通本）",
    title = "Setup", thint = "[{'name': 'F1', 'value': 'General Help', 'title': 'General Help', 'buttons': ['Close'], 'content': [{'ratio': '0.5', 'value': [{'name': 'Navigate', 'type': 'plainText', 'value': 'You can navigate within the Basic Input/Output System(BIOS) for ThinkPad Setup using the TrackPoint, Touchpad or Keyboard.'}], 'type': 'plainText'}, {'ratio': '0.5', 'value': [{'name': 'Troubleshooting', 'type': 'plainText', 'value': 'Conflicting BIOS configurations may cause the device to not boot. Try loading the default configuration values to recover. \\n${ENTER}More help is  availableon Lenovo eSupport site.'}], 'type': 'plainText'}, {'value': [{'value': 'Help(this page)', 'name': 'F1'}, {'value': 'Back/Close dialog', 'name': 'Esc'}, {'value': 'Select/Open submenu', 'name': 'Enter'}, {'value': 'Select/Scroll page', 'name': 'Up/Down Arrows,or'}, {'value': '', 'name': 'Page Up/Page Down'}], 'type': 'zebraStripe'}, {'value': [{'value': 'Setup defaults', 'name': 'F9'}, {'value': 'Move keyboard focus', 'name': 'Left/Right Arrows'}, {'value': 'Change Boot Priority Order', 'name': 'F5/F6'}, {'value': 'Save & Exit', 'name': 'F10'}], 'type': 'zebraStripe'}], 'hasDialog': True}, {'name': 'F9', 'value': 'Setup Defaults', 'title': 'Setup Confirmation', 'buttons': ['Yes', 'No'], 'content': [{'ratio': '1.0', 'value': [{'type': 'plainText', 'value': 'Load default configuration now?'}], 'type': 'plainText'}], 'hasDialog': True}, {'name': 'Esc', 'value': 'Exit', 'hasDialog': False}, {'name': 'F10', 'value': 'Save and Exit', 'title': 'Setup Confirmation', 'buttons': ['Yes', 'No'], 'content': [{'ratio': '1.0', 'value': [{'type': 'plainText', 'value': 'Save configuration changes and exit now?'}], 'type': 'plainText'}], 'hasDialog': True}]",
    menus = {"Main", "Config", "Date/Time", "Security", "Startup", "Restart"},
    pages = {
      ["Main"] = {"k14_0001", "k14_0002", "k14_0003", "k14_0004", "k14_0005", "k14_0006", "k14_0007", "k14_0008", "k14_0009", "k14_0010", "k14_0011", "k14_0012", "k14_0013", "k14_0014", "k14_0015", "k14_0016", "k14_0017"},
      ["Config"] = {"k14_0018", "k14_0019", "k14_0020", "k14_0021", "k14_0022", "k14_0023", "k14_0024", "@Wi-Fi Configuration", "@Network", "k14_0025", "k14_0026", "@USB", "k14_0027", "k14_0028", "k14_0029", "@Keyboard", "k14_0030", "@Display", "k14_0031", "k14_0032", "k14_0033", "k14_0034", "k14_0035", "k14_0036", "k14_0037", "@Power", "k14_0038", "@Storage", "k14_0039", "@CPU", "k14_0040", "k14_0041", "k14_0042", "@Intel(R) ISM"},
      ["Date/Time"] = {"k14_0043", "k14_0044"},
      ["Security"] = {"k14_0045", "k14_0046", "k14_0047", "k14_0048", "k14_0049", "k14_0050", "@Password", "k14_0051", "k14_0052", "k14_0053", "k14_0054", "@Security Chip", "k14_0055", "k14_0056", "@UEFI BIOS Update Option", "k14_0057", "k14_0058", "k14_0059", "@Virtualization", "k14_0060", "k14_0061", "k14_0062", "k14_0063", "k14_0064", "k14_0065", "k14_0066", "k14_0067", "k14_0068", "k14_0069", "k14_0070", "k14_0071", "@I/O Port Access", "k14_0072", "k14_0073", "@Absolute Persistence(R) Module", "k14_0074", "k14_0075", "k14_0076", "k14_0077", "k14_0078", "k14_0079", "k14_0080", "@Secure Boot", "k14_0081", "@ThinkShield secure wipe", "k14_0082", "@ThinkShield Passwordless Power-On", "k14_0083", "@Reset system to Factory Defaults", "k14_0084", "k14_0085", "k14_0086", "@Device security"},
      ["Startup"] = {"k14_0087", "k14_0088", "k14_0089", "k14_0090", "k14_0091", "k14_0092", "k14_0093", "k14_0094", "k14_0095"},
      ["Restart"] = {"k14_0096", "k14_0097", "k14_0098", "k14_0099", "k14_0100", "k14_0101"},
    },
    subpages = {
      ["Network"] = {},
      ["Wi-Fi Configuration"] = {},
      ["USB"] = {},
      ["Keyboard"] = {},
      ["Display"] = {},
      ["Power"] = {},
      ["Storage"] = {},
      ["CPU"] = {},
      ["Intel(R) ISM"] = {},
      ["Password"] = {},
      ["Security Chip"] = {},
      ["UEFI BIOS Update Option"] = {},
      ["Virtualization"] = {},
      ["I/O Port Access"] = {},
      ["Absolute Persistence(R) Module"] = {},
      ["Secure Boot"] = {},
      ["ThinkShield secure wipe"] = {},
      ["ThinkShield Passwordless Power-On"] = {},
      ["Reset system to Factory Defaults"] = {},
      ["Device security"] = {},
    },
    items = {
      ["k14_0001"] = {name = "Product Name", vals = {"Lenovo K14 G3 IMU"}, def = 1, ro = true, help = ""},
      ["k14_0002"] = {name = "UEFI BIOS Version", vals = {"P3CNXXWW"}, def = 1, ro = true, help = ""},
      ["k14_0003"] = {name = "UEFI BIOS Date(Year-Month-Day)", vals = {"2023-02-03"}, def = 1, ro = true, help = ""},
      ["k14_0004"] = {name = "Embedded Controller Version", vals = {"P3ECXXWW"}, def = 1, ro = true, help = ""},
      ["k14_0005"] = {name = "ME Firmware Version", vals = {"16.1.25.2091"}, def = 1, ro = true, help = ""},
      ["k14_0006"] = {name = "Machine Type Model", vals = {"21KL"}, def = 1, ro = true, help = ""},
      ["k14_0007"] = {name = "Lenovo SN", vals = {"PF123456"}, def = 1, ro = true, help = ""},
      ["k14_0008"] = {name = "Asset Tag", vals = {"No Asset Tag"}, def = 1, ro = true, help = ""},
      ["k14_0009"] = {name = "UUID Number", vals = {"CB95E7F4-1234-ABCD-1234-ABCD1234ABCD"}, def = 1, ro = true, help = ""},
      ["k14_0010"] = {name = "CPU Type", vals = {"Intel(R) Core(TM)"}, def = 1, ro = true, help = ""},
      ["k14_0011"] = {name = "CPU Speed", vals = {"1.800GHZ"}, def = 1, ro = true, help = ""},
      ["k14_0012"] = {name = "Installed Memory", vals = {"8192 MB"}, def = 1, ro = true, help = ""},
      ["k14_0013"] = {name = "Preinstalled OS license", vals = {"NO DPK"}, def = 1, ro = true, help = ""},
      ["k14_0014"] = {name = "OA3 Key ID", vals = {"0000000"}, def = 1, ro = true, help = ""},
      ["k14_0015"] = {name = "Hard Disk", vals = {"XXXX"}, def = 1, ro = true, help = ""},
      ["k14_0016"] = {name = "UEFI Secure Boot", vals = {"Enabled"}, def = 1, ro = true, help = ""},
      ["k14_0017"] = {name = "Language", vals = {"English/中文"}, def = 1, ro = true, help = ""},
      ["k14_0018"] = {name = "Wake On LAN", vals = {"Disabled", "AC Only", "AC and Battery"}, def = 1, ro = false, help = ""},
      ["k14_0019"] = {name = "MAC Address Pass Through", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0020"] = {name = "Wake On LAN from Dock", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0021"] = {name = "Lenovo Cloud Services", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0022"] = {name = "Reinstall Windows from Cloud", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0023"] = {name = "UEFI Wi-Fi Network Boot", vals = {"Enabled", "Disabled"}, def = 2, ro = true, help = ""},
      ["k14_0024"] = {name = "Simulator not Support", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0025"] = {name = "Always On USB", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0026"] = {name = "Charge in Battery Mode", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0027"] = {name = "F1-F12 as Primary Function", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0028"] = {name = "Fool Proof Fn Ctrl", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0029"] = {name = "Fn and Ctrl Key swap", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0030"] = {name = "Graphics Device", vals = {"Switchable Graphics", "Discrete Graphics", "UMA Graphics"}, def = 1, ro = false, help = ""},
      ["k14_0031"] = {name = "Disable Built-in Battery", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0032"] = {name = "System Performance Mode", vals = {"Intelligent Cooling", "Extreme Performance", "Battery Saving"}, def = 1, ro = false, help = ""},
      ["k14_0033"] = {name = "Super Boost", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0034"] = {name = "Ultra Quiet Mode", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0035"] = {name = "One Key Battery", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0036"] = {name = "Flip To Start", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0037"] = {name = "Power On with AC Attach", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0038"] = {name = "Storage Controller Mode", vals = {"AHCI", "RAID"}, def = 1, ro = false, help = ""},
      ["k14_0039"] = {name = "Intel(R) Hyper-Threading Technology", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0040"] = {name = "Intel(R) Standard Manageability Control", vals = {"Enabled", "Disabled", "Permanently Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0041"] = {name = "USB Key Provisioning", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["k14_0042"] = {name = "Console Type", vals = {"VT100", "VT100+", "VT-UTF8", "PC ANSI"}, def = 2, ro = false, help = ""},
      ["k14_0043"] = {name = "System Date", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0044"] = {name = "System Time", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0045"] = {name = "Supervisor Password", vals = {"Not Set"}, def = 1, ro = true, help = ""},
      ["k14_0046"] = {name = "User Password", vals = {"Not Set"}, def = 1, ro = true, help = ""},
      ["k14_0047"] = {name = "HDD Password", vals = {"Not Set"}, def = 1, ro = true, help = ""},
      ["k14_0048"] = {name = "Set Supervisor Password", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0049"] = {name = "Set Hard Disk Password", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0050"] = {name = "Set Strong Password", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0051"] = {name = "Intel Platform Trust Technology", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0052"] = {name = "Clear Intel PTT Key", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0053"] = {name = "Security Chip", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0054"] = {name = "Clear Security Chip Key", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0055"] = {name = "Secure RollBack Prevention", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0056"] = {name = "BIOS Self-Healing", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0057"] = {name = "Intel(R) Virtualization Technology", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0058"] = {name = "Intel(R) VT-d Feature", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0059"] = {name = "Enhanced Windows Biometric Security", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0060"] = {name = "Wireless LAN", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0061"] = {name = "Ethernet LAN", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0062"] = {name = "Wireless WAN", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0063"] = {name = "Bluetooth", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0064"] = {name = "USB Port", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0065"] = {name = "Memory Card Slot", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0066"] = {name = "Smart Card Slot", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0067"] = {name = "Integrated Camera", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0068"] = {name = "Microphone", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0069"] = {name = "Fingerprint Reader", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0070"] = {name = "Thunderbolt(TM)", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0071"] = {name = "NFC Device", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0072"] = {name = "Absolute Persistence(R) Module Activation", vals = {"Disabled", "Enabled", "Permanently Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0073"] = {name = "Absolute Persistence(R) Module Activation Status", vals = {"Not Activated"}, def = 1, ro = true, help = ""},
      ["k14_0074"] = {name = "Secure Boot", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0075"] = {name = "Secure Boot Status", vals = {"Enabled/Disabled"}, def = 1, ro = true, help = ""},
      ["k14_0076"] = {name = "Secure Boot Mode", vals = {"User Mode"}, def = 1, ro = true, help = ""},
      ["k14_0077"] = {name = "Secure Boot Key State", vals = {"Standard Mode"}, def = 1, ro = true, help = ""},
      ["k14_0078"] = {name = "Reset to Setup Mode", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0079"] = {name = "Restore Factory Keys", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0080"] = {name = "Allow Microsoft 3rd Party UEFI CA", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0081"] = {name = "ThinkShield secure wipe", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0082"] = {name = "ThinkShield Passwordless Power-On Authentication", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0083"] = {name = "Reset system to Factory Defaults", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0084"] = {name = "Device Guard", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["k14_0085"] = {name = "Natural File Guard", vals = {"Enabled", "Disabled"}, def = 2, ro = true, help = ""},
      ["k14_0086"] = {name = "Security Erase HDD1 Data", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0087"] = {name = "Boot Mode", vals = {"UEFI", "Legacy Support"}, def = 1, ro = false, help = ""},
      ["k14_0088"] = {name = "Boot Priority", vals = {"UEFI First", "Legacy First"}, def = 1, ro = false, help = ""},
      ["k14_0089"] = {name = "Fast Boot", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0090"] = {name = "USB Boot", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0091"] = {name = "PXE Boot to LAN", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0092"] = {name = "IPV4 PXE First", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["k14_0093"] = {name = "EFI", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0094"] = {name = "Windows Boot Manager", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0095"] = {name = "EFI PXE Network", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0096"] = {name = "Exit Saving Changes", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0097"] = {name = "Exit Discarding Changes", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0098"] = {name = "Discard Changes", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0099"] = {name = "Save Changes", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0100"] = {name = "Load Default Settings", vals = {""}, def = 1, ro = true, help = ""},
      ["k14_0101"] = {name = "OS Optimized Defaults", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
    },
  },
  {
    key = "legion", short = "拯救者", icon = "ibm",
    label = "联想 拯救者 Legion Pro 7",
    title = "Lenovo BIOS Setup  Lenovo Legion Pro 7 16IRX8H (82WQ)", thint = "",
    menus = {"Information", "Configuration", "Security", "Boot", "Exit"},
    pages = {
      ["Information"] = {"legion_0001", "legion_0002", "legion_0003", "legion_0004", "legion_0005", "legion_0006", "legion_0007", "legion_0008", "legion_0009", "legion_0010", "legion_0011", "legion_0012", "legion_0013"},
      ["Configuration"] = {"legion_0014", "legion_0015", "legion_0016", "legion_0017", "legion_0018", "legion_0019", "legion_0020", "legion_0021", "legion_0022", "legion_0023", "legion_0024", "legion_0025", "legion_0026", "legion_0027", "legion_0028", "legion_0029", "legion_0030", "legion_0031", "legion_0032", "legion_0033", "legion_0034", "legion_0035", "legion_0036", "legion_0037"},
      ["Security"] = {"legion_0038", "legion_0039", "legion_0040", "legion_0041", "legion_0042", "legion_0043", "legion_0044", "legion_0045", "legion_0046", "legion_0047", "legion_0048", "legion_0049", "legion_0050", "legion_0051", "legion_0052"},
      ["Boot"] = {"legion_0053", "legion_0054", "legion_0055", "legion_0056", "legion_0057"},
      ["Exit"] = {"legion_0058", "legion_0059", "legion_0060", "legion_0061", "legion_0062", "legion_0063"},
    },
    subpages = {
    },
    items = {
      ["legion_0001"] = {name = "Product Name", vals = {"Lenovo Legion Pro 7 16IRX8H"}, def = 1, ro = true, help = ""},
      ["legion_0002"] = {name = "BIOS Version", vals = {"KWCNXXWW"}, def = 1, ro = true, help = ""},
      ["legion_0003"] = {name = "EC Version", vals = {"KWECXXWW"}, def = 1, ro = true, help = ""},
      ["legion_0004"] = {name = "MTM", vals = {"82WQ"}, def = 1, ro = true, help = ""},
      ["legion_0005"] = {name = "Lenovo SN", vals = {"PF123456"}, def = 1, ro = true, help = ""},
      ["legion_0006"] = {name = "UUID Number", vals = {"CB95E7F4-1234-ABCD-1234-ABCD1234ABCD"}, def = 1, ro = true, help = ""},
      ["legion_0007"] = {name = "CPU", vals = {"Intel(R) Core(TM)"}, def = 1, ro = true, help = ""},
      ["legion_0008"] = {name = "System Memory", vals = {"8192 MB"}, def = 1, ro = true, help = ""},
      ["legion_0009"] = {name = "NVME SSD", vals = {"XXXX"}, def = 1, ro = true, help = ""},
      ["legion_0010"] = {name = "Preinstalled OS license", vals = {"NO DPK"}, def = 1, ro = true, help = ""},
      ["legion_0011"] = {name = "OA3 Key ID", vals = {"0000000"}, def = 1, ro = true, help = ""},
      ["legion_0012"] = {name = "Secure Boot", vals = {"Enabled"}, def = 1, ro = true, help = ""},
      ["legion_0013"] = {name = "Language", vals = {"English", "中文"}, def = 1, ro = false, help = ""},
      ["legion_0014"] = {name = "System Time", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0015"] = {name = "System Date", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0016"] = {name = "Wireless LAN", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0017"] = {name = "Storage Controller Mode", vals = {"AHCI"}, def = 1, ro = false, help = ""},
      ["legion_0018"] = {name = "Graphic Device", vals = {"Dynamic Graphics", "Desicrete Graphics"}, def = 1, ro = false, help = ""},
      ["legion_0019"] = {name = "Intel(R) Virtualization Technology", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0020"] = {name = "Intel(R) VT-d Feature", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0021"] = {name = "* Unselectable for Device Guard", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0022"] = {name = "Intel(R) Hyper-Threading Technology", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0023"] = {name = "BIOS Back Flash", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["legion_0024"] = {name = "Fool Proof Fn Ctrl", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0025"] = {name = "Always On USB", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0026"] = {name = "Charge in Battery Mode", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0027"] = {name = "Disable Built-in Battery", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["legion_0028"] = {name = "Flip to Boot", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["legion_0029"] = {name = "BIOS Self-Healing", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0030"] = {name = "Restore Default Overclocking", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["legion_0031"] = {name = "Instant boot", vals = {"Adapter and Type-C Power Delivery", "Adapter", "Type-C Power Delivery", "Disabled"}, def = 4, ro = false, help = ""},
      ["legion_0032"] = {name = "Active Atom Cores", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0033"] = {name = "Legacy Game Compatibility Mode", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["legion_0034"] = {name = "CPU Unlock", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0035"] = {name = "GPU Overclocking", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0036"] = {name = "Performance Mode Fan Level", vals = {"", ""}, def = 1, ro = false, help = ""},
      ["legion_0037"] = {name = "Smart Mode", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0038"] = {name = "Administrator Password", vals = {"Not Set"}, def = 1, ro = true, help = ""},
      ["legion_0039"] = {name = "User Password", vals = {"Not Set"}, def = 1, ro = true, help = ""},
      ["legion_0040"] = {name = "HDD Password", vals = {"Not Set"}, def = 1, ro = true, help = ""},
      ["legion_0041"] = {name = "Set Administrator Password", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0042"] = {name = "Set Hard Disk Passwords", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0043"] = {name = "Security Erase NVME SSD1 Data", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0044"] = {name = "Intel Platform Trust Technology", vals = {"MFG MODE"}, def = 1, ro = true, help = ""},
      ["legion_0045"] = {name = "Device Guard", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["legion_0046"] = {name = "Secure Boot", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0047"] = {name = "Secure Boot Status", vals = {"Enabled"}, def = 1, ro = true, help = ""},
      ["legion_0048"] = {name = "Platform Mode", vals = {"User Mode"}, def = 1, ro = true, help = ""},
      ["legion_0049"] = {name = "Secure Boot Mode", vals = {"Standard"}, def = 1, ro = true, help = ""},
      ["legion_0050"] = {name = "Reset to Setup Mode", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["legion_0051"] = {name = "Restore Factory Keys", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["legion_0052"] = {name = "* Unselectable for Device Guard", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0053"] = {name = "USB Boot", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0054"] = {name = "PXE Boot to LAN", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0055"] = {name = "IPV4 PXE First", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["legion_0056"] = {name = "EFI", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0057"] = {name = "* Unselectable for Device Guard", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0058"] = {name = "Exit Saving Changes", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0059"] = {name = "Exit Discarding Changes", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0060"] = {name = "Discard Changes", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0061"] = {name = "Save Changes", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0062"] = {name = "Load Default Settings", vals = {""}, def = 1, ro = true, help = ""},
      ["legion_0063"] = {name = "OS Optimized Defaults", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
    },
  },
  {
    key = "award", short = "Award", icon = "ibm",
    label = "Award BIOS",
    title = "Award BIOS", thint = "",
    menus = {"Standard CMOS Features", "Advanced BIOS Features", "Advanced Chipset Features", "Integrated Peripherals", "Power Management Setup", "PnP/PCI Configurations", "PC Health Status", "Frequency/Voltage Control", "Main Menu"},
    pages = {
      ["Standard CMOS Features"] = {"award_0001", "award_0002", "@IDE Primary Master", "@IDE Primary Slave", "@IDE Secondary Master", "@IDE Secondary Slave", "award_0003", "award_0004", "award_0005", "award_0006", "award_0007", "award_0008", "award_0009"},
      ["Advanced BIOS Features"] = {"award_0010", "award_0011", "award_0012", "award_0013", "award_0014", "award_0015", "award_0016", "award_0017", "award_0018", "award_0019", "award_0020", "award_0021", "award_0022", "award_0023", "award_0024", "award_0025", "award_0026"},
      ["Advanced Chipset Features"] = {"award_0027", "award_0028", "award_0029", "award_0030", "award_0031", "award_0032", "award_0033", "award_0034", "award_0035", "award_0036"},
      ["Integrated Peripherals"] = {"award_0037", "award_0038", "award_0039", "award_0040", "award_0041", "award_0042", "award_0043", "@OnChip IDE Device", "award_0044", "award_0045", "award_0046", "award_0047", "@Onboard Device", "award_0048", "award_0049", "award_0050", "award_0051", "award_0052", "@SuperIO Device"},
      ["Power Management Setup"] = {"award_0053", "award_0054", "award_0055", "award_0056", "award_0057", "award_0058", "award_0059", "award_0060", "award_0061"},
      ["PnP/PCI Configurations"] = {"award_0062", "award_0063", "@IRQ Resources", "award_0064"},
      ["PC Health Status"] = {"award_0065", "award_0066", "award_0067", "award_0068", "award_0069", "award_0070", "award_0071", "award_0072"},
      ["Frequency/Voltage Control"] = {"award_0073", "award_0074", "award_0075"},
      ["Main Menu"] = {"award_0076", "award_0077", "award_0078", "award_0079", "award_0080", "award_0081", "award_0082"},
    },
    subpages = {
      ["IDE Primary Master"] = {},
      ["IDE Primary Slave"] = {},
      ["IDE Secondary Master"] = {},
      ["IDE Secondary Slave"] = {},
      ["OnChip IDE Device"] = {},
      ["Onboard Device"] = {},
      ["SuperIO Device"] = {},
      ["IRQ Resources"] = {},
    },
    items = {
      ["award_0001"] = {name = "Date (mm:dd:yy)", vals = {"mm:dd:yy"}, def = 1, ro = false, help = ""},
      ["award_0002"] = {name = "Time (hh:mm:ss)", vals = {"hh:mm:ss"}, def = 1, ro = false, help = ""},
      ["award_0003"] = {name = "Drive A", vals = {"None", "360K, 5.25 in.", "1.2M, 5.25 in.", "720K, 3.5 in.", "1.44M, 3.5 in.", "2.88M, 3.5 in."}, def = 5, ro = false, help = ""},
      ["award_0004"] = {name = "Drive B", vals = {"None", "360K, 5.25 in.", "1.2M, 5.25 in.", "720K, 3.5 in.", "1.44M, 3.5 in.", "2.88M, 3.5 in."}, def = 1, ro = false, help = ""},
      ["award_0005"] = {name = "Video", vals = {"EGA/VGA", "CGA 40", "CGA 80", "MONO"}, def = 1, ro = false, help = ""},
      ["award_0006"] = {name = "Halt On", vals = {"All Errors", "No Errors", "All, But Keyboard", "All, But Diskette", "All, But Disk/Key"}, def = 3, ro = false, help = ""},
      ["award_0007"] = {name = "Base Memory", vals = {"640K"}, def = 1, ro = true, help = ""},
      ["award_0008"] = {name = "Total Memory", vals = {"65536K"}, def = 1, ro = true, help = ""},
      ["award_0009"] = {name = "Extended Memory", vals = {"64512K"}, def = 1, ro = true, help = ""},
      ["award_0010"] = {name = "Virus Warning", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["award_0011"] = {name = "CPU L1 & L2 Cache", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0012"] = {name = "CPU L2 Cache ECC Checking", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0013"] = {name = "Quick Power On Self Test", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0014"] = {name = "First Boot Device", vals = {"Floppy", "LS120", "HDD-0", "SCSI", "CDROM", "HDD-1", "HDD-2", "HDD-3", "ZIP100", "USB-FDD", "USB-ZIP", "USB-CDROM", "USB-HDD", "LAN", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0015"] = {name = "Second Boot Device", vals = {"Floppy", "LS120", "HDD-0", "SCSI", "CDROM", "HDD-1", "HDD-2", "HDD-3", "ZIP100", "USB-FDD", "USB-ZIP", "USB-CDROM", "USB-HDD", "LAN", "Disabled"}, def = 3, ro = false, help = ""},
      ["award_0016"] = {name = "Third Boot Device", vals = {"Floppy", "LS120", "HDD-0", "SCSI", "CDROM", "HDD-1", "HDD-2", "HDD-3", "ZIP100", "USB-FDD", "USB-ZIP", "USB-CDROM", "USB-HDD", "LAN", "Disabled"}, def = 2, ro = false, help = ""},
      ["award_0017"] = {name = "Boot Other Device", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0018"] = {name = "Swap Floppy Drive", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["award_0019"] = {name = "Boot Up Floppy Seek", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0020"] = {name = "Boot Up NumLock Status", vals = {"On", "Off"}, def = 1, ro = false, help = ""},
      ["award_0021"] = {name = "Gate A20 Option", vals = {"Fast", "Normal"}, def = 1, ro = false, help = ""},
      ["award_0022"] = {name = "Typematic Rate Setting", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["award_0023"] = {name = "Security Option", vals = {"Setup", "System"}, def = 1, ro = false, help = ""},
      ["award_0024"] = {name = "OS Select For DRAM > 64MB", vals = {"Non-OS2", "OS2"}, def = 1, ro = false, help = ""},
      ["award_0025"] = {name = "HDD S.M.A.R.T. Capability", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["award_0026"] = {name = "Full Screen LOGO Show", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0027"] = {name = "DRAM Timing Selectable", vals = {"By SPD", "Manual"}, def = 1, ro = false, help = ""},
      ["award_0028"] = {name = "CAS Latency Time", vals = {"Auto", "2", "2.5", "3"}, def = 1, ro = false, help = ""},
      ["award_0029"] = {name = "Active to Precharge Delay", vals = {"Auto", "5", "6", "7"}, def = 1, ro = false, help = ""},
      ["award_0030"] = {name = "DRAM RAS# to CAS# Delay", vals = {"Auto", "2", "3", "4"}, def = 1, ro = false, help = ""},
      ["award_0031"] = {name = "DRAM RAS# Precharge", vals = {"Auto", "2", "3", "4"}, def = 1, ro = false, help = ""},
      ["award_0032"] = {name = "System Memory Frequency", vals = {"Auto", "100MHz", "133MHz"}, def = 1, ro = false, help = ""},
      ["award_0033"] = {name = "AGP Aperture Size (MB)", vals = {"4", "8", "16", "32", "64", "128", "256"}, def = 5, ro = false, help = ""},
      ["award_0034"] = {name = "On-Chip Frame Buffer Size", vals = {"1MB", "8MB"}, def = 1, ro = false, help = ""},
      ["award_0035"] = {name = "On-Chip Video Window Size", vals = {"32MB", "64MB", "128MB"}, def = 2, ro = false, help = ""},
      ["award_0036"] = {name = "Init Display First", vals = {"PCI Slot", "Onboard/AGP"}, def = 1, ro = false, help = ""},
      ["award_0037"] = {name = "On-Chip Primary PCI IDE", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0038"] = {name = "On-Chip Secondary PCI IDE", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0039"] = {name = "IDE Primary Master PIO", vals = {"Auto", "Mode 0", "Mode 1", "Mode 2", "Mode 3", "Mode 4"}, def = 1, ro = false, help = ""},
      ["award_0040"] = {name = "IDE Primary Master UDMA", vals = {"Auto", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0041"] = {name = "IDE Primary Slave UDMA", vals = {"Auto", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0042"] = {name = "IDE Secondary Master UDMA", vals = {"Auto", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0043"] = {name = "IDE Secondary Slave UDMA", vals = {"Auto", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0044"] = {name = "USB Controller", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0045"] = {name = "USB Keyboard Support", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["award_0046"] = {name = "AC97 Audio", vals = {"Auto", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0047"] = {name = "Onboard LAN", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0048"] = {name = "Onboard FDC Controller", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0049"] = {name = "Onboard Serial Port 1", vals = {"3F8/IRQ4", "2F8/IRQ3", "3E8/IRQ4", "2E8/IRQ3", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0050"] = {name = "Onboard Serial Port 2", vals = {"3F8/IRQ4", "2F8/IRQ3", "3E8/IRQ4", "2E8/IRQ3", "Disabled"}, def = 2, ro = false, help = ""},
      ["award_0051"] = {name = "Onboard Parallel Port", vals = {"378/IRQ7", "278/IRQ5", "3BC/IRQ7", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0052"] = {name = "Parallel Port Mode", vals = {"SPP", "EPP", "ECP", "ECP+EPP"}, def = 1, ro = false, help = ""},
      ["award_0053"] = {name = "ACPI Function", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0054"] = {name = "ACPI Suspend Type", vals = {"S1(POS)", "S3(STR)"}, def = 1, ro = false, help = ""},
      ["award_0055"] = {name = "Power Management", vals = {"User Define", "Min Saving", "Max Saving"}, def = 1, ro = false, help = ""},
      ["award_0056"] = {name = "Video Off Method", vals = {"DPMS", "Blank Screen", "V/H SYNC+Blank"}, def = 1, ro = false, help = ""},
      ["award_0057"] = {name = "MODEM Use IRQ", vals = {"3", "4", "5", "7", "9", "10", "11", "NA"}, def = 1, ro = false, help = ""},
      ["award_0058"] = {name = "Soft-Off by PWRBTN", vals = {"Instant-Off", "Delay 4 Sec"}, def = 1, ro = false, help = ""},
      ["award_0059"] = {name = "Wake-Up by PCI Card", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["award_0060"] = {name = "Power On by Ring", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["award_0061"] = {name = "Resume by Alarm", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["award_0062"] = {name = "Reset Configuration Data", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["award_0063"] = {name = "Resources Controlled By", vals = {"Auto(ESCD)", "Manual"}, def = 1, ro = false, help = ""},
      ["award_0064"] = {name = "PCI/VGA Palette Snoop", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["award_0065"] = {name = "System Temperature", vals = {"34C/93F"}, def = 1, ro = true, help = ""},
      ["award_0066"] = {name = "CPU Temperature", vals = {"45C/113F"}, def = 1, ro = true, help = ""},
      ["award_0067"] = {name = "CPU Fan Speed", vals = {"3200 RPM"}, def = 1, ro = true, help = ""},
      ["award_0068"] = {name = "Vcore", vals = {"1.72 V"}, def = 1, ro = true, help = ""},
      ["award_0069"] = {name = "Vcc3", vals = {"3.34 V"}, def = 1, ro = true, help = ""},
      ["award_0070"] = {name = "Vcc5", vals = {"5.02 V"}, def = 1, ro = true, help = ""},
      ["award_0071"] = {name = "+12V", vals = {"12.16 V"}, def = 1, ro = true, help = ""},
      ["award_0072"] = {name = "Shutdown Temperature", vals = {"Disabled", "60C/140F", "65C/149F", "70C/158F"}, def = 1, ro = false, help = ""},
      ["award_0073"] = {name = "CPU Clock Ratio", vals = {"Auto", "8 X", "9 X", "10 X", "11 X", "12 X"}, def = 1, ro = false, help = ""},
      ["award_0074"] = {name = "Auto Detect PCI Clk", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["award_0075"] = {name = "Spread Spectrum", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["award_0076"] = {name = "Select Language", vals = {"English", "中文"}, def = 1, ro = false, help = ""},
      ["award_0077"] = {name = "Load Fail-Safe Defaults", vals = {"[Enter]"}, def = 1, ro = true, help = "载入最安全的(稳定)默认值。"},
      ["award_0078"] = {name = "Load Optimized Defaults", vals = {"[Enter]"}, def = 1, ro = true, help = "载入出厂优化默认值(性能最佳)。"},
      ["award_0079"] = {name = "Set Supervisor Password", vals = {"[Enter]"}, def = 1, ro = true, help = "设置/更改/取消管理员密码，限制对 Setup 的访问。"},
      ["award_0080"] = {name = "Set User Password", vals = {"[Enter]"}, def = 1, ro = true, help = "设置/更改/取消用户密码；已设 Supervisor 时用户只能查看不能修改。"},
      ["award_0081"] = {name = "Save & Exit Setup", vals = {"[Enter]"}, def = 1, ro = true, help = "保存 CMOS 设置并退出 (F10)。"},
      ["award_0082"] = {name = "Exit Without Saving", vals = {"[Enter]"}, def = 1, ro = true, help = "放弃所有更改并退出。"},
    },
  },
  {
    key = "ami", short = "AMI", icon = "ibm",
    label = "AMI Aptio Setup Utility",
    title = "AMI Aptio Setup Utility", thint = "",
    menus = {"Main", "Advanced", "Chipset", "Security", "Boot", "Save & Exit"},
    pages = {
      ["Main"] = {"ami_0001", "ami_0002", "ami_0003", "ami_0004", "ami_0005", "ami_0006", "ami_0007", "ami_0008", "ami_0009"},
      ["Advanced"] = {"ami_0010", "ami_0011", "ami_0012", "@ACPI Settings", "ami_0013", "@Trusted Computing", "ami_0014", "ami_0015", "ami_0016", "ami_0017", "@CPU Configuration", "ami_0018", "ami_0019", "@SATA Configuration", "@PCH-FW Configuration", "ami_0020", "ami_0021", "@USB Configuration", "@H/W Monitor", "ami_0022", "@Serial Port 0 Configuration", "ami_0023", "@Serial Port 1 Configuration", "ami_0024", "@Parallel Port Configuration", "@Super IO Configuration"},
      ["Chipset"] = {"ami_0025", "ami_0026", "ami_0027", "ami_0028", "@Graphics Configuration", "@Memory Configuration", "@System Agent (SA) Configuration", "ami_0029", "@PCH-IO Configuration"},
      ["Security"] = {"ami_0030", "ami_0031", "ami_0032", "ami_0033", "ami_0034", "@Key Management"},
      ["Boot"] = {"ami_0035", "ami_0036", "ami_0037", "ami_0038", "ami_0039", "@Boot Option Priorities"},
      ["Save & Exit"] = {"ami_0040", "ami_0041", "ami_0042", "ami_0043", "ami_0044", "ami_0045", "ami_0046", "ami_0047", "ami_0048", "@Boot Override"},
    },
    subpages = {
      ["ACPI Settings"] = {},
      ["Trusted Computing"] = {},
      ["CPU Configuration"] = {},
      ["SATA Configuration"] = {},
      ["PCH-FW Configuration"] = {},
      ["USB Configuration"] = {},
      ["H/W Monitor"] = {},
      ["Super IO Configuration"] = {},
      ["Serial Port 0 Configuration"] = {},
      ["Serial Port 1 Configuration"] = {},
      ["Parallel Port Configuration"] = {},
      ["System Agent (SA) Configuration"] = {},
      ["Graphics Configuration"] = {},
      ["Memory Configuration"] = {},
      ["PCH-IO Configuration"] = {},
      ["Key Management"] = {},
      ["Boot Option Priorities"] = {},
      ["Boot Override"] = {},
    },
    items = {
      ["ami_0001"] = {name = "BIOS Vendor", vals = {"American Megatrends Inc."}, def = 1, ro = true, help = ""},
      ["ami_0002"] = {name = "Core Version", vals = {"5.x"}, def = 1, ro = true, help = ""},
      ["ami_0003"] = {name = "Compliancy", vals = {"UEFI 2.x; PI 1.x"}, def = 1, ro = true, help = ""},
      ["ami_0004"] = {name = "Project Version", vals = {"Aptio V"}, def = 1, ro = true, help = ""},
      ["ami_0005"] = {name = "Build Date", vals = {"01/01/2020"}, def = 1, ro = true, help = ""},
      ["ami_0006"] = {name = "Access Level", vals = {"Administrator"}, def = 1, ro = true, help = ""},
      ["ami_0007"] = {name = "System Language", vals = {"English", "简体中文"}, def = 1, ro = false, help = "选择系统默认语言。"},
      ["ami_0008"] = {name = "System Date", vals = {"MM/DD/YY"}, def = 1, ro = false, help = ""},
      ["ami_0009"] = {name = "System Time", vals = {"HH:MM:SS"}, def = 1, ro = false, help = ""},
      ["ami_0010"] = {name = "Launch PXE OpROM", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "系统启动时是否启用板载网卡的 Boot ROM 功能(PXE 网络引导)。"},
      ["ami_0011"] = {name = "Launch Storage OpROM", vals = {"Disabled", "Enabled"}, def = 2, ro = false, help = "是否启用传统大容量存储设备(UEFI/legacy Option ROM)引导。"},
      ["ami_0012"] = {name = "ACPI Sleep State", vals = {"Suspend Disabled", "S1 (CPU Stop Clock)", "S3 (Suspend to RAM)"}, def = 3, ro = false, help = ""},
      ["ami_0013"] = {name = "TPM Support", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "启用/禁用 TPM。禁用后操作系统将看不到 TPM(需重置平台)。"},
      ["ami_0014"] = {name = "Brand String", vals = {"Intel(R) Core(TM) Processor"}, def = 1, ro = true, help = ""},
      ["ami_0015"] = {name = "Intel Virtualization Technology", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["ami_0016"] = {name = "Hyper-Threading", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["ami_0017"] = {name = "Active Processor Cores", vals = {"All", "1", "2", "4"}, def = 1, ro = false, help = ""},
      ["ami_0018"] = {name = "SATA Controller(s)", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["ami_0019"] = {name = "SATA Mode Selection", vals = {"IDE Mode", "AHCI Mode", "RAID Mode"}, def = 2, ro = false, help = "SATA 工作模式：IDE=老兼容；AHCI=标准(首选)；RAID=Intel RST。"},
      ["ami_0020"] = {name = "Legacy USB Support", vals = {"Enabled", "Disabled", "Auto"}, def = 1, ro = false, help = "传统操作系统下的 USB 支持。Auto=无 USB 设备时关闭传统支持。"},
      ["ami_0021"] = {name = "USB 3.0 Support", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["ami_0022"] = {name = "Serial Port", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["ami_0023"] = {name = "Serial Port", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["ami_0024"] = {name = "Parallel Port", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["ami_0025"] = {name = "Primary Display", vals = {"IGFX", "PEG", "PCI", "SG"}, def = 1, ro = false, help = ""},
      ["ami_0026"] = {name = "Primary IGFX Boot Display", vals = {"CRT", "DDI1", "LFP", "DDI3", "DDI2"}, def = 1, ro = false, help = ""},
      ["ami_0027"] = {name = "Secondary IGFX Boot Display", vals = {"Disabled", "CRT", "DDI1", "LFP", "DDI3", "DDI2"}, def = 1, ro = false, help = ""},
      ["ami_0028"] = {name = "LCD Panel Type", vals = {"VBIOS Default"}, def = 1, ro = false, help = ""},
      ["ami_0029"] = {name = "Wake on LAN", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = "是否允许集成网卡唤醒系统。"},
      ["ami_0030"] = {name = "Administrator Password", vals = {"Not Installed"}, def = 1, ro = true, help = ""},
      ["ami_0031"] = {name = "User Password", vals = {"Not Installed"}, def = 1, ro = true, help = ""},
      ["ami_0032"] = {name = "Set Administrator Password", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["ami_0033"] = {name = "Set User Password", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["ami_0034"] = {name = "Secure Boot", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["ami_0035"] = {name = "Setup Prompt Timeout", vals = {"1", "3", "5", "65535"}, def = 1, ro = false, help = "等待进入 Setup 的秒数。65535(0xFFFF)=无限等待。"},
      ["ami_0036"] = {name = "Bootup NumLock State", vals = {"On", "Off"}, def = 1, ro = false, help = ""},
      ["ami_0037"] = {name = "Quiet Boot", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["ami_0038"] = {name = "Fast Boot", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["ami_0039"] = {name = "CSM16 Module Version", vals = {"07.64"}, def = 1, ro = true, help = ""},
      ["ami_0040"] = {name = "Save Changes and Exit", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["ami_0041"] = {name = "Discard Changes and Exit", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["ami_0042"] = {name = "Save Changes and Reset", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["ami_0043"] = {name = "Discard Changes and Reset", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["ami_0044"] = {name = "Save Changes", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["ami_0045"] = {name = "Discard Changes", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["ami_0046"] = {name = "Restore Defaults", vals = {"[Enter]"}, def = 1, ro = true, help = "载入全部选项的默认值。"},
      ["ami_0047"] = {name = "Save as User Defaults", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["ami_0048"] = {name = "Restore User Defaults", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
    },
  },
  {
    key = "asus", short = "华硕", icon = "ibm",
    label = "ASUS UEFI BIOS Utility",
    title = "ASUS UEFI BIOS Utility", thint = "",
    menus = {"My Favorites", "Main", "Ai Tweaker", "Advanced", "Monitor", "Boot", "Tool", "Exit"},
    pages = {
      ["My Favorites"] = {},
      ["Main"] = {"asus_0001", "asus_0002", "asus_0003", "asus_0004", "asus_0005", "asus_0006", "@Security"},
      ["Ai Tweaker"] = {"asus_0007", "asus_0008", "asus_0009", "asus_0010", "asus_0011", "asus_0012", "@DRAM Timing Control", "@DIGI+ VRM", "@Internal CPU Power Management", "asus_0013", "asus_0014", "@TDP Configuration", "@Precision Boost Overdrive", "asus_0015"},
      ["Advanced"] = {"@Platform Misc Configuration", "asus_0016", "asus_0017", "asus_0018", "asus_0019", "@CPU Configuration", "asus_0020", "@Graphics Configuration", "@Memory Configuration", "@System Agent (SA) Configuration", "@PCH Configuration", "asus_0021", "@SATA Configuration", "@PCH Storage Configuration", "@PCH-FW Configuration", "asus_0022", "@Thunderbolt(TM) Configuration", "@PCI Subsystem Settings", "asus_0023", "asus_0024", "@USB Single Port Control", "@USB Configuration", "asus_0025", "@Network Stack Configuration", "@NVMe Configuration", "@HDD/SSD SMART Information", "asus_0026", "asus_0027", "asus_0028", "@APM Configuration", "@Onboard Devices Configuration", "@Intel(R) Rapid Storage Technology"},
      ["Monitor"] = {"asus_0029", "asus_0030", "asus_0031", "asus_0032", "asus_0033", "asus_0034", "asus_0035", "asus_0036", "@Q-Fan Configuration", "@Chassis Fan(s) Configuration"},
      ["Boot"] = {"asus_0037", "asus_0038", "asus_0039", "asus_0040", "asus_0041", "asus_0042", "asus_0043", "asus_0044", "asus_0045", "asus_0046", "asus_0047", "asus_0048", "@CSM (Compatibility Support Module)", "asus_0049", "asus_0050", "asus_0051", "asus_0052", "asus_0053", "@Key Management", "@Secure Boot", "@Boot Option Priorities", "asus_0054"},
      ["Tool"] = {"@ASUS EZ Flash 3 Utility", "@ASUS Secure Erase", "@ASUS User Profile", "@ASUS SPD Information", "@MemTest86", "@ASUS Armoury Crate", "@MyASUS", "asus_0055", "asus_0056", "asus_0057", "asus_0058"},
      ["Exit"] = {"asus_0059", "asus_0060", "asus_0061", "asus_0062"},
    },
    subpages = {
      ["Security"] = {},
      ["DRAM Timing Control"] = {},
      ["DIGI+ VRM"] = {},
      ["Internal CPU Power Management"] = {},
      ["TDP Configuration"] = {},
      ["Precision Boost Overdrive"] = {},
      ["Platform Misc Configuration"] = {},
      ["CPU Configuration"] = {},
      ["System Agent (SA) Configuration"] = {},
      ["Graphics Configuration"] = {},
      ["Memory Configuration"] = {},
      ["PCH Configuration"] = {},
      ["PCH Storage Configuration"] = {},
      ["SATA Configuration"] = {},
      ["PCH-FW Configuration"] = {},
      ["Thunderbolt(TM) Configuration"] = {},
      ["PCI Subsystem Settings"] = {},
      ["USB Configuration"] = {},
      ["USB Single Port Control"] = {},
      ["Network Stack Configuration"] = {},
      ["NVMe Configuration"] = {},
      ["HDD/SSD SMART Information"] = {},
      ["APM Configuration"] = {},
      ["Onboard Devices Configuration"] = {},
      ["Intel(R) Rapid Storage Technology"] = {},
      ["Q-Fan Configuration"] = {},
      ["Chassis Fan(s) Configuration"] = {},
      ["CSM (Compatibility Support Module)"] = {},
      ["Secure Boot"] = {},
      ["Key Management"] = {},
      ["Boot Option Priorities"] = {},
      ["ASUS EZ Flash 3 Utility"] = {},
      ["ASUS Secure Erase"] = {},
      ["ASUS User Profile"] = {},
      ["ASUS SPD Information"] = {},
      ["MemTest86"] = {},
      ["ASUS Armoury Crate"] = {},
      ["MyASUS"] = {},
    },
    items = {
      ["asus_0001"] = {name = "System Language", vals = {"English", "简体中文"}, def = 1, ro = false, help = ""},
      ["asus_0002"] = {name = "System Date", vals = {"MM/DD/YYYY"}, def = 1, ro = false, help = ""},
      ["asus_0003"] = {name = "System Time", vals = {"HH:MM:SS"}, def = 1, ro = false, help = ""},
      ["asus_0004"] = {name = "Access Level", vals = {"Administrator"}, def = 1, ro = true, help = ""},
      ["asus_0005"] = {name = "Administrator Password", vals = {"[Enter]"}, def = 1, ro = true, help = "默认 [Not Installed]，设置后显示 [Installed]。"},
      ["asus_0006"] = {name = "User Password", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["asus_0007"] = {name = "Ai Overclock Tuner", vals = {"Auto", "Manual", "XMP I", "XMP II", "D.O.C.P."}, def = 1, ro = false, help = ""},
      ["asus_0008"] = {name = "ASUS MultiCore Enhancement", vals = {"Auto - Keep booting with ASUS optimized settings", "Disabled - Enforce All limits", "Enabled - Remove All limits"}, def = 1, ro = false, help = ""},
      ["asus_0009"] = {name = "BCLK Frequency : DRAM Frequency Ratio", vals = {"Auto", "100:133", "100:100"}, def = 1, ro = false, help = ""},
      ["asus_0010"] = {name = "DRAM Frequency", vals = {"Auto", "DDR4-2133MHz", "DDR4-2666MHz", "DDR4-3200MHz"}, def = 1, ro = false, help = ""},
      ["asus_0011"] = {name = "Performance Core Ratio", vals = {"Auto", "Sync All Cores", "By Core Usage"}, def = 1, ro = false, help = ""},
      ["asus_0012"] = {name = "CPU SVID Support", vals = {"Auto", "Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["asus_0013"] = {name = "CPU Core/Cache Voltage", vals = {"Auto", "Manual Mode", "Offset Mode", "Adaptive Mode"}, def = 1, ro = false, help = ""},
      ["asus_0014"] = {name = "DRAM Voltage", vals = {"Auto"}, def = 1, ro = false, help = ""},
      ["asus_0015"] = {name = "GPU Boost", vals = {"Auto", "Disabled"}, def = 1, ro = false, help = ""},
      ["asus_0016"] = {name = "Brand String", vals = {"Intel(R) Core(TM) Processor"}, def = 1, ro = true, help = ""},
      ["asus_0017"] = {name = "Intel (VMX) Virtualization Technology", vals = {"Disabled", "Enabled"}, def = 2, ro = false, help = "启用后 VMM 可使用 Vanderpool 虚拟化技术提供的硬件能力。"},
      ["asus_0018"] = {name = "Hyper-Threading", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["asus_0019"] = {name = "Active Processor Cores", vals = {"All", "1", "2", "4"}, def = 1, ro = false, help = ""},
      ["asus_0020"] = {name = "Primary Display", vals = {"Auto", "IGFX", "PEG", "PCIE"}, def = 1, ro = false, help = ""},
      ["asus_0021"] = {name = "SATA Mode Selection", vals = {"AHCI", "RAID"}, def = 1, ro = false, help = "SATA 工作模式。AHCI=标准；RAID=Intel RST(需在 Tool 里建阵列)。"},
      ["asus_0022"] = {name = "Discrete Thunderbolt(TM) Support", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["asus_0023"] = {name = "Legacy USB Support", vals = {"Enabled", "Disabled", "Auto"}, def = 1, ro = false, help = ""},
      ["asus_0024"] = {name = "XHCI Hand-off", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["asus_0025"] = {name = "Network Stack", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["asus_0026"] = {name = "ErP Ready", vals = {"Disabled", "Enabled(S4+S5)", "Enabled(S5)"}, def = 1, ro = false, help = ""},
      ["asus_0027"] = {name = "Power On By PCI-E", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["asus_0028"] = {name = "Restore AC Power Loss", vals = {"Power Off", "Power On", "Last State"}, def = 1, ro = false, help = ""},
      ["asus_0029"] = {name = "CPU Temperature", vals = {"36C"}, def = 1, ro = true, help = ""},
      ["asus_0030"] = {name = "Motherboard Temperature", vals = {"32C"}, def = 1, ro = true, help = ""},
      ["asus_0031"] = {name = "CPU Fan Speed", vals = {"1200 RPM"}, def = 1, ro = true, help = ""},
      ["asus_0032"] = {name = "Chassis Fan Speed", vals = {"N/A"}, def = 1, ro = true, help = ""},
      ["asus_0033"] = {name = "CPU Core Voltage", vals = {"1.163 V"}, def = 1, ro = true, help = ""},
      ["asus_0034"] = {name = "3.3V Voltage", vals = {"3.312 V"}, def = 1, ro = true, help = ""},
      ["asus_0035"] = {name = "5V Voltage", vals = {"5.040 V"}, def = 1, ro = true, help = ""},
      ["asus_0036"] = {name = "12V Voltage", vals = {"12.192 V"}, def = 1, ro = true, help = ""},
      ["asus_0037"] = {name = "Fast Boot", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["asus_0038"] = {name = "Next Boot after AC Power Loss", vals = {"Normal Boot", "Fast Boot"}, def = 1, ro = false, help = ""},
      ["asus_0039"] = {name = "Boot Logo Display", vals = {"Auto", "Full Screen", "Disabled"}, def = 1, ro = false, help = ""},
      ["asus_0040"] = {name = "Post Delay Time", vals = {"0 sec", "3 sec", "5 sec", "10 sec"}, def = 1, ro = false, help = ""},
      ["asus_0041"] = {name = "Post Report", vals = {"1 sec", "5 sec", "10 sec", "Until Press ESC"}, def = 2, ro = false, help = ""},
      ["asus_0042"] = {name = "Boot up NumLock State", vals = {"On", "Off"}, def = 1, ro = false, help = ""},
      ["asus_0043"] = {name = "Wait For 'F1' If Error", vals = {"Disabled", "Enabled"}, def = 2, ro = false, help = ""},
      ["asus_0044"] = {name = "Option ROM Messages", vals = {"Force BIOS", "Keep Current"}, def = 1, ro = false, help = ""},
      ["asus_0045"] = {name = "Interrupt 19 Capture", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["asus_0046"] = {name = "AMI Native NVMe Driver Support", vals = {"Disabled", "Enabled"}, def = 2, ro = false, help = ""},
      ["asus_0047"] = {name = "Setup Mode", vals = {"Advanced Mode", "EZ Mode"}, def = 1, ro = false, help = ""},
      ["asus_0048"] = {name = "Launch CSM", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = "兼容性支持模块。装 Win7/老系统需 Enabled；纯 UEFI(Win11)设 Disabled。"},
      ["asus_0049"] = {name = "OS Type", vals = {"Windows UEFI mode", "Other OS"}, def = 1, ro = false, help = ""},
      ["asus_0050"] = {name = "Secure Boot Mode", vals = {"Standard", "Custom"}, def = 1, ro = false, help = ""},
      ["asus_0051"] = {name = "Install Default Secure Boot keys", vals = {"Yes", "No"}, def = 2, ro = false, help = ""},
      ["asus_0052"] = {name = "Clear Secure Boot keys", vals = {"Yes", "No"}, def = 2, ro = false, help = ""},
      ["asus_0053"] = {name = "Save all Secure Boot variables", vals = {"Yes", "No"}, def = 2, ro = false, help = ""},
      ["asus_0054"] = {name = "Boot Override", vals = {"（列出可用设备，回车即从所选设备启动）"}, def = 1, ro = true, help = ""},
      ["asus_0055"] = {name = "BIOS Image Rollback Support", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["asus_0056"] = {name = "Publish HII Resources", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["asus_0057"] = {name = "Flexkey", vals = {"Reset", "Aura On/Off", "DirectKey", "Safe Boot"}, def = 1, ro = false, help = ""},
      ["asus_0058"] = {name = "Setup Animator", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["asus_0059"] = {name = "Load Optimized Defaults", vals = {"[Enter]"}, def = 1, ro = true, help = "载入优化默认值 (F5)。"},
      ["asus_0060"] = {name = "Save Changes & Reset", vals = {"[Enter]"}, def = 1, ro = true, help = "保存并重启 (F10)。"},
      ["asus_0061"] = {name = "Discard Changes & Exit", vals = {"[Enter]"}, def = 1, ro = true, help = "放弃更改并退出 (Esc)。"},
      ["asus_0062"] = {name = "Launch EFI Shell from USB drives", vals = {"[Enter]"}, def = 1, ro = true, help = "从文件系统设备启动 EFI Shell (shellx64.efi)。"},
    },
  },
  {
    key = "dell", short = "戴尔", icon = "ibm",
    label = "Dell BIOS Setup",
    title = "Dell BIOS Setup", thint = "",
    menus = {"General", "System Configuration", "Video", "Security", "Secure Boot", "Performance", "Power Management", "POST Behavior", "Virtualization Support", "Wireless", "Maintenance", "System Logs", "SupportAssist System Resolution"},
    pages = {
      ["General"] = {"dell_0001", "dell_0002", "dell_0003", "dell_0004", "dell_0005", "dell_0006", "dell_0007", "dell_0008", "dell_0009", "dell_0010", "dell_0011", "dell_0012", "dell_0013", "dell_0014", "dell_0015", "dell_0016", "dell_0017", "dell_0018", "dell_0019", "dell_0020", "dell_0021", "dell_0022", "dell_0023", "dell_0024", "dell_0025", "dell_0026", "dell_0027", "dell_0028", "dell_0029", "dell_0030", "dell_0031", "dell_0032", "dell_0033", "dell_0034", "dell_0035", "dell_0036", "dell_0037", "dell_0038", "dell_0039", "dell_0040", "@System Information", "dell_0041", "dell_0042", "dell_0043", "@Boot Sequence", "dell_0044", "dell_0045"},
      ["System Configuration"] = {"dell_0046", "dell_0047", "dell_0048", "dell_0049", "dell_0050", "dell_0051", "@Drives", "dell_0052", "dell_0053", "dell_0054", "dell_0055", "dell_0056", "dell_0057", "@USB Configuration", "dell_0058", "@Miscellaneous Devices"},
      ["Video"] = {"dell_0059", "dell_0060"},
      ["Security"] = {"dell_0061", "dell_0062", "dell_0063", "@Password Configuration", "dell_0064", "dell_0065", "dell_0066", "dell_0067", "@PTT Security", "dell_0068", "dell_0069", "dell_0070", "dell_0071", "dell_0072", "dell_0073", "dell_0074", "@Intel Software Guard Extensions"},
      ["Secure Boot"] = {"dell_0075", "dell_0076", "dell_0077", "dell_0078", "dell_0079", "dell_0080", "@Expert Key Management"},
      ["Performance"] = {"dell_0081", "dell_0082", "dell_0083", "dell_0084", "dell_0085"},
      ["Power Management"] = {"dell_0086", "dell_0087", "dell_0088", "dell_0089", "dell_0090", "dell_0091", "dell_0092"},
      ["POST Behavior"] = {"dell_0093", "dell_0094", "dell_0095", "dell_0096", "dell_0097", "dell_0098"},
      ["Virtualization Support"] = {"dell_0099", "dell_0100"},
      ["Wireless"] = {"dell_0101", "dell_0102", "dell_0103", "@Wireless Device Enable"},
      ["Maintenance"] = {"dell_0104", "dell_0105", "dell_0106", "dell_0107", "dell_0108", "dell_0109"},
      ["System Logs"] = {"@BIOS Events"},
      ["SupportAssist System Resolution"] = {"dell_0110", "dell_0111"},
    },
    subpages = {
      ["System Information"] = {},
      ["Boot Sequence"] = {},
      ["Drives"] = {},
      ["USB Configuration"] = {},
      ["Miscellaneous Devices"] = {},
      ["Password Configuration"] = {},
      ["PTT Security"] = {},
      ["Intel Software Guard Extensions"] = {},
      ["Expert Key Management"] = {},
      ["Wireless Device Enable"] = {},
      ["BIOS Events"] = {},
    },
    items = {
      ["dell_0001"] = {name = "System Information", vals = {""}, def = 1, ro = true, help = ""},
      ["dell_0002"] = {name = "BIOS Version", vals = {"1.28.0"}, def = 1, ro = true, help = ""},
      ["dell_0003"] = {name = "Service Tag", vals = {"J4K2L83"}, def = 1, ro = true, help = ""},
      ["dell_0004"] = {name = "Asset Tag", vals = {""}, def = 1, ro = true, help = ""},
      ["dell_0005"] = {name = "Ownership Tag", vals = {""}, def = 1, ro = true, help = ""},
      ["dell_0006"] = {name = "Manufacture Date", vals = {"06/11/2024"}, def = 1, ro = true, help = ""},
      ["dell_0007"] = {name = "Ownership Date", vals = {""}, def = 1, ro = true, help = ""},
      ["dell_0008"] = {name = "Express Service Code", vals = {"12345678901"}, def = 1, ro = true, help = ""},
      ["dell_0009"] = {name = "Memory Information", vals = {""}, def = 1, ro = true, help = ""},
      ["dell_0010"] = {name = "Memory Installed", vals = {"16384 MB"}, def = 1, ro = true, help = ""},
      ["dell_0011"] = {name = "Memory Available", vals = {"16000 MB"}, def = 1, ro = true, help = ""},
      ["dell_0012"] = {name = "Memory Speed", vals = {"3200 MHz"}, def = 1, ro = true, help = ""},
      ["dell_0013"] = {name = "Memory Channel Mode", vals = {"Dual"}, def = 1, ro = true, help = ""},
      ["dell_0014"] = {name = "Memory Technology", vals = {"DDR4"}, def = 1, ro = true, help = ""},
      ["dell_0015"] = {name = "DIMM 1 Size", vals = {"8192 MB"}, def = 1, ro = true, help = ""},
      ["dell_0016"] = {name = "DIMM 2 Size", vals = {"8192 MB"}, def = 1, ro = true, help = ""},
      ["dell_0017"] = {name = "PCI Information", vals = {""}, def = 1, ro = true, help = ""},
      ["dell_0018"] = {name = "SLOT2", vals = {"Empty"}, def = 1, ro = true, help = ""},
      ["dell_0019"] = {name = "SLOT3", vals = {"Empty"}, def = 1, ro = true, help = ""},
      ["dell_0020"] = {name = "SLOT5_M.2", vals = {"M.2 PCIe SSD"}, def = 1, ro = true, help = ""},
      ["dell_0021"] = {name = "Processor Information", vals = {""}, def = 1, ro = true, help = ""},
      ["dell_0022"] = {name = "Processor Type", vals = {"Intel(R) Core(TM) i7-11700"}, def = 1, ro = true, help = ""},
      ["dell_0023"] = {name = "Core Count", vals = {"8"}, def = 1, ro = true, help = ""},
      ["dell_0024"] = {name = "Processor ID", vals = {"0x0000000000000000"}, def = 1, ro = true, help = ""},
      ["dell_0025"] = {name = "Current Clock Speed", vals = {"2500 MHz"}, def = 1, ro = true, help = ""},
      ["dell_0026"] = {name = "Minimum Clock Speed", vals = {"800 MHz"}, def = 1, ro = true, help = ""},
      ["dell_0027"] = {name = "Maximum Clock Speed", vals = {"4900 MHz"}, def = 1, ro = true, help = ""},
      ["dell_0028"] = {name = "Processor L2 Cache", vals = {"4 MB"}, def = 1, ro = true, help = ""},
      ["dell_0029"] = {name = "Processor L3 Cache", vals = {"16 MB"}, def = 1, ro = true, help = ""},
      ["dell_0030"] = {name = "HT Capable", vals = {"Yes"}, def = 1, ro = true, help = ""},
      ["dell_0031"] = {name = "64-Bit Technology", vals = {"Yes"}, def = 1, ro = true, help = ""},
      ["dell_0032"] = {name = "Device Information", vals = {""}, def = 1, ro = true, help = ""},
      ["dell_0033"] = {name = "SATA-0", vals = {"None"}, def = 1, ro = true, help = ""},
      ["dell_0034"] = {name = "SATA-1", vals = {"None"}, def = 1, ro = true, help = ""},
      ["dell_0035"] = {name = "M.2 PCIe SSD-0", vals = {"512 GB"}, def = 1, ro = true, help = ""},
      ["dell_0036"] = {name = "LOM MAC Address", vals = {"00:00:00:00:00:00"}, def = 1, ro = true, help = ""},
      ["dell_0037"] = {name = "Video Controller", vals = {"Intel(R) UHD Graphics 750"}, def = 1, ro = true, help = ""},
      ["dell_0038"] = {name = "Audio Controller", vals = {"Realtek ALC3246"}, def = 1, ro = true, help = ""},
      ["dell_0039"] = {name = "Wi-Fi Device", vals = {"Intel(R) Wi-Fi 6 AX201"}, def = 1, ro = true, help = ""},
      ["dell_0040"] = {name = "Bluetooth Device", vals = {"Intel(R) Bluetooth"}, def = 1, ro = true, help = ""},
      ["dell_0041"] = {name = "Boot Mode", vals = {"UEFI", "Legacy"}, def = 1, ro = false, help = "引导模式。UEFI=纯 UEFI 引导(装 Win10/11 首选)；Legacy=传统引导(需 CSM)。"},
      ["dell_0042"] = {name = "Boot List Option", vals = {"Legacy", "UEFI"}, def = 2, ro = false, help = ""},
      ["dell_0043"] = {name = "Boot Sequence", vals = {"（可启动设备顺序列表：选中设备后用 +/- 或空格调整）"}, def = 1, ro = true, help = ""},
      ["dell_0044"] = {name = "UEFI Boot Path Security", vals = {"Always, Except Internal HDD", "Always", "Never"}, def = 1, ro = false, help = "从 F12 启动菜单引导 UEFI 路径时是否要求输入管理员密码。"},
      ["dell_0045"] = {name = "Date/Time", vals = {"MM/DD/YY HH:MM:SS"}, def = 1, ro = false, help = ""},
      ["dell_0046"] = {name = "Integrated NIC", vals = {"Disabled", "Enabled", "Enabled with PXE"}, def = 3, ro = false, help = "板载网卡控制。Enabled with PXE=启用并支持 PXE 网络引导。"},
      ["dell_0047"] = {name = "Enable UEFI Network Stack", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0048"] = {name = "SATA Operation", vals = {"Disabled", "AHCI", "RAID On"}, def = 2, ro = false, help = "集成 SATA 硬盘控制器工作模式。AHCI=标准模式(装系统首选)；RAID On=Intel RST/RAID。"},
      ["dell_0049"] = {name = "SATA-0", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0050"] = {name = "SATA-1", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0051"] = {name = "M.2 PCIe SSD-0", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0052"] = {name = "SMART Reporting", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = "系统启动时是否启用 SMART 报告。"},
      ["dell_0053"] = {name = "Enable USB Boot Support", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0054"] = {name = "Enable Front USB Ports", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0055"] = {name = "Enable Rear USB Ports", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0056"] = {name = "Front USB Configuration", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0057"] = {name = "Rear USB Configuration", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0058"] = {name = "Audio", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0059"] = {name = "Multi-Display", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0060"] = {name = "Primary Display", vals = {"Auto"}, def = 1, ro = false, help = ""},
      ["dell_0061"] = {name = "Admin Password", vals = {"Not Set"}, def = 1, ro = true, help = ""},
      ["dell_0062"] = {name = "System Password", vals = {"Not Set"}, def = 1, ro = true, help = ""},
      ["dell_0063"] = {name = "Internal HDD-0 Password", vals = {"Not Set"}, def = 1, ro = true, help = ""},
      ["dell_0064"] = {name = "Password Change", vals = {"Permitted", "Denied"}, def = 1, ro = false, help = "设置管理员密码后, 是否允许更改系统与硬盘密码。"},
      ["dell_0065"] = {name = "UEFI Capsule Firmware Updates", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0066"] = {name = "PTT On", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "平台信任技术(PTT)对操作系统的可见性。"},
      ["dell_0067"] = {name = "Clear", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0068"] = {name = "PPI ByPass for Clear Command", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0069"] = {name = "Absolute(R)", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0070"] = {name = "Admin Setup Lockout", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0071"] = {name = "Master Password Lockout", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0072"] = {name = "SMM Security Mitigation", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0073"] = {name = "Intel SGX Enable", vals = {"Enabled", "Software Controlled", "Disabled"}, def = 3, ro = false, help = ""},
      ["dell_0074"] = {name = "Enclave Memory Size", vals = {"32 MB", "64 MB", "128 MB"}, def = 3, ro = false, help = ""},
      ["dell_0075"] = {name = "Secure Boot Enable", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "启用/禁用安全引导。"},
      ["dell_0076"] = {name = "Secure Boot Mode", vals = {"Deployed Mode", "Audit Mode"}, def = 1, ro = false, help = ""},
      ["dell_0077"] = {name = "Deployed Mode", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0078"] = {name = "Audit Mode", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0079"] = {name = "Expert Key Management", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0080"] = {name = "Custom Mode Key Management", vals = {"PK", "KEK", "db", "dbx"}, def = 1, ro = false, help = ""},
      ["dell_0081"] = {name = "Multi Core Support", vals = {"All", "1", "2", "3"}, def = 1, ro = false, help = "启用多个核心。"},
      ["dell_0082"] = {name = "Intel SpeedStep", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0083"] = {name = "C-States Control", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0084"] = {name = "Intel TurboBoost", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0085"] = {name = "HyperThread control", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0086"] = {name = "AC Recovery", vals = {"Power Off", "Power On", "Last Power State"}, def = 3, ro = false, help = ""},
      ["dell_0087"] = {name = "Enable Intel Speed Shift Technology", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0088"] = {name = "Auto On Time", vals = {"Disabled", "Everyday", "Weekdays", "Selected Days"}, def = 1, ro = false, help = ""},
      ["dell_0089"] = {name = "USB Wake Support", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0090"] = {name = "Deep Sleep Control", vals = {"Disabled", "Enabled in S4 and S5", "Enabled in S5 Only"}, def = 1, ro = false, help = ""},
      ["dell_0091"] = {name = "Wake on LAN/WLAN", vals = {"Disabled", "LAN Only", "WLAN Only", "LAN or WLAN"}, def = 1, ro = false, help = ""},
      ["dell_0092"] = {name = "Block Sleep", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0093"] = {name = "Numlock LED", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0094"] = {name = "Keyboard Errors", vals = {"Report", "Do Not Report"}, def = 1, ro = false, help = ""},
      ["dell_0095"] = {name = "Fastboot", vals = {"Minimal", "Thorough", "Auto"}, def = 2, ro = false, help = "设置引导过程的速度。"},
      ["dell_0096"] = {name = "Extend BIOS POST Time", vals = {"0 seconds", "5 seconds", "10 seconds"}, def = 1, ro = false, help = ""},
      ["dell_0097"] = {name = "Full Screen Logo", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0098"] = {name = "Warnings and Errors", vals = {"Prompt on Warnings and Errors", "Continue on Warnings", "Continue on Warnings and Errors"}, def = 1, ro = false, help = ""},
      ["dell_0099"] = {name = "Virtualization", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = "指定 VMM 是否可使用 Intel 虚拟化技术提供的硬件功能。"},
      ["dell_0100"] = {name = "VT for Direct I/O", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0101"] = {name = "WLAN", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0102"] = {name = "Bluetooth", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0103"] = {name = "WWAN", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
      ["dell_0104"] = {name = "Service Tag", vals = {"J4K2L83"}, def = 1, ro = true, help = ""},
      ["dell_0105"] = {name = "Asset Tag", vals = {""}, def = 1, ro = false, help = ""},
      ["dell_0106"] = {name = "SERR Messages", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0107"] = {name = "BIOS Downgrade", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0108"] = {name = "Data Wipe", vals = {"[Enter]"}, def = 1, ro = true, help = "安全擦除所有内部存储设备中的数据。"},
      ["dell_0109"] = {name = "BIOS Recovery", vals = {"Enabled", "Disabled"}, def = 1, ro = false, help = ""},
      ["dell_0110"] = {name = "Auto OS Recovery Threshold", vals = {"Off", "1", "2", "3"}, def = 3, ro = false, help = ""},
      ["dell_0111"] = {name = "SupportAssist OS Recovery", vals = {"Enabled", "Disabled"}, def = 2, ro = false, help = ""},
    },
  },
  {
    key = "hp", short = "惠普", icon = "ibm",
    label = "HP BIOS Setup Utility",
    title = "HP BIOS Setup Utility", thint = "",
    menus = {"Main", "Security", "Advanced", "UEFI Drivers"},
    pages = {
      ["Main"] = {"hp_0001", "hp_0002", "hp_0003", "hp_0004", "hp_0005", "hp_0006", "hp_0007", "hp_0008", "hp_0009", "hp_0010", "hp_0011", "@System Information", "hp_0012", "hp_0013", "hp_0014", "@Set Machine Unique Data", "@System IDs", "@Replicated Setup", "hp_0015", "hp_0016", "hp_0017", "hp_0018", "hp_0019", "hp_0020"},
      ["Security"] = {"hp_0021", "hp_0022", "@Password Policy", "hp_0023", "@Administrator Tools", "@TPM Embedded Security", "@BIOS SureStart", "@HP Secure Platform Management (SPM)", "hp_0024", "hp_0025", "@Security Configuration", "@Hard Drive Utilities", "@Utilities", "hp_0026", "hp_0027", "hp_0028", "hp_0029"},
      ["Advanced"] = {"hp_0030", "@Scheduled Power-On", "@Boot Options", "@OS Recovery", "@Secure Boot Configuration", "@System Options", "@Built-In Device Options", "@Port Options", "@Option ROM Launch Policy", "@Power Management Options", "@Electronic Labels", "@MAC Address Pass Through", "@Remote HP PC Hardware Diagnostics", "hp_0031"},
      ["UEFI Drivers"] = {"hp_0032"},
    },
    subpages = {
      ["System Information"] = {},
      ["Set Machine Unique Data"] = {},
      ["System IDs"] = {},
      ["Replicated Setup"] = {},
      ["Administrator Tools"] = {},
      ["Password Policy"] = {},
      ["Security Configuration"] = {},
      ["TPM Embedded Security"] = {},
      ["BIOS SureStart"] = {},
      ["HP Secure Platform Management (SPM)"] = {},
      ["Utilities"] = {},
      ["Hard Drive Utilities"] = {},
      ["Scheduled Power-On"] = {},
      ["Boot Options"] = {},
      ["OS Recovery"] = {},
      ["Secure Boot Configuration"] = {},
      ["System Options"] = {},
      ["Built-In Device Options"] = {},
      ["Port Options"] = {},
      ["Option ROM Launch Policy"] = {},
      ["Power Management Options"] = {},
      ["Electronic Labels"] = {},
      ["MAC Address Pass Through"] = {},
      ["Remote HP PC Hardware Diagnostics"] = {},
    },
    items = {
      ["hp_0001"] = {name = "Show Advanced System Information", vals = {"Disabled", "Enabled"}, def = 1, ro = false, help = ""},
      ["hp_0002"] = {name = "Product Name", vals = {"HP EliteBook 830/840/840 HC/850 G5 Notebook PCs"}, def = 1, ro = true, help = ""},
      ["hp_0003"] = {name = "Processor 1", vals = {"Intel(R) Core(TM) i5-8250U CPU @ 1.60GHz"}, def = 1, ro = true, help = ""},
      ["hp_0004"] = {name = "Memory Size", vals = {"16384 MB"}, def = 1, ro = true, help = ""},
      ["hp_0005"] = {name = "System BIOS", vals = {"Q78 Ver.80.31.00 10/16/2017"}, def = 1, ro = true, help = ""},
      ["hp_0006"] = {name = "SERVICE ---------------------------------------------------------------", vals = {""}, def = 1, ro = true, help = ""},
      ["hp_0007"] = {name = "Born On Date", vals = {"00/00/0000"}, def = 1, ro = true, help = ""},
      ["hp_0008"] = {name = "Serial Number", vals = {"00074400PK"}, def = 1, ro = true, help = ""},
      ["hp_0009"] = {name = "SKU Number", vals = {"123456#ABA"}, def = 1, ro = true, help = ""},
      ["hp_0010"] = {name = "Universally Unique Identifier (UUID)", vals = {"6757586A-357D-8BB9-BC26-E08933445044"}, def = 1, ro = true, help = ""},
      ["hp_0011"] = {name = "Asset Tracking Number", vals = {""}, def = 1, ro = true, help = ""},
      ["hp_0012"] = {name = "System Diagnostics", vals = {"[Enter]"}, def = 1, ro = true, help = "进入 HP PC Hardware Diagnostics UEFI (BIOS 外部诊断程序)。"},
      ["hp_0013"] = {name = "Update System BIOS", vals = {"[Enter]"}, def = 1, ro = true, help = "更新系统 BIOS。"},
      ["hp_0014"] = {name = "Change Date And Time", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0015"] = {name = "Save Custom Defaults", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0016"] = {name = "Apply Custom Defaults and Exit", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0017"] = {name = "Apply Factory Defaults and Exit", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0018"] = {name = "Ignore Changes and Exit", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0019"] = {name = "Save Changes and Exit", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0020"] = {name = "Suppress POST Errors", vals = {"Disabled", "Enabled"}, def = 2, ro = false, help = ""},
      ["hp_0021"] = {name = "Create BIOS Administrator Password", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0022"] = {name = "Create POST Power-On Password", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0023"] = {name = "Fingerprint Reset on Reboot", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0024"] = {name = "Physical Presence Interface", vals = {"Disabled", "Enabled"}, def = 2, ro = false, help = ""},
      ["hp_0025"] = {name = "Intel Software Guard Extensions (SGX)", vals = {"Software control", "Disabled"}, def = 1, ro = false, help = ""},
      ["hp_0026"] = {name = "Absolute(R) Persistence Module Current State", vals = {"Activation Status: Inactive"}, def = 1, ro = true, help = ""},
      ["hp_0027"] = {name = "Absolute(R) Persistence Module Permanent Disable", vals = {"No", "Yes"}, def = 1, ro = false, help = ""},
      ["hp_0028"] = {name = "System Management Command", vals = {"Disabled", "Enabled"}, def = 2, ro = false, help = ""},
      ["hp_0029"] = {name = "Restore Security Settings to Factory Defaults", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0030"] = {name = "Display Language", vals = {"English", "简体中文", "Francais", "Deutsch", "Espanol"}, def = 1, ro = false, help = ""},
      ["hp_0031"] = {name = "Execute Remote HP PC Hardware Diagnostics", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
      ["hp_0032"] = {name = "3rd Party Option ROM Management", vals = {"[Enter]"}, def = 1, ro = true, help = "重启系统进入第三方 Option ROM 管理程序；也可在启动时按 F3 直接进入。"},
    },
  },
  {
    key = "huawei", short = "华为", icon = "ibm",
    label = "HUAWEI MateBook UEFI Setup Utility",
    title = "HUAWEI MateBook UEFI Setup Utility", thint = "",
    menus = {"信息概览", "安全设置", "启动配置", "退出"},
    pages = {
      ["信息概览"] = {"huawei_0001", "huawei_0002", "huawei_0003", "huawei_0004"},
      ["安全设置"] = {"huawei_0005", "huawei_0006", "huawei_0007"},
      ["启动配置"] = {"huawei_0008", "@可启动设备 (Bootable Device)"},
      ["退出"] = {"huawei_0009", "huawei_0010"},
    },
    subpages = {
      ["可启动设备 (Bootable Device)"] = {},
    },
    items = {
      ["huawei_0001"] = {name = "BIOS 版本", vals = {"1.xx"}, def = 1, ro = true, help = ""},
      ["huawei_0002"] = {name = "EC 版本", vals = {"1.xx"}, def = 1, ro = true, help = ""},
      ["huawei_0003"] = {name = "语言", vals = {"English", "简体中文"}, def = 2, ro = false, help = "切换 BIOS 界面语言。"},
      ["huawei_0004"] = {name = "（分隔线）", vals = {""}, def = 1, ro = true, help = ""},
      ["huawei_0005"] = {name = "安全启动 (Secure Boot)", vals = {"开启", "关闭"}, def = 1, ro = false, help = "官方默认开启。装第三方系统/PE 若无法引导，可在此关闭；改完按 F10 保存。"},
      ["huawei_0006"] = {name = "设置管理员密码 (SETUP Administrator Password)", vals = {"[Enter]"}, def = 1, ro = true, help = "进入 BIOS 时需输入，登录后可修改任意配置；不能当开机密码使用。"},
      ["huawei_0007"] = {name = "设置开机密码 (POST Password)", vals = {"[Enter]"}, def = 1, ro = true, help = "开机进入系统或 BIOS 时需输入；不能作为 BIOS 配置密码。丢失只能更换主板。"},
      ["huawei_0008"] = {name = "PXE 设备 (PXE Device Enable)", vals = {"开启", "关闭"}, def = 2, ro = false, help = "启用后可用 PXE 网络引导。"},
      ["huawei_0009"] = {name = "保存并退出 (F10)", vals = {"[Enter]"}, def = 1, ro = true, help = "官方文档：按 F10 保存并退出 BIOS。"},
      ["huawei_0010"] = {name = "放弃更改并退出", vals = {"[Enter]"}, def = 1, ro = true, help = ""},
    },
  },
}
-- @@OFFICIAL-END@@

local BRAND_ICON = {
    ami = "000000000000000000000000000000000000000000000000000000000000000000000000000000001ffffffffffffff81ffffffffffffff8180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818007e1819ff801818007e1819ff80181801819e781800181801819e78180018180181999818001818018199981800181801ff98181800181801ff981818001818018198181800181801819818180018180181981818001818018198181800181801819819ff80181801819819ff801818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181ffffffffffffff81ffffffffffffff80000001ff80000000000001ff80000000000001ff80000000000001ff80000000000001ff800000000007ffffffe000000007ffffffe000000007ffffffe00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    asus = "000000000000000000000000000000000000000000000000000000000000000000000000000000001ffffffffffffff81ffffffffffffff81800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018181f81fe6061fe18181f81fe6061fe181860660060660018186066006066001818606600606600181860660060660018187fe1f86061f818187fe1f86061f8181860600660600618186060066060061818606006606006181860600660600618186067f81f87f818186067f81f87f81818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181ffffffffffffff81ffffffffffffff80000001ff80000000000001ff80000000000001ff80000000000001ff80000000000001ff800000000007ffffffe000000007ffffffe000000007ffffffe00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    award = "000000000000000000000000000000000000000000000000000000000000000000000000000000001ffffffffffffff81ffffffffffffff8180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818fc3030fc3fc3fc18fc3030fc3fc3fc1b0330330330331b1b0330330330331b1b0330330330331b1b0330330330331b1bff3333ff3fc31b1bff3333ff3fc31b1b0333330333031b1b0333330333031b1b033cf30330c31b1b033cf30330c31b1b033033033033fc1b033033033033fc18000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181ffffffffffffff81ffffffffffffff80000001ff80000000000001ff80000000000001ff80000000000001ff80000000000001ff800000000007ffffffe000000007ffffffe000000007ffffffe00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    dell = "000000000000000000000000000000000000000000000000000000000000000000000000000000001ffffffffffffff81ffffffffffffff81800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018187f87fe60060018187f87fe600600181860660060060018186066006006001818606600600600181860660060060018186067f860060018186067f8600600181860660060060018186066006006001818606600600600181860660060060018187f87fe7fe7fe18187f87fe7fe7fe1818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181ffffffffffffff81ffffffffffffff80000001ff80000000000001ff80000000000001ff80000000000001ff80000000000001ff800000000007ffffffe000000007ffffffe000000007ffffffe00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    hp = "000000000000000000000000000000000000000000000000000000000000000000000000000000001ffffffffffffff81ffffffffffffff81800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180006067f800018180006067f8000181800060660600018180006066060001818000606606000181800060660600018180007fe7f800018180007fe7f80001818000606600000181800060660000018180006066000001818000606600000181800060660000018180006066000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181ffffffffffffff81ffffffffffffff80000001ff80000000000001ff80000000000001ff80000000000001ff80000000000001ff800000000007ffffffe000000007ffffffe000000007ffffffe00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    huawei = "000000000000000000000000000000000000000000000000000000000000000000000000000000001ffffffffffffff81ffffffffffffff818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181b033030fc3033ff1b033030fc3033ff1b033033033033181b033033033033181b033033033033181b033033033033181bff3033ff3333fc1bff3033ff3333fc1b033033033333181b033033033333181b033033033cf3181b033033033cf3181b030fc3033033ff1b030fc3033033ff18000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181ffffffffffffff81ffffffffffffff80000001ff80000000000001ff80000000000001ff80000000000001ff80000000000001ff800000000007ffffffe000000007ffffffe000000007ffffffe00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    ibm = "000000000000000000000000000000000000000000000000000000000000000000000000000000001ffffffffffffff81ffffffffffffff818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181801ff9fe18180181801ff9fe18180181800181819e780181800181819e78018180018181999801818001818199980181800181fe18180181800181fe181801818001818198180181800181819818018180018181981801818001818198180181801ff9fe18180181801ff9fe181801818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181800000000000018180000000000001818000000000000181ffffffffffffff81ffffffffffffff80000001ff80000000000001ff80000000000001ff80000000000001ff80000000000001ff800000000007ffffffe000000007ffffffe000000007ffffffe00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
  }
-- @@DATA-END@@

-- ---------------------------------------------------------------------------
-- 追加项(不改自动生成的数据段, 否则重跑 biossim_gen_lua.py 会被覆盖):
--   ① ITEMS.lang —— Config 页第一项, 中/英界面切换 (③ linit 要求)
--   ② 把 "lang" 插到 联想 Config 页最前, 最容易看到
-- ---------------------------------------------------------------------------
ITEMS.lang = {
  name = "Language",
  cn   = "语言",
  vals = { "English", "中文" },
  def  = 2,                                  -- 默认中文
  help = "界面语言 / UI language。切换后菜单名与项目名立即变成中文或英文(取值仍按真机习惯显示英文)。",
}
for _, brand in pairs(PAGES) do
  if type(brand) == "table" and brand.Config then table.insert(brand.Config, 1, "lang") end
end

-- ============================================================================
-- 版式常量（像素）
-- ============================================================================
local W, H = 400, 300
local CH    = 16          -- 行高 (改用 16px 字模)
local AW    = 8           -- ASCII 宽 (app.textsb = 8x16)
local CW    = 16          -- 汉字宽 (app.zh = font_zh16 真 16x16, 正好 = 2 个 ASCII 格)
local TOPY  = 14          -- 标题栏下方的内容起点
local BAR_H = 16          -- 底部信息条高
local BOTY  = H - 1       -- 底部信息条已去掉(2026-10-02 linit 要求), 内容区直接用到底边

local BK, WH = app.BLACK, app.WHITE

-- ⚠ 按键常量必须**带数值兜底** —— 真机上出现过 A_UP/DOWN/LEFT/RIGHT/CONFIRM 取不到值的情况,
--   结果 action() 里所有分支都不匹配: 手柄上下左右/确认全都没反应(只有 app.exit() 那条还灵)。
local A_UP    = 1   -- 用数值字面量: 桥的按键常量在真机上取不到(实测-2), 直接比较会全不命中
local A_DOWN  = 2   -- 用数值字面量: 桥的按键常量在真机上取不到(实测-2), 直接比较会全不命中
local A_LEFT  = 3   -- 用数值字面量: 桥的按键常量在真机上取不到(实测-2), 直接比较会全不命中
local A_RIGHT = 4   -- 用数值字面量: 桥的按键常量在真机上取不到(实测-2), 直接比较会全不命中
local A_OK    = 5   -- 用数值字面量: 桥的按键常量在真机上取不到(实测-2), 直接比较会全不命中
local A_BACK  = 6   -- 用数值字面量: 桥的按键常量在真机上取不到(实测-2), 直接比较会全不命中

-- ============================================================================
-- 混合排版工具（ASCII 8px + 汉字 12px —— 宽度必须按**像素**算，不能用 #s）
-- ============================================================================
local function is_ascii(b) return b < 0x80 end

-- 逐个 UTF-8 字符遍历 → 返回 字符, 字节长度
local function uch(s, i)
  local b = s:byte(i)
  if not b then return nil, 1 end
  if b < 0x80 then return s:sub(i, i), 1 end
  local n = (b >= 0xF0) and 4 or ((b >= 0xE0) and 3 or 2)
  return s:sub(i, i + n - 1), n
end

local function wpx(s)
  local w, i = 0, 1
  while i <= #s do
    local c, n = uch(s, i)
    if not c then break end
    w = w + (is_ascii(c:byte(1)) and AW or CW)
    i = i + n
  end
  return w
end

-- 按像素宽截断（超出时末尾加 ".."，不要从汉字中间切）
local function cut_px(s, maxw)
  if wpx(s) <= maxw then return s end
  local out, w, i = "", 0, 1
  while i <= #s do
    local c, n = uch(s, i)
    if not c then break end
    local cw = is_ascii(c:byte(1)) and AW or CW
    if w + cw > maxw - 2 * AW then break end
    out = out .. c
    w = w + cw
    i = i + n
  end
  return out .. ".."
end

-- 画一行混排文本（ASCII 合并成一次调用，汉字逐字调用）
local function draw_mix(x, y, s, inv)
  local i = 1
  while i <= #s do
    if is_ascii(s:byte(i)) then
      local j = i
      while j <= #s and is_ascii(s:byte(j)) do j = j + 1 end
      local run = s:sub(i, j - 1)
      app.textsb(x, y, run, inv)
      x = x + #run * AW
      i = j
    else
      local c, n = uch(s, i)
      if not c then break end
      -- ⚠ 必须用 app.zh (= lua_zh16 → font_zh16), 它和系统 UI 用的是**同一套 16×16 字模**。
      --   app.zhsb 是把 24×24 的中央 22×22 最近邻缩到 16×16 → 字形糊、笔画粘连,
      --   一眼就能看出"不是内置的 16px"。2026-10-02 实测对比后改掉。
      app.zh(x, y, c, inv)
      x = x + CW
      i = i + n
    end
  end
  return x
end

-- 右对齐（xr = 右边界）
local function draw_right(xr, y, s, inv)
  return draw_mix(xr - wpx(s), y, s, inv)
end

-- 一行: 可选反白底 + 左文本 + 右文本
-- ⚠ inv 的语义是"整格反白"= 填黑 + 白字（app.texts 的第 4 参）
local function row_line(x0, y, x1, inv, txt, tright)
  if inv then app.fill(x0, y, x1, y + CH - 1, BK) end
  if txt then
    if tright then
      local n = wpx(tright)
      draw_mix(x0 + AW, y, cut_px(txt, (x1 - x0) - n - 3 * AW), inv)
      draw_right(x1 - 2, y, tright, inv)
    else
      draw_mix(x0 + AW, y, cut_px(txt, (x1 - x0) - 2 * AW), inv)
    end
  end
end

local function txt(x, y, s, inv) return draw_mix(x, y, s, inv) end

-- 一个"菜单片"：宽度由自己定，反白时填黑（**不能走 row_line** —— 那会按盒宽再截一次，
-- 菜单项会被切成 "Adva.." 这种）
local function chip(x, y, t, sel)
  if sel then app.fill(x, y, x + wpx(t) - 1, y + CH - 1, BK) end
  txt(x, y, t, sel)
end

-- ============================================================================
-- 状态
-- ============================================================================
local G   = {}
local CFG = {}          -- CFG[brand] = { 选项 key = 取值下标 }（每个品牌各自独立）

local function key_list(t)
  local ks = {}
  for k in pairs(t) do ks[#ks + 1] = k end
  table.sort(ks)
  return ks
end

local function cfg_of(b)
  if not CFG[b] then
    local c = {}
    for k, it in pairs(ITEMS) do c[k] = it.def end
    CFG[b] = c
  end
  return CFG[b]
end

local function reset_to(b, t)
  G.brand = b
  G.task  = t
  G.cfg   = cfg_of(b)
  G.page  = (BRANDS[b].home and PAGES[b][BRANDS[b].home]) and BRANDS[b].home or key_list(PAGES[b])[1]
  G.nav   = {}
  G.cur   = 1
  G.boEdit, G.boSel = false, 1
  G.dlg, G.msg, G.hint = nil, nil, nil
  G.dscroll = 1
  G.order = {}
  for i, d in ipairs(BOOT_DEVICES) do G.order[i] = d end
end

local function in_sub() return #G.nav > 0 end

-- 每页末尾追加的三行"掌机动作行"（替代 F8 / F1 / F9）
-- ============================================================================
-- 左栏「分类」= 厂商菜单 + 末尾**多一个**「说明」入口  (2026-10-02 linit 定案)
--   ① 不再给每个分类页尾部挂「检查结果 / 说明 / 载入默认值」三行 —— 每页只列项目本身;
--   ② 要看法就看左栏最后那一项「说明」(键位 + 用法); 载入默认值在 Restart 页, 与真机一致;
--   ③ 左右键不再切分类 —— 改成"切换聚焦栏"(左栏 ↔ 右栏), 与**应用管家**一致:
--        左栏里 UP/DOWN 换分类, ← → OK BACK 任意键回右栏;
--        右栏里 UP 到顶 → 跳左栏, LEFT → 跳左栏。
-- ============================================================================
-- ⚠ 这两个常量必须声明在**所有使用点之前**: handle_act / build_rows 都用到 BOOT_TASK。
--   (2026-10-02 事故: 原先声明在 on_enter 之前 → 比 handle_act 晚 → handle_act 里读到的是
--    全局 BOOT_TASK=nil → reset_to(brand,nil) → G.task=nil → app.nvs.set_str(...,nil)
--    报 "bad argument #2 ... (string expected, got nil)"，每次进 BIOS 都弹 toast。)
local BOOT_BRAND = "ibm"      -- BRANDS 里的键: 联想 ThinkPad Setup
local BOOT_TASK  = "free"     -- 自由模式: 无规则、不判分, 进去自己改

local MENU_HELP = "__help"

local function menu_list()
  local t = {}
  for _, m in ipairs(BRANDS[G.brand].menus) do t[#t + 1] = m end
  t[#t + 1] = MENU_HELP
  return t
end

-- 界面语言: 由 Config 页的 Language 项驱动 (1 = English, 2 = 中文)
-- ⚠ 必须把所有厂商用到的菜单名都收进来, 否则中文界面下会**中英混排**
--   (2026-10-02 实测: V530 的 Main/Devices/Advanced/Power/Exit 没译名 → 左栏一半中文一半英文)。
local MENU_ZH = { Config = "配置", ["Date/Time"] = "日期和时间", Security = "安全",
                  Startup = "启动", Restart = "重新启动", Network = "网络", Display = "显示",
                  Main = "主要", Devices = "设备", Advanced = "高级", Power = "电源",
                  Exit = "退出", Information = "信息", Configuration = "配置",
                  Boot = "启动", Monitor = "监视", Chipset = "芯片组", Diagnostics = "诊断" }
local function lang_zh() return ((G.cfg and G.cfg.lang) or 1) == 2 end
local function menu_name(m)
  if m == MENU_HELP then return lang_zh() and "说明" or "Help" end
  if lang_zh() then return MENU_ZH[m] or m end
  return m
end

local function build_rows(def)
  local out = {}
  for _, e in ipairs(def or {}) do
    local c = e:sub(1, 1)
    if c == "@" then
      local nm = e:sub(2)
      out[#out + 1] = { kind = "sub", label = nm, sub = nm }
    elseif c == "*" then
      local a = ACTIONS[e]
      if a then out[#out + 1] = { kind = "act", label = a.name, key = e, act = a.act, help = a.help } end
    elseif ITEMS[e] and not ITEMS[e].hidden then
      local it = ITEMS[e]
      out[#out + 1] = { kind = "item", key = e, label = it.name, help = it.help, ro = it.ro }
    end
  end
  return out
end

-- 左栏末尾那一项「说明」的内容 (键位 + 用法; 不再往每个分类页塞动作行)
local function help_rows()
  local zh = lang_zh()
  local t  = {}
  local function add(a, b) t[#t + 1] = { kind = "info", label = zh and a or b } end
  add("<- 回左栏   ^v 选项目   OK 改值/进入   BACK 返回",
      "Left: to menu   Up/Down: move   OK: change/enter   Back: back")
  add("左栏里 ^v 换分类, 再按任意键回右栏",
      "In the menu column: Up/Down switch pages; any other key returns")
  add("改值 = 选中项按 OK 循环切换取值",
      "Change a value: press OK on the item to cycle values")
  add("载入默认值在「重新启动 / Restart」页",
      "Load Setup Defaults lives in the Restart page")
  add("界面语言 = 「配置 / Config」页第一项 Language",
      "UI language = first item (Language) of the Config page")
  return t
end

function rows()
  if in_sub() then return build_rows(SUBPAGES[G.nav[#G.nav]]) end
  if G.page == MENU_HELP then return help_rows() end
  return build_rows(PAGES[G.brand][G.page])
end

local function page_title()
  if in_sub() then return menu_name(G.page) .. "  >  " .. table.concat(G.nav, " > ") end
  return menu_name(G.page)
end

-- 品牌专属叫法 / 身份值（戴尔叫 SATA Operation、机型是 OptiPlex 7090 之类）
-- 界面语言 = 中文时优先用 ITEMS[k].cn（数据段里每个项目都带中文名）
local function item_name(k)
  local B = BRANDS[G.brand]
  if lang_zh() and ITEMS[k].cn then return ITEMS[k].cn end
  if B.ovn and B.ovn[k] then return B.ovn[k] end
  if B.cn and ITEMS[k].cn then return ITEMS[k].cn end
  return ITEMS[k].name
end

local function cfg_name(k)
  local B = BRANDS[G.brand]
  if B.ov and B.ov[k] ~= nil then return B.ov[k] end
  local v = ITEMS[k].vals
  local i = G.cfg[k] or 1
  if i < 1 then i = 1 end
  if i > #v then i = #v end
  return v[i]
end

local function set_cfg(k, i)
  local it = ITEMS[k]
  if not it or it.ro or it.act then return false end
  local n = #it.vals
  G.cfg[k] = ((i - 1) % n) + 1
  G.need_redraw = true
  return true
end

-- ============================================================================
-- 判定引擎（与 HTML 版逐条一致）
-- ============================================================================
local function evaluate(cf, taskId)
  local t = TASKS[taskId]
  local lines, ok = {}, 0
  for _, r in ipairs(t.rules) do
    local k, want, why = r[1], r[2], r[3]
    local it = ITEMS[k]
    if not it then
      lines[#lines + 1] = { ok = true, text = why, now = "(本机无此项, 跳过)" }
      ok = ok + 1
    else
      local good = (cf[k] == want)
      if good then ok = ok + 1 end
      lines[#lines + 1] = { ok = good, text = why, now = it.name .. " = " .. it.vals[cf[k]] }
    end
  end
  local extra = {}
  local sata, vmd = cf.sataMode, cf.vmd
  if (taskId == "win11" or taskId:sub(1, 6) == "ubuntu") and (sata == 3 or sata == 4 or vmd == 2) then
    extra[#extra + 1] = "[!] 现在这个 SATA 模式, 安装器会看不到硬盘 -- 装机最常卡这一步。"
  end
  if taskId == "win11" and cf.secureBoot == 1 then
    extra[#extra + 1] = "[!] Secure Boot 关着, Win11 安装器会直接拒绝。"
  end
  if taskId:sub(1, 4) == "win7" and cf.secureBoot == 2 then
    extra[#extra + 1] = "[!] Secure Boot 开着装 Win7: 会出现 Windows 无法验证数字签名。"
  end
  return { lines = lines, ok = ok, total = #t.rules, pass = (ok == #t.rules), extra = extra }
end

-- ============================================================================
-- 按像素换行
-- ============================================================================
local function wrap(s, maxw)
  local out, cur, w, i = {}, "", 0, 1
  while i <= #s do
    local c, n = uch(s, i)
    if not c then break end
    if c == "\n" then
      out[#out + 1] = cur; cur = ""; w = 0
    else
      local cw = is_ascii(c:byte(1)) and AW or CW
      if w + cw > maxw then
        out[#out + 1] = cur; cur = c; w = cw
      else
        cur = cur .. c; w = w + cw
      end
    end
    i = i + n
  end
  if cur ~= "" or #out == 0 then out[#out + 1] = cur end
  return out
end

-- ============================================================================
-- 选择屏（第一次打开程序时选 厂商 + 装机场景；选过以后记在 nvs，下次直接进）
-- ============================================================================
-- 顺序取自数据段里的 BRAND_ORDER / TASK_ORDER（照 HTML 的书写顺序，第一个 = 默认项）
local BRAND_KEYS = BRAND_ORDER
local TASK_KEYS  = TASK_ORDER

-- ============================================================================
-- 合并「官方机型」(由 tools/biossim_gen_official.py 生成的 OFFICIAL_MODELS)
--   每个机型 = 品牌网格里的**一个图标位**; 菜单/子菜单/条目全部来自联想官方模拟器的真实数据
--   (download.lenovo.com/bsco/public/<机型>.json), 不是编的。
--   ⚠ 必须放在 `local BRAND_KEYS = BRAND_ORDER` **之后** —— 合并要往 BRAND_KEYS 里追加。
-- ============================================================================
if OFFICIAL_MODELS then
  for _, m in ipairs(OFFICIAL_MODELS) do
    BRANDS[m.key] = {
      label = m.label, short = m.short, home = m.menus[1],
      title = m.title, thint = m.thint, footer = "",
      menus = m.menus, layout = "leftmenu",
    }
    PAGES[m.key] = m.pages
    for k, v in pairs(m.subpages) do SUBPAGES[k] = v end
    for k, v in pairs(m.items)    do ITEMS[k]    = v end
    -- ⚠ 只有**新机型**才占一个图标位。内置的 award/ami/asus/dell/hp/huawei 是"同名覆盖"
    --   (官方菜单树替换掉原来手编的骨架), 不能再往 BRAND_KEYS 里加一遍 —— 否则网格出现重复项。
    if not BRAND_ICON[m.key] then
      BRAND_ICON[m.key] = BRAND_ICON[m.icon or "ibm"]   -- 图标复用联想(同一家产品)
      BRAND_KEYS[#BRAND_KEYS + 1] = m.key
    end
  end
end


-- 三个模式的标签（linit: 系统安装 / 阵列配置 / 自由模式）
local MODE_TAG = { os = "[系统安装]", raid = "[阵列配置]", free = "[自由模式]" }

local function brand_short(k) return BRANDS[k].short or k end

-- 品牌选择 = 64x64 图标网格（4 列 x 2 行，7 个品牌；图标可替换，见 apps/biossim/brand_icons/）
-- ⚠ 2026-10-02: 品牌数从 7 涨到 10(7 内置 + 3 台联想官方机型) → 网格必须 5 列 × 2 行 = 10 格。
--   原来 4 列 × 2 行只有 8 格, 第 3 行会跑到屏幕外(y=298+, 屏高只有 300)。
local GRID_COLS = 5
local CELL_W, CELL_H = 80, 148      -- 80 = 400/5; 64 图标 + 左右各 8 边距; 两行铺满 300 高
local ICON_SZ = 64

local function draw_choose()
  app.clear()
  local step = G.step or 1
  if step == 1 then
    -- 只放图标网格: 不要顶栏、不要底栏、不要"已选/数量"
    for i, k in ipairs(BRAND_KEYS) do
      local c  = (i - 1) % GRID_COLS
      local r  = (i - 1) // GRID_COLS
      local x0 = c * CELL_W
      local y0 = 2 + r * CELL_H
      local sel = (i == G.bcur)
      local ix = x0 + (CELL_W - ICON_SZ) // 2
      local iy = y0 + 12
      local ic = BRAND_ICON[k]
      if ic then app.bmp(ix, iy, ICON_SZ, ICON_SZ, ic)
      else       app.rect(ix, iy, ix + ICON_SZ - 1, iy + ICON_SZ - 1, BK) end
      local nm = brand_short(k)
      local ny = iy + ICON_SZ + 6
      if sel then                       -- 选中: 双线框 + 品牌名反白
        app.rect(x0 + 3, y0 + 4, x0 + CELL_W - 4, ny + CH - 1, BK)
        -- ⚠ 不要在这里再 app.fill 一次: draw_mix(...,true) 内部的 textsb/zhsb
        --   已经"逐字铺黑底 + 白字"了, 后补的黑块会把刚画好的白字整条抹掉
        --   (选中项的品牌名会变成一条纯黑横杠)。
        draw_mix(x0 + (CELL_W - wpx(nm)) // 2, ny, nm, true)
      else
        draw_mix(x0 + (CELL_W - wpx(nm)) // 2, ny, nm, false)
      end
    end
    -- 【临时诊断】抓屏即可读出常数真实取值
  else
    -- 模式与方案: 同样不要顶栏/底栏; 方案要求放在"检查结果"里看
    local y = 4
    for i, k in ipairs(TASK_KEYS) do
      if y > H - CH then break end
      local t  = TASKS[k]
      local on = (i == G.bcur)
      row_line(0, y, W - 1, on,
               ((on and "> " or "  ") .. (MODE_TAG[t.mode] or "") .. " " .. t.label))
      y = y + CH
    end
  end
end

-- ============================================================================
-- 全屏 BIOS
-- ============================================================================
local function draw_bios()
  app.clear()
  local B  = BRANDS[G.brand]
  local rs = rows()
  if G.cur > #rs then G.cur = #rs end
  if G.cur < 1  then G.cur = 1  end

  row_line(0, 0, W - 1, true, B.title, B.thint)      -- 标题栏(反白)
  app.hline(0, CH, W - 1, BK)

  local y0
  if B.layout == "topmenu" then
    local x = 2
    for _, m in ipairs(B.menus) do
      local t = " " .. m .. " "
      chip(x, TOPY, t, (m == G.page))          -- 当前页反白
      x = x + wpx(t) + AW
    end
    y0 = TOPY + CH + 4
    txt(6, y0, cut_px(page_title(), W - 12), false)
    y0 = y0 + CH + 2
    for i, r in ipairs(rs) do
      if y0 > BOTY - CH then break end
      local on  = (i == G.cur)
      local nm  = (r.kind == "item") and item_name(r.key) or r.label
      local val = (r.kind == "item") and cfg_name(r.key) or (r.kind == "sub" and "> 进入" or "")
      row_line(0, y0, W - 1, on, ((on and "> " or "  ") .. nm), val)
      y0 = y0 + CH
    end
  else
    -- 左侧竖菜单 + 右侧选项面板（品牌差异保留在这里）
    -- 宽度**按最长分类名动态算**(原来死写 128px 太宽); 语言切换会改变宽度, 所以每次都算。
    local LW = 0
    for _, m in ipairs(menu_list()) do
      local w = 3 * AW + wpx(menu_name(m)) + AW
      if w > LW then LW = w end
    end
    if LW > 118 then LW = 118 end        -- 上限(极长名字才触发截断)
    local RX = LW + 6
    local my = TOPY + 2
    for _, m in ipairs(menu_list()) do
      if my > BOTY - CH then break end
      -- ★ 焦点可见化(照应用管家): 只有焦点在左栏时才反色当前分类
      row_line(0, my, LW - 1, (G.bar == 2 and m == G.page), " " .. menu_name(m))
      my = my + CH
    end
    app.vline(LW + 2, TOPY, BOTY - 1, BK)
    txt(RX, TOPY + 2, cut_px(page_title(), W - RX - 4), false)
    local ry = TOPY + CH + 4
    for i, r in ipairs(rs) do
      if ry > BOTY - CH then break end
      -- ★ 只有焦点在右栏时内容项才反色
      local on  = (G.bar == 0 and i == G.cur)
      local nm  = (r.kind == "item") and item_name(r.key) or r.label
      local val = (r.kind == "item") and cfg_name(r.key) or (r.kind == "sub" and ">" or "")
      row_line(RX, ry, W - 1, on, ((on and "> " or "  ") .. nm), val)
      ry = ry + CH
    end
  end

  -- 引导顺序编辑态: 把真实设备串列出来
  local cr = rs[G.cur]
  if cr and cr.act == "bootorder" then
    local n  = #G.order
    local by = BOTY - (n + 1) * CH - 2
    if by < TOPY + 3 * CH then by = TOPY + 3 * CH end
    app.fill(0, by, W - 1, BOTY - 1, WH)
    app.rect(0, by, W - 1, BOTY - 1, BK)
    row_line(1, by, W - 2, true, "  引导顺序", (G.boEdit and "^ v 选  < > 移" or "OK 进入编辑"))
    for i, d in ipairs(G.order) do
      local ly = by + i * CH
      if ly > BOTY - CH then break end
      local on = (G.boEdit and i == G.boSel)
      row_line(2, ly, W - 3, on, ((on and "> " or "  ") .. i .. ". " .. d))
    end
  end

  -- 底部信息条已按 linit 要求去掉(2026-10-02): 不再常驻显示"自由模式 - 随便改"这一排。
  -- 腾出的 16px 归内容区(见上面 BOTY = H - 1)。
end

-- ============================================================================
-- 弹窗（BIOS 风格消息框）
-- ============================================================================
local function dialog_body()
  local rs = rows()
  local R  = rs[G.cur]
  local lines, title = {}, ""
  if G.dlg == "help" then
    local HR = (R and not R.tail) and R or G.hint or rs[1]
    title = HR and ((HR.kind == "item") and item_name(HR.key) or HR.label) or "说明"
    if HR and HR.help then
      for _, l in ipairs(wrap(HR.help, W - 22)) do lines[#lines + 1] = l end
    else
      lines[#lines + 1] = "(此项无额外说明)"
    end
    lines[#lines + 1] = ""
    for _, l in ipairs(wrap("操作(掌机六键): ^ v 选择   < > 切菜单   OK 改值或进入   BACK 返回", W - 22)) do
      lines[#lines + 1] = l
    end
  elseif G.dlg == "check" then
    local r   = evaluate(G.cfg, G.task)
    local tot = TASKS[G.task].rules and #TASKS[G.task].rules or 0
    title = (tot == 0) and "自由模式" or ("检查结果 " .. r.ok .. "/" .. r.total)
    for _, l in ipairs(wrap(TASKS[G.task].brief, W - 22)) do lines[#lines + 1] = l end
    lines[#lines + 1] = ""
    if tot == 0 then
      lines[#lines + 1] = "自由模式: 不判定任何规则, 想改哪项改哪项。"
    else
      for _, l in ipairs(r.lines) do
        for _, x in ipairs(wrap((l.ok and "[OK] " or "[X]  ") .. l.text, W - 22)) do
          lines[#lines + 1] = x
        end
        lines[#lines + 1] = "      现在: " .. l.now
      end
    end
    for _, e in ipairs(r.extra) do
      lines[#lines + 1] = ""
      for _, l in ipairs(wrap(e, W - 22)) do lines[#lines + 1] = l end
    end
    lines[#lines + 1] = ""
    if tot == 0 then
      lines[#lines + 1] = '练熟了就回上一级换成"系统安装"或"阵列配置"模式。'
    else
      lines[#lines + 1] = r.pass and "这台机器的设置可以装这个系统了。"
                          or ("还差 " .. (r.total - r.ok) .. " 项, 标 [X] 的就是要改的。")
    end
  else
    title = "提示"
    for _, l in ipairs(wrap(G.msg or "", W - 22)) do lines[#lines + 1] = l end
  end
  return title, lines
end

local function draw_dlg()
  if not G.dlg then return end
  local title, lines = dialog_body()
  local x0, x1 = 6, W - 7
  local y0, y1 = 34, H - 58
  local maxl = math.floor((y1 - y0) / CH) - 2
  if maxl < 1 then maxl = 1 end
  local maxs = #lines - maxl + 1
  if maxs < 1 then maxs = 1 end
  if G.dscroll > maxs then G.dscroll = maxs end
  if G.dscroll < 1 then G.dscroll = 1 end
  app.fill(x0, y0, x1, y1, WH)
  app.rect(x0, y0, x1, y1, BK)
  app.rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, BK)
  row_line(x0 + 3, y0 + 3, x1 - 3, true, " " .. title)
  local y = y0 + 3 + CH
  for i = G.dscroll, math.min(#lines, G.dscroll + maxl - 1) do
    if y + CH > y1 - CH - 2 then break end
    txt(x0 + 5, y, lines[i] or "", false)
    y = y + CH
  end
  row_line(x0 + 3, y1 - CH - 3, x1 - 3, true,
           (#lines > maxl) and (" ^ v 翻页   共 " .. #lines .. " 行") or " OK / BACK 关闭")
end

-- ============================================================================
-- 契约回调
-- ============================================================================
-- 按需重绘: 原来 lua_poll 每帧置 needs_redraw → 我的 render 每帧全量重绘
-- (7 个 64x64 位图 + 整屏文字) → 按键响应被拖慢。
-- 现在只有"状态变了"(G.need_redraw) 或每隔 15 帧(兜底清残影)才真画。
-- 只在"状态真的变了"(G.need_redraw) 时才重画。
-- 整屏重绘实测 ~50ms (每个像素一次 st7305_draw_pixel); 原来还有个"每 15 帧兜底清残影",
-- 但帧缓冲是单缓冲直刷、根本不会留残影 —— 那个兜底只是定期白花 50ms,
-- 而且常和按键触发的重绘撞车 → 手感发滞。2026-10-02 去掉, 改纯事件驱动。
function render()
  G.fcnt = (G.fcnt or 0) + 1
  if not G.need_redraw then return end
  G.need_redraw = false
  app.clear()
  if G.mode == "choose" then draw_choose() else draw_bios() end
  draw_dlg()
end

local function enter_bios()
  G.need_redraw = true
  G.bar = 0                             -- 初始焦点在右栏(内容)
  G.mode, G.step, G.dlg, G.dscroll = "bios", nil, nil, 1
  -- app.nvs 的命名空间是固定的 "luaapp"，签名里**没有** ns 参数；
  -- 键名上限 15 字符(ESP-IDF 限制)，所以用短的 bs_*。
  app.nvs.set_str("bs_brand", G.brand or "ibm")
  app.nvs.set_str("bs_task",  G.task  or BOOT_TASK)   -- 兜底: 参数为 nil 会抛 bad argument
end

-- 品牌选择 = 64×64 图标网格 (4 列 × 2 行; 图标可替换, 见 apps/biossim/brand_icons/)
-- 2026-10-02 linit: 不同品牌不同 BIOS UI → 入口就用**品牌图标一个一个选**。
local function open_choose()
  G.need_redraw = true
  G.mode, G.step = "choose", 1
  G.dlg  = nil
  G.bcur = 1
  for i, k in ipairs(BRAND_KEYS) do if k == G.brand then G.bcur = i end end
end

local function remember_hint()
  local rs = rows()
  for i = G.cur, 1, -1 do
    if rs[i] and not rs[i].tail then G.hint = rs[i]; return end
  end
  for i = 1, #rs do
    if rs[i] and not rs[i].tail then G.hint = rs[i]; return end
  end
end

local function apply_defaults()
  local d = {}
  for k, it in pairs(ITEMS) do d[k] = it.def end
  CFG[G.brand] = d
  G.cfg = d
  G.msg = "已载入默认值 (Load Setup Defaults)"
  G.dlg = "msg"
end

local function act_ok()
  local rs = rows()
  local R  = rs[G.cur]
  if not R or R.kind == "info" then return end    -- 「说明」页的纯文字行: 按 OK 不做任何事
  remember_hint()
  if R.kind == "sub" then
    if SUBPAGES[R.sub] then
      G.nav[#G.nav + 1] = R.sub
      G.cur = 1
    else
      G.msg = "本模拟器未收录该子菜单"
      G.dlg = "msg"
    end
  elseif R.act == "bootorder" then
    G.boEdit, G.boSel = true, 1
  elseif R.act == "check" then
    G.dlg, G.dscroll = "check", 1
  elseif R.act == "help" then
    G.dlg, G.dscroll = "help", 1
  elseif R.act == "defaults" then
    apply_defaults()
  elseif R.act == "saveexit" then
    G.msg = "Changes saved. 正在重启...(模拟)"
    G.dlg = "msg"
  elseif R.act == "discardexit" then
    CFG[G.brand] = nil
    G.cfg = cfg_of(G.brand)
    G.nav = {}
    G.msg = "未保存退出, 已恢复上次状态"
    G.dlg = "msg"
  elseif R.kind == "item" then
    if R.ro then
      G.msg = "这一项是只读信息, 真机上也改不动"
      G.dlg = "msg"
    else
      local n = #ITEMS[R.key].vals
      set_cfg(R.key, (G.cfg[R.key] or 1) + 1)
      if G.cfg[R.key] == n then app.vibrate(12) end      -- 循环到头给一次触觉
    end
  end
end

local function handle_act(a)
  G.need_redraw = true--  任何按键都可能改变画面 → 标记重绘 
  if G.dlg then                                   -- 弹窗优先
    if a == A_UP then G.dscroll = math.max(1, G.dscroll - 1)
    elseif a == A_DOWN then G.dscroll = G.dscroll + 1
    else G.dlg = nil end
    app.redraw(); return true
  end

  if G.mode == "choose" then
    if (G.step or 1) == 1 then
      -- 图标网格: 左右 ±1 格; 上下 ±一行(越界则退化成 ±1, 保证任何一对方向键都能走遍)
      local m = #BRAND_KEYS
      if a == A_LEFT then
        G.bcur = math.max(1, G.bcur - 1)
      elseif a == A_RIGHT then
        G.bcur = math.min(m, G.bcur + 1)
      elseif a == A_UP then
        local t = G.bcur - GRID_COLS
        G.bcur = (t >= 1) and t or math.max(1, G.bcur - 1)
      elseif a == A_DOWN then
        local t = G.bcur + GRID_COLS
        G.bcur = (t <= m) and t or math.min(m, G.bcur + 1)
      elseif a == A_OK then
        G.brand = BRAND_KEYS[G.bcur]
        G.task  = BOOT_TASK
        reset_to(G.brand, BOOT_TASK)      -- 每个品牌各自独立的配置/菜单结构
        enter_bios()                      -- 直接进该品牌的 BIOS(自由配置, 无模式选择层)
      elseif a == A_BACK then
        app.exit(); return true
      end
    else
      local m = #TASK_KEYS
      if a == A_UP or a == A_LEFT then G.bcur = math.max(1, G.bcur - 1)
      elseif a == A_DOWN or a == A_RIGHT then G.bcur = math.min(m, G.bcur + 1)
      elseif a == A_OK then
        G.task = TASK_KEYS[G.bcur]
        reset_to(G.brand, G.task)
        enter_bios()
      elseif a == A_BACK then
        G.step, G.bcur = 1, 1
      end
    end
    app.redraw(); return true
  end

  local rs = rows()
  local R  = rs[G.cur]

  if G.boEdit and (not R or R.act ~= "bootorder") then G.boEdit = false end
  if G.boEdit then                                -- 引导顺序编辑态
    if a == A_UP then G.boSel = math.max(1, G.boSel - 1)
    elseif a == A_DOWN then G.boSel = math.min(#G.order, G.boSel + 1)
    elseif a == A_LEFT or a == A_RIGHT then
      local to = (a == A_LEFT) and (G.boSel - 1) or (G.boSel + 1)
      if to >= 1 and to <= #G.order then
        G.order[G.boSel], G.order[to] = G.order[to], G.order[G.boSel]
        G.boSel = to
        app.vibrate(10)
      end
    else
      G.boEdit = false
    end
    app.redraw(); return true
  end

  -- ★ 左栏(分类)聚焦: 完全照 **应用管家** 的 s_focus_bar==2 语义
  --   UP/DOWN 换分类(循环); ← → OK BACK 任意其它键 → 回右栏
  if G.bar == 2 then
    if a == A_UP or a == A_DOWN then
      local ms = menu_list()
      local mi = 1
      for i, m in ipairs(ms) do if m == G.page then mi = i end end
      mi = (a == A_UP) and (((mi - 2) % #ms) + 1) or ((mi % #ms) + 1)
      G.page, G.cur, G.nav = ms[mi], 1, {}
    else
      G.bar = 0
    end
    app.redraw(); return true
  end

  if a == A_UP then
    if G.cur > 1 then G.cur = G.cur - 1
    else G.bar = 2 end                  -- 到顶 → 跳左栏(与应用管家一致)
  elseif a == A_DOWN then
    if G.cur < #rs then G.cur = G.cur + 1 end
  elseif a == A_LEFT then
    if in_sub() then table.remove(G.nav); G.cur = 1
    else G.bar = 2 end                  -- LEFT → 去左栏
  elseif a == A_RIGHT then
    local R = rs[G.cur]
    if R and R.kind == "sub" then act_ok() end   -- 右栏 RIGHT: 是子菜单就进去, 否则不动
  elseif a == A_OK then
    act_ok()
  elseif a == A_BACK then
    if in_sub() then table.remove(G.nav); G.cur = 1
    else open_choose() end              -- 顶层: 回品牌选择(可换品牌); 在网格里再按 BACK 才退出
  end
  app.redraw(); return true
end

-- ============================================================================
-- 按键通道(2026-10-02 真机定案): **只用 action(a) 的参数, 不用 app.key()**
-- ============================================================================
-- ⛔ `app.key()` 在真机上**不可用**: C 侧日志明明是 `key() pop 2`, 但屏上探针显示
--    Lua 侧拿到的是 **1009365760(一个 PSRAM 地址)** —— 于是 `k>=1 and k<=7` 必然为假,
--    handle_act 一次都不跑, 表现就是"按任何键都没反应"。
--    (同样症状早先也出现过: 常量 `app.UP` 读成 -2、`action(a)` 收到 1012453460。)
-- ✅ 唯一可信的通道是 **action(a) 的入参**: 桥用 lua_pushinteger 把动作码压栈后 pcall,
--    真机反证 = BACK 一直被正确识别("返回键一直有效"), C 侧 `lua_action a=2` 也对。
-- ⇒ 单通道, 天然不会重复触发(不需要任何时间去重)。
-- 按键处理: 单通道, 就在 action(a) 里(2026-10-02 定案)。
-- 桥的 lua_action() 把动作码当**参数**传进来, 且已修好 LUA_32BITS 的 ABI 不一致
-- (原来 lua_pushinteger 送出的数字到脚本侧是垃圾 → 这也是历史上"常量取不到值/参数乱"的总根源)。
-- ✅ 所以这里直接用形参 a 即可, 不需要 app.key()(边沿 FIFO), 更不需要任何"时间去重"。
-- 返回值: 恒 true = 已被应用消费, 框架不要替我 pop。
--   BACK 的语义由 handle_act 自己拥有: 有弹窗→关弹窗; 在子菜单→退一层; 顶层→app.exit()。
function action(a)
  if type(a) ~= "number" or a < 1 or a > 7 then return true end
  handle_act(a)
  return true
end

-- 2026-10-02 linit 定案: **只保留「联想 + 自由模式」**。
--   打开应用 = 直接进 BIOS 全屏配置页, 不再让你先选品牌、再选装机方案。
--   品牌图标网格与"系统安装 / 阵列配置 / 方案列表"全部撤掉。
--   数据(BRANDS/PAGES/ITEMS/TASKS)仍原样保留 → 以后要挂回来只改下面两行。
function on_enter()
  G.need_redraw = true
  G.fcnt = 0
  G.order = {}
  for i, d in ipairs(BOOT_DEVICES) do G.order[i] = d end
  reset_to(BOOT_BRAND, BOOT_TASK)
  open_choose()               -- ★ 入口 = 品牌图标网格 (选品牌 → 进它自己的 BIOS)
end

function on_exit()
  app.sbar(false)
end

-- poll(): 本机**没有触摸芯片**，而 app.touch() 在无触摸硬件时可能返回残留坐标 →
-- 之前用它做点选，会变成"幽灵点击"不断改选中/触发确认，表现就是
-- "手柄上下左右选不中、也确认不了"。所以这里**完全不碰触摸**，只留给按键(action)。
-- (2026-10-02 linit: 掌机本来就只用按键)

function poll()
  -- 按键**不在**这里 —— 见上方 action(a) 的说明(单通道, 在 action 里处理)。
  -- 这个函数留给以后做动画/计时(例如需要逐帧推进的状态机)。
  -- ⚠ 本机没有触摸芯片, 也不要在这里用 app.touch(), 否则会出现"幽灵点击"。
end
