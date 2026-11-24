#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第二轮种子数据 — 16 字段, 顺序与现库一致。

字段: code, en, zh, cat, scope, cause, steps, advice, risk, severity,
      source, note, keys, layer, vendor, verify

约定:
  · cause / steps / verify 用 "1) … 2) …" 分条, build_detail 会按编号折行;
  · 条目以 "@" 开头 = 命令行, 详情页单独一行并加 ">" 前缀 (不与解释混排);
  · keys 纯 ASCII; 禁止编造官方来源。
来源依据 (本轮实抓/实核):
  · Windows 更新错误码表: learn.microsoft.com/troubleshoot/azure/virtual-machines/windows/
    windows-update-errors-requiring-in-place-upgrade + troubleshoot-windows-update-azure-vm
    (0x80073712 / 0x800F0831 / 0x800F0922 / 0x8007000D / 0x800F0982 / 0x800F0988 / 0x800F0830 / 0xC01A001D)
  · Win32 系统错误消息: learn.microsoft.com/windows/win32/debug/system-error-codes-*
  · NTSTATUS: MS-ERREF (0xC000000D STATUS_INVALID_PARAMETER)
  · Bug Check 参考: 0x1CA SYNTHETIC_WATCHDOG_TIMEOUT
"""

# ---------------------------------------------------------------- 蓝屏补充
BSOD = [
    (
        "0x000001CA",
        "SYNTHETIC_WATCHDOG_TIMEOUT - A system wide watchdog has expired. This indicates that the system is hung and not processing timer ticks",
        "看门狗超时: 系统整体挂死, 不再处理时钟节拍 (timer tick)",
        "C_MB",
        "Windows 10/11 (虚拟机/笔记本低功耗场景多见)",
        "1) 系统被长时间挂起后恢复 (虚拟机快照/暂停/迁移, 笔记本深度睡眠)"
        " 2) 存储或芯片组驱动响应超时"
        " 3) CPU 深度节能状态 (C-State) 与固件不兼容"
        " 4) 硬件长时间无响应: 盘掉线、PCIe 设备挂死",
        "1) 回忆蓝屏前是否做过挂起/恢复/快照/迁移, 先排除这条"
        " 2) 事件查看器 → Windows 日志 → 系统, 查 Kernel-Power 41 与 WHEA-Logger"
        " 3) @powercfg /energy"
        " 4) 更新芯片组与存储驱动, BIOS 里关掉激进的节能/深度睡眠后复测"
        " 5) 虚拟机换宿主机、或关闭快照对比是否复现",
        "先怀疑「系统被挂起过」, 再查驱动与节能设置; 别一上来就判主板坏",
        "中: 蓝屏瞬间未落盘的写入可能丢",
        "现场可修",
        "MS",
        "与联想 POST 码 01CA 不是同一个码: 带 0x 的 0x000001CA 是蓝屏, 01CA 是固件自检",
        "0x1ca 1ca synthetic watchdog timeout kanguan",
        "BSOD",
        "Microsoft",
        "1) 复现前后 Kernel-Power 41 / WHEA-Logger 事件是否消失"
        " 2) 连续「挂起-恢复」5 次不再蓝屏"
        " 3) 关掉节能仍蓝屏 ⇒ 按驱动或硬件方向继续查, 不能再归因于挂起",
    ),
]

# ---------------------------------------------------------------- 引导补充
BOOT = [
    (
        "0xC000000D",
        "STATUS_INVALID_PARAMETER - An invalid parameter was passed to a service or function",
        "引导参数无效: 启动管理器拿到的 BCD 或设备参数不合法",
        "C_BOOT",
        "Windows 7/8/10/11 (UEFI 与 Legacy 都可能)",
        "1) BCD 里 device/osdevice 指向了不存在的分区 (克隆、换盘、扩容后常见)"
        " 2) 分区表被改动, 盘符或 ESP 分区号变了"
        " 3) 固件启动项参数与当前 UEFI/Legacy 模式不匹配"
        " 4) 第三方分区/引导工具改写过启动配置",
        "1) 进 WinRE → 疑难解答 → 命令提示符"
        " 2) @bootrec /scanos"
        " 3) @bootrec /rebuildbcd"
        " 4) @bcdedit /enum all"
        " 5) 确认 device 与 osdevice 指向的分区真实存在; 不存在先修分区表"
        " 6) UEFI 机器重建 ESP: @bcdboot C:\\Windows /s S: /f UEFI",
        "九成是 BCD 参数与真实分区对不上, 重建启动项即可, 不必重装",
        "中: 重建 BCD 前先导出备份, 否则可能连修复菜单都进不去",
        "现场可修",
        "MS",
        "NTSTATUS 0xC000000D 在引导阶段表示参数非法, 本身不代表硬件损坏",
        "0xc000000d c000000d invalid parameter bcd bootmgr yindao",
        "BOOT",
        "Microsoft",
        "1) bcdedit /enum 列出的分区都能访问"
        " 2) 重启不再出现该码"
        " 3) 双系统环境下另一套系统仍能正常启动",
    ),
]

# ---------------------------------------------------------------- 系统错误
SYS = [
    (
        "0x80073712",
        "ERROR_SXS_COMPONENT_STORE_CORRUPT - The component store is corrupted",
        "组件存储损坏, 更新或功能安装失败",
        "C_OS",
        "Windows 8/10/11, Server 2012 及以上",
        "1) WinSxS 组件存储被损坏 (断电、强制关机、磁盘坏道)"
        " 2) 之前某次更新安装中断, 留下半成品"
        " 3) 第三方「精简/优化」工具删过组件",
        "1) 先跑组件存储修复"
        " 2) @DISM /Online /Cleanup-Image /RestoreHealth"
        " 3) @sfc /scannow"
        " 4) 仍失败看日志 C:\\Windows\\Logs\\CBS\\CBS.log 里第一条 FAIL"
        " 5) 反复损坏要查盘: @chkdsk C: /scan",
        "先 DISM 修组件存储再 sfc; 修完再做更新, 别直接重装",
        "无: 只读修复, 不改用户数据",
        "现场可修",
        "MS",
        "MS 更新错误码表把该码归为组件存储损坏, 首选动作是 DISM 修复",
        "0x80073712 80073712 sxs component store corrupt cbs dism gengxin",
        "SYS",
        "Microsoft",
        "1) DISM 返回「操作成功完成」"
        " 2) 重新执行原更新能装完"
        " 3) CBS.log 不再出现同一处 ERROR_SXS_COMPONENT_STORE_CORRUPT",
    ),
    (
        "0x800F0831",
        "CBS_E_STORE_CORRUPTION - The CBS store is corrupted",
        "组件存储 (CBS) 损坏, 更新装不上",
        "C_OS",
        "Windows 10/11, Server 2016 及以上",
        "1) 更新包下载不完整或校验失败"
        " 2) 组件存储本身已有损坏 (与 0x80073712 常同时出现)"
        " 3) 磁盘写入错误导致包体残缺",
        "1) 清空更新缓存后重新下载"
        " 2) @net stop wuauserv"
        " 3) @ren C:\\Windows\\SoftwareDistribution SoftwareDistribution.old"
        " 4) @net start wuauserv"
        " 5) 再跑 @DISM /Online /Cleanup-Image /RestoreHealth",
        "先清缓存重下, 再 DISM; 只清缓存不修存储往往下次还报",
        "无: 只删更新缓存, 不动个人文件",
        "现场可修",
        "MS",
        "更新缓存目录被占用时先停 wuauserv 与 bits 服务",
        "0x800f0831 800f0831 cbs store corruption gengxinbao sunhuai",
        "SYS",
        "Microsoft",
        "1) 重新下载的包能安装完成"
        " 2) CBS.log 中 CBS_E_STORE_CORRUPTION 不再出现"
        " 3) 再次检查更新不再卡在同一 KB 号",
    ),
    (
        "0x800F0922",
        "CBS_E_INSTALLERS_FAILED - Processing advanced installers and generic commands failed",
        "更新安装器执行失败, 常见于系统保留分区空间不足",
        "C_OS",
        "Windows 8/10/11",
        "1) 系统保留分区 (EFI/恢复分区) 剩余空间不足, 放不下新组件"
        " 2) .NET 或服务堆栈安装器执行失败"
        " 3) 安全软件拦截了安装器写系统目录",
        "1) 查各分区剩余空间, 特别是 EFI 与恢复分区"
        " 2) 临时关闭第三方杀软/防护后重试"
        " 3) 跑 @DISM /Online /Cleanup-Image /RestoreHealth"
        " 4) 功能更新反复失败时, 用官方介质做就地升级",
        "功能更新报这个码, 先看 EFI/恢复分区是不是被塞满 (常见 100MB 分区不够)",
        "中: 调整分区有风险, 先备份",
        "需备件",
        "MS",
        "MS 更新错误码表归类为「高级安装器失败」, 首选动作是重置更新组件",
        "0x800f0922 800f0922 cbs installers failed baoliu fenqu kongjian",
        "SYS",
        "Microsoft",
        "1) EFI/恢复分区可用空间满足更新要求"
        " 2) 重试安装能过 99% 之后的安装阶段"
        " 3) 事件日志中不再出现同一安装器的失败记录",
    ),
    (
        "0x800F0982",
        "PSFX_E_MATCHING_COMPONENT_NOT_FOUND - Matching component not found for delta update",
        "增量更新找不到匹配组件",
        "C_OS",
        "Windows 10 1903 及以上 / 11",
        "1) 组件存储里缺少增量更新需要的基线组件"
        " 2) 之前用第三方工具删过 WinSxS 内容"
        " 3) 长期不更新, 版本跨度过大",
        "1) 先修组件存储: @DISM /Online /Cleanup-Image /RestoreHealth"
        " 2) 再跑 @sfc /scannow"
        " 3) 仍失败则用同版本官方 ISO 做就地升级 (保留文件与应用)",
        "跨版本太久导致的增量不匹配, DISM 修不好就只能就地升级",
        "无: 就地升级保留个人文件, 但建议先备份",
        "现场可修",
        "MS",
        "MS 列为累积更新常见失败码",
        "0x800f0982 800f0982 psfx matching component delta zengliang",
        "SYS",
        "Microsoft",
        "1) 就地升级后版本号正确"
        " 2) 后续累积更新能正常安装"
        " 3) CBS.log 不再出现 PSFX_E_MATCHING_COMPONENT_NOT_FOUND",
    ),
    (
        "0x800F0988",
        "PSFX_E_INVALID_DELTA_COMBINATION - Delta combination is invalid",
        "增量补丁组合非法 (补丁分支不匹配)",
        "C_OS",
        "Windows 10 1903 及以上 / 11",
        "1) 系统装过非本通道的补丁 (测试版/第三方包)"
        " 2) 组件存储中基线版本与本次增量不匹配"
        " 3) 更新中途断电留下不一致状态",
        "1) 确认系统版本与通道: @winver"
        " 2) 卸载最近的可疑补丁: @wusa /uninstall /kb:编号"
        " 3) 修组件存储: @DISM /Online /Cleanup-Image /RestoreHealth"
        " 4) 仍失败按官方 ISO 就地升级",
        "补丁组合非法多数是装错了通道包, 先回退可疑补丁再修",
        "中: 卸载补丁可能回退安全修复",
        "现场可修",
        "MS",
        "与 0x800F0982 的区别: 0982 是缺组件, 0988 是组合非法",
        "0x800f0988 800f0988 psfx invalid delta combination buding",
        "SYS",
        "Microsoft",
        "1) winver 显示版本与目标补丁通道一致"
        " 2) 累积更新安装成功"
        " 3) 事件日志不再记录同一 KB 的失败",
    ),
    (
        "0x800F0830",
        "CBS_E_IMAGE_UNSERVICEABLE - The image can no longer be serviced",
        "系统映像已不可维护, 常规修复无效",
        "C_OS",
        "Windows 10/11, Server 2016 及以上",
        "1) 组件存储严重损坏, DISM 已无法自愈"
        " 2) 系统被第三方大幅精简"
        " 3) 磁盘出现坏道导致系统文件反复损坏",
        "1) 先确认不是盘的问题: @chkdsk C: /scan"
        " 2) 试一次 @DISM /Online /Cleanup-Image /RestoreHealth"
        " 3) 报此码时 MS 建议用官方介质就地升级重装系统组件",
        "这个码意味着映像本身不可服务, 别在 DISM 上反复耗时间",
        "高: 就地升级/重装前必须备份",
        "需备件",
        "MS",
        "MS 列为需要就地升级 (IPU) 的错误",
        "0x800f0830 800f0830 cbs image unserviceable yingxiang",
        "SYS",
        "Microsoft",
        "1) 就地升级完成后 DISM /ScanHealth 无损坏"
        " 2) 后续更新可安装"
        " 3) 若换盘后仍出现 ⇒ 排查系统精简或镜像来源",
    ),
    (
        "0x8007000D",
        "ERROR_INVALID_DATA - The data is invalid",
        "更新元数据损坏 (数据无效)",
        "C_OS",
        "Windows 7/8/10/11",
        "1) Windows 更新元数据文件损坏"
        " 2) 更新缓存被写坏"
        " 3) 磁盘或内存不稳定导致数据落盘错误",
        "1) 重置更新组件: 停 wuauserv 与 bits 服务, 重命名 SoftwareDistribution 目录"
        " 2) @net stop wuauserv"
        " 3) @ren C:\\Windows\\SoftwareDistribution SoftwareDistribution.old"
        " 4) @net start wuauserv"
        " 5) 再执行 @DISM /Online /Cleanup-Image /RestoreHealth",
        "先重置更新组件让它重下元数据; 反复出现要查内存与磁盘",
        "无: 只清更新缓存",
        "现场可修",
        "MS",
        "MS 归类为「元数据损坏」, 首选 WU 重置再 CBS 修复",
        "0x8007000d 8007000d invalid data yuanshuju gengxin",
        "SYS",
        "Microsoft",
        "1) 重新检查更新能下载到元数据"
        " 2) 目标更新安装完成"
        " 3) 若隔几天又报同一码 ⇒ 跑内存检测与 chkdsk",
    ),
    (
        "0x80070003",
        "ERROR_PATH_NOT_FOUND - The system cannot find the path specified",
        "系统找不到指定路径 (更新或安装解包失败)",
        "C_OS",
        "Windows 7/8/10/11",
        "1) 更新缓存中的临时目录被清理或权限异常"
        " 2) 系统临时目录被改到了不存在的位置"
        " 3) 环境变量 TEMP/TMP 指向无效盘符",
        "1) 检查 TEMP/TMP: @echo %TEMP%"
        " 2) 改回默认值 C:\\Windows\\Temp 与 %USERPROFILE%\\AppData\\Local\\Temp"
        " 3) 手动创建缺失目录并给 SYSTEM 完全控制"
        " 4) 重置更新组件后重试",
        "多半是临时目录被清理软件搬走或删掉了, 恢复默认路径即可",
        "无: 只改环境变量与临时目录",
        "现场可修",
        "MS",
        "该码本身是通用路径错误, 出现在更新场景要按临时目录查",
        "0x80070003 80070003 path not found lujing linshi",
        "SYS",
        "Microsoft",
        "1) echo %TEMP% 指向已存在的可写目录"
        " 2) 更新不再在此码处中断"
        " 3) 安装程序日志中无「找不到路径」记录",
    ),
    (
        "0x8024402C",
        "WU_E_PT_WINHTTP_NAME_NOT_RESOLVED - Proxy or server name could not be resolved",
        "更新服务器或代理名称解析失败",
        "C_OS",
        "Windows 7/8/10/11",
        "1) DNS 解析异常, 解析不到更新服务器"
        " 2) 代理配置错误或代理不可达"
        " 3) hosts 文件被改写了微软更新域名"
        " 4) 网络策略/防火墙拦截",
        "1) 先测域名解析: @nslookup download.windowsupdate.com"
        " 2) 检查代理: @netsh winhttp show proxy"
        " 3) 清掉错误代理: @netsh winhttp reset proxy"
        " 4) 检查 hosts 是否屏蔽了 microsoft/windowsupdate 域名"
        " 5) 换网络 (手机热点) 复测, 判断是本机还是网络侧",
        "这是网络层错误, 不是系统损坏; 换热点一试就能定性",
        "无: 只改网络配置",
        "现场可修",
        "MS",
        "WU_E_PT_* 前缀表示「平台/传输层」错误, 与组件存储无关",
        "0x8024402c 8024402c wu proxy dns jiexi wangluo",
        "SYS",
        "Microsoft",
        "1) nslookup 能解析更新域名"
        " 2) netsh winhttp show proxy 显示「直接访问」或正确代理"
        " 3) 检查更新能连上服务器 (不再报 8024 系列)",
    ),
    (
        "0x8024401C",
        "WU_E_PT_HTTP_STATUS_REQUEST_TIMEOUT - The HTTP request timed out",
        "更新请求超时 (连上了但服务器/网络太慢)",
        "C_OS",
        "Windows 7/8/10/11",
        "1) 网络质量差或带宽被占满"
        " 2) 代理/网关响应超时"
        " 3) WSUS 或更新服务器负载过高",
        "1) 换网络或错峰重试"
        "2) 关掉代理直连: @netsh winhttp reset proxy"
        "3) 清理更新缓存后重试"
        "4) 域环境确认 WSUS 地址可访问: @wuauclt /detectnow 后看 WindowsUpdate.log",
        "先归因网络, 再归因缓存; 不要按系统损坏处理",
        "无",
        "现场可修",
        "MS",
        "与 0x8024402C 的区别: 2C 是解析不到名字, 1C 是连上了但超时",
        "0x8024401c 8024401c wu timeout chaoshi wangluo",
        "SYS",
        "Microsoft",
        "1) 换网络后不再超时"
        " 2) 更新能下载到 100%"
        " 3) Get-WindowsUpdateLog 中无连续超时记录",
    ),
    (
        "0x8007001F",
        "ERROR_GEN_FAILURE - A device attached to the system is not functioning",
        "挂载的设备工作不正常 (更新/驱动场景)",
        "C_OS",
        "Windows 7/8/10/11",
        "1) 某个外设或驱动在更新期间无响应"
        " 2) USB/存储设备掉线, 更新枚举设备失败"
        " 3) 驱动与当前系统版本不匹配",
        "1) 拔掉非必要外设 (U 盘/扩展坞/打印机) 后重试"
        " 2) 设备管理器看有无黄色感叹号设备"
        " 3) 更新对应驱动 (芯片组/存储/USB)"
        " 4) 仍失败看 setupact.log 定位是哪个设备",
        "这个码是「有设备不正常」的统称, 必须结合 setupact.log 才定得下来",
        "无",
        "现场可修",
        "MS",
        "通用设备错误码, 单独出现不能判定具体硬件",
        "0x8007001f 8007001f gen failure shebei qudong",
        "SYS",
        "Microsoft",
        "1) 设备管理器无异常设备"
        " 2) 更新/安装能越过设备枚举阶段"
        " 3) 同一外设反复触发 ⇒ 换线换口或换驱动版本复测",
    ),
    (
        "0x8007007E",
        "ERROR_MOD_NOT_FOUND - The specified module could not be found",
        "找不到指定的模块 (DLL 缺失导致安装起步失败)",
        "C_OS",
        "Windows 7/8/10/11",
        "1) 安装器依赖的运行库 (VC++/.NET) 缺失"
        " 2) 系统目录中的 DLL 被删除或改名"
        " 3) 64/32 位版本装错",
        "1) 事件查看器确认是哪个 DLL/EXE"
        " 2) 补装对应运行库 (VC++ 2015-2022 x86 与 x64 都装)"
        " 3) 修系统文件: @sfc /scannow"
        " 4) 仍缺模块则用官方安装包重装该程序",
        "先确认缺哪个模块再补, 盲目重装系统代价太大",
        "无",
        "现场可修",
        "MS",
        "错误消息里的模块名就是线索, 别忽略",
        "0x8007007e 8007007e mod not found dll mocuai",
        "SYS",
        "Microsoft",
        "1) 对应程序能正常启动"
        " 2) 事件日志不再报该模块缺失"
        " 3) 依赖检查工具 (Dependencies) 无红色缺失项",
    ),
    (
        "0x80070490",
        "ERROR_NOT_FOUND - Element not found",
        "找不到元素: 组件或系统文件缺失 (CBS 场景常见)",
        "C_OS",
        "Windows 8/10/11",
        "1) 组件存储中缺少某个 manifest 或文件"
        " 2) 系统文件被第三方清理工具删除"
        " 3) 更新所需组件被策略移除",
        "1) 修组件存储: @DISM /Online /Cleanup-Image /RestoreHealth"
        " 2) 再跑 @sfc /scannow"
        " 3) 仍报则从同版本 ISO 挂载源修复: @DISM /Online /Cleanup-Image /RestoreHealth /Source:wim:D:\\sources\\install.wim:1"
        " 4) 检查 CBS.log 中具体缺的组件名",
        "这个码的关键是「缺什么」, 用 /Source 指到同版本镜像最有效",
        "无: 只读修复",
        "现场可修",
        "MS",
        "常与 0x80073712 同时出现",
        "0x80070490 80070490 not found cbs quexi",
        "SYS",
        "Microsoft",
        "1) DISM 用 /Source 后返回成功"
        " 2) 目标功能/更新安装完成"
        " 3) CBS.log 不再报同一组件缺失",
    ),
    (
        "0x80070570",
        "ERROR_FILE_CORRUPT - The file or directory is corrupted and unreadable",
        "文件或目录已损坏且无法读取",
        "C_OS",
        "Windows 7/8/10/11",
        "1) 磁盘坏道或 SSD 掉块"
        " 2) 安装介质 (ISO/U 盘) 损坏"
        " 3) 断电/强关导致文件系统不一致",
        "1) 先查盘: @chkdsk C: /f /r"
        " 2) 安装场景换官方介质重新制作 U 盘 (用 Media Creation Tool)"
        " 3) 修系统文件: @sfc /scannow"
        " 4) 反复损坏同一区域 ⇒ 盘要换",
        "安装时报这个码, 先怀疑 U 盘/ISO, 再怀疑硬盘",
        "高: chkdsk /f 会改文件系统, 先备份重要数据",
        "需备件",
        "MS",
        "SMART 有重映射/待映射扇区时直接换盘, 不要反复修",
        "0x80070570 80070570 file corrupt sunhuai cipan",
        "SYS",
        "Microsoft",
        "1) chkdsk 无坏扇区报告"
        " 2) 换介质后安装通过"
        " 3) 同一文件不再报损坏",
    ),
    (
        "0x8007045D",
        "ERROR_IO_DEVICE - The request could not be performed because of an I/O device error",
        "I/O 设备错误 (磁盘读写失败)",
        "C_OS",
        "Windows 7/8/10/11",
        "1) 硬盘/SSD 出现坏道或掉盘"
        " 2) SATA/NVMe 线缆、接口接触不良"
        " 3) 存储驱动异常或掉电保护触发",
        "1) 看 SMART: @wmic diskdrive get model,status"
        "2) 查线缆与接口, 换口换线复测"
        "3) @chkdsk C: /scan"
        "4) 事件日志查 disk 类错误 (Event ID 7/11/51/153)",
        "这是磁盘层错误, 优先保数据再谈修复",
        "高: 盘可能随时掉线, 先备份",
        "需备件",
        "MS",
        "Event ID 153 表示存储设备请求超时, 常与掉盘同时出现",
        "0x8007045d 8007045d io device cipan diaopan",
        "SYS",
        "Microsoft",
        "1) 事件日志不再出现 disk 超时/重试"
        " 2) 大文件读写压测无报错"
        " 3) SMART 健康状态正常",
    ),
    (
        "0x80070020",
        "ERROR_SHARING_VIOLATION - The process cannot access the file because it is being used by another process",
        "文件被其他进程占用",
        "C_OS",
        "Windows 7/8/10/11",
        "1) 杀软/备份软件正在扫描目标文件"
        " 2) 上一次安装进程没退干净"
        " 3) 索引服务或同步盘占着文件",
        "1) 关掉杀软实时防护后重试"
        " 2) 任务管理器结束残留安装进程"
        " 3) 用资源监视器 (resmon) 查「关联的句柄」定位占用者"
        " 4) 重启后立即重试",
        "用 resmon 的句柄搜索能直接点出占用进程, 比猜快",
        "无",
        "现场可修",
        "MS",
        "安装/更新场景最常见, 不是文件损坏",
        "0x80070020 80070020 sharing violation zanyong",
        "SYS",
        "Microsoft",
        "1) resmon 中该文件无占用句柄"
        " 2) 安装/复制操作顺利完成"
        " 3) 重启后问题不复现",
    ),
    (
        "0x800706BA",
        "RPC_S_SERVER_UNAVAILABLE - The RPC server is unavailable",
        "RPC 服务器不可用 (服务没起来或被防火墙拦)",
        "C_OS",
        "Windows 7/8/10/11",
        "1) RPC 相关服务未启动或被禁用"
        " 2) 防火墙/安全软件拦截 135 端口"
        " 3) 域环境 DNS 解析不到目标机器",
        "1) 确认服务状态: @sc query rpcss"
        "2) 启动服务: @net start rpcss"
        "3) 检查防火墙是否放行 RPC"
        "4) 域内确认 DNS 与网络连通: @ping 目标机",
        "先看 rpcss 服务是否在跑, 再看 135 端口通不通",
        "无",
        "现场可修",
        "MS",
        "常出现在远程管理、打印、WSUS 场景",
        "0x800706ba 800706ba rpc server unavailable fuwu",
        "SYS",
        "Microsoft",
        "1) sc query rpcss 显示 RUNNING"
        " 2) 远程管理/打印功能恢复"
        " 3) 事件日志无 RPC 连接失败记录",
    ),
    (
        "0x800706BE",
        "RPC_S_CALL_FAILED - The remote procedure call failed",
        "远程过程调用失败 (调用链中途断开)",
        "C_OS",
        "Windows 7/8/10/11",
        "1) 目标服务崩溃或重启"
        " 2) 调用超时 (默认 60 秒左右) "
        " 3) 网络抖动或防火墙中断连接",
        "1) 确认目标服务状态与最近重启时间"
        " 2) 事件日志查对应服务的崩溃记录"
        " 3) 关掉第三方防护后重试"
        " 4) 反复失败则修复该组件 (如打印组件/更新代理)",
        "和 0x800706BA 的区别: BA 是叫不到服务器, BE 是叫到了但调用失败",
        "无",
        "现场可修",
        "MS",
        "常见于打印后台、更新代理、WMI 调用",
        "0x800706be 800706be rpc call failed diaoyong",
        "SYS",
        "Microsoft",
        "1) 相关服务连续运行无崩溃"
        " 2) 同一操作不再返回 0x800706BE"
        " 3) 事件日志中 RPC 错误成对消失",
    ),
    (
        "0x800705B4",
        "ERROR_TIMEOUT - This operation returned because the timeout period expired",
        "操作超时",
        "C_OS",
        "Windows 7/8/10/11",
        "1) 磁盘响应慢 (坏道/叠瓦盘/后台扫描)"
        " 2) 服务无响应"
        " 3) 网络或远程资源超时",
        "1) 看磁盘队列与响应时间 (资源监视器)"
        " 2) 查 SMART 与事件日志 disk 错误"
        " 3) 关闭占用磁盘的扫描/备份任务后重试"
        "4) 服务场景则重启对应服务",
        "超时本身不是根因, 顺藤摸到慢的那一环才算定位",
        "无",
        "现场可修",
        "MS",
        "更新场景出现时多为磁盘慢或服务卡住",
        "0x800705b4 800705b4 timeout chaoshi",
        "SYS",
        "Microsoft",
        "1) 磁盘平均响应时间恢复正常"
        " 2) 原操作在超时时间内完成"
        " 3) 无新的 disk 超时事件",
    ),
    (
        "0x8007025D",
        "ERROR_BAD_COMPRESSION_BUFFER - The specified buffer contains ill-formed data",
        "安装时弹出「Windows 无法安装所需文件」, 底层是缓冲区/数据格式异常 (安装介质或内存问题)",
        "C_OS",
        "Windows 7/8/10/11 安装与功能更新",
        "1) 安装 U 盘/ISO 损坏或制作方式不对"
        " 2) 内存不稳定, 解压 install.wim 时出错"
        " 3) 目标分区空间不足或有坏道",
        "1) 用官方 Media Creation Tool 重做 U 盘"
        " 2) 换 USB 口 (优先主板后置口) 重试"
        " 3) 跑内存检测: @mdsched.exe"
        " 4) 目标盘先 @chkdsk C: /f /r",
        "这个码八成是介质或内存, 不是硬盘容量问题",
        "中: 重装前备份数据",
        "现场可修",
        "MS",
        "MS 安装错误文档把该码指向介质与内存",
        "0x8007025d 8007025d bad compression buffer anzhuang jiezhi",
        "SYS",
        "Microsoft",
        "1) 换介质与内存后安装通过"
        " 2) 安装日志中没有 install.wim 解压失败"
        " 3) 同一 U 盘在别的机器上也能装 (反向确认介质)",
    ),
    (
        "0x80070017",
        "ERROR_CRC - Data error (cyclic redundancy check)",
        "数据错误 (循环冗余检查), 读取的数据校验不通过",
        "C_OS",
        "Windows 7/8/10/11",
        "1) 光驱/安装介质物理损坏"
        " 2) 硬盘坏道"
        " 3) 数据线接触不良或 U 盘主控异常",
        "1) 换介质或换 U 盘重试"
        "2) @chkdsk C: /f /r"
        "3) 换数据线/换接口复测"
        "4) 同一位置反复 CRC ⇒ 盘体问题",
        "CRC 是校验失败, 说明数据真的没读对, 不要当成软件 bug",
        "高: 盘可能有坏道, 先备份",
        "需备件",
        "MS",
        "安装场景常见于刻录盘与劣质 U 盘",
        "0x80070017 80070017 crc shuju cuowu",
        "SYS",
        "Microsoft",
        "1) chkdsk 无坏扇区"
        " 2) 换介质后读取正常"
        " 3) 事件日志无 153/7 类磁盘错误",
    ),
    (
        "0xC01A001D",
        "STATUS_LOG_FULL - The transaction log is full due to insufficient disk space",
        "磁盘空间耗尽, 事务日志写满 (安装/更新中断)",
        "C_OS",
        "Windows 10/11, Server 2016 及以上",
        "1) 系统盘剩余空间不足"
        " 2) 更新缓存与日志占满分区"
        " 3) 卷影副本/还原点占用过多",
        "1) 查剩余空间: @dir C:\\ /-c"
        "2) 清更新缓存与临时文件"
        "3) 限制还原点占用: @vssadmin list shadowstorage"
        "4) 留出至少 20GB 再重试更新",
        "先把系统盘腾到 20GB 以上, 再谈其它修复",
        "无: 删缓存不动个人文件, 但别删 Windows 目录下的未知文件",
        "现场可修",
        "MS",
        "MS 更新错误表归为「磁盘空间耗尽」",
        "0xc01a001d c01a001d status log full cipan kongjian",
        "SYS",
        "Microsoft",
        "1) 系统盘剩余空间大于 20GB"
        " 2) 原安装/更新能继续"
        " 3) 事件日志无 STATUS_LOG_FULL",
    ),
]

# ---------------------------------------------------------------- 屏幕提示
SELF = [
    (
        "BitLocker 恢复",
        "Enter the password to unlock this drive / Enter the recovery key to unlock this drive",
        "BitLocker 恢复界面: 要求输入密码或 48 位恢复密钥",
        "C_DSK",
        "Windows 10/11 (专业版及以上, 已启用 BitLocker)",
        "1) 主板/BIOS 更新、改启动顺序、换 TPM、清 CMOS 触发恢复"
        " 2) 系统盘被拆到别的机器"
        " 3) 固件模式 (UEFI/Legacy) 或安全启动状态改变",
        "1) 先在微软账户里找恢复密钥: account.microsoft.com/devices/recoverykey"
        " 2) 域/工作账户环境找管理员取密钥"
        " 3) 输入 48 位密钥解锁, 进去后立即备份密钥"
        " 4) 用 @manage-bde -status 确认保护状态",
        "密钥找不到就不要尝试格式化以外的操作; 强行重装同样读不到数据",
        "高: 没有恢复密钥将无法取回数据",
        "必须返厂",
        "MS",
        "屏幕原话不是错误码; 常见触发原因是固件或硬件变动",
        "bitlocker recovery huifu miyao 48wei",
        "SELF",
        "Microsoft",
        "1) 解锁后 manage-bde -status 显示「完全解密」或「已加密」正常状态"
        " 2) 重启不再进入恢复界面"
        " 3) 已把恢复密钥另存到非本机位置",
    ),
]
