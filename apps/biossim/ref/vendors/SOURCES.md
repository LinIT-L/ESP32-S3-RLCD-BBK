# 6 品牌 BIOS Setup 菜单树 —— 来源与完整度说明（vendors/）

- 目标：为 `apps/biossim` 补齐 **联想之外** 6 家（Dell / HP / ASUS / AMI / Award / 华为）的真实 BIOS 菜单结构
- 抓取日期：**2026-10-02**
- 数据抓取/整理方式：联网检索 + 官方站点/官方 PDF 下载（curl + pdftotext）+ 仓库内真机 IFR dump 交叉核对；**未使用截图手抄**
- 输出格式：严格照任务给定 schema（`type = item | info | submenu | separator | action`）。
  该 schema 与现成生成器 `tools/biossim_gen_official.py` 完全兼容 —— 生成器只特判 `submenu / info / separator`，
  其余一律当 `item`（读 `values` / `current` / `tip` / `disabled`）。已用 `build_one()` 实测 **6 个文件全部可被直接消费、0 同名子菜单冲突**。
- ⚠ 版权：素材来自各厂商官方公开手册/模拟器，仅供本地对照学习，**不要外发/随固件镜像分发**。

---

## 0. 总表

| 键 | 品牌 / 机型 | 顶层菜单 | 子菜单 | 条目 | 候选值 | 完整度(估) | 主来源类型 |
|---|---|---:|---:|---:|---:|---|---|
| `dell`   | Dell OptiPlex 7090 | 13 | 11 | 122 | 147 | ≈90% | **官方服务手册**（表格逐项） |
| `hp`     | HP EliteBook 840 G5 (F10 Setup) | 4 | 24 | 56 | 17 | ≈80% | **官方 interactive BIOS simulator PDF** |
| `asus`   | ASUS UEFI BIOS (Advanced Mode) | 8 | 38 | 100 | 112 | ≈85% | **官方手册** + **仓库真机 IFR dump** |
| `ami`    | AMI Aptio Setup Utility (Aptio V) | 6 | 18 | 66 | 67 | ≈85% | **官方手册**（Aptio 章逐项） |
| `award`  | Award BIOS (CMOS Setup Utility) | 9 | 8 | 90 | 225 | ≈80% | **官方风格手册** ×3 交叉 |
| `huawei` | HUAWEI MateBook BIOS | 4 | 1 | 11 | 6 | **≈25–30%** | **官方支持文档**（仅零散条目） |

> `顶层菜单`= JSON `menus[]` 个数；`条目` 含各级嵌套；计数由生成脚本统计。

---

## 1. `dell` — Dell OptiPlex 7090（完整度 ≈90%）

- **来源 URL**
  - 主源（官方服务手册「System setup options」章）：`https://www.dell.com/support/manuals/en-us/optiplex-7090-sff/d11_optiplex_7090_sff_sm/system-setup-options`
  - 中文同页：`https://www.dell.com/support/manuals/zh-cn/optiplex-7090-sff/d11_optiplex_7090_sff_sm/系统设置程序选项`
  - 交叉核对：`https://www.manualslib.com/manual/2474732/Dell-Optiplex-7090-Small-Form-Factor.html?page=67`
- **手段**：`WebFetch` 直接抓戴尔官方手册页（纯文本表格），**逐张表照抄**「菜单名 → 项目名 → 说明/默认值」。
- **一级菜单（13）**：`General / System Configuration / Video / Security / Secure Boot / Performance /
  Power Management / POST Behavior / Virtualization Support / Wireless / Maintenance / System Logs / SupportAssist System Resolution`
  —— 全部来自官方手册各表标题 + Dell 真实 F2 菜单顺序。
- **拿到**：13 个顶层菜单名；每菜单的主要项目；`General → System Information` 下 5 组信息（System/Memory/PCI/Processor/Device）全部字段名；
  `Secure Boot / Performance / Power Management / POST Behavior / Virtualization Support` 的项与已知取值；
  `System Configuration` 的 `Integrated NIC / SATA Operation / Drives / USB Configuration / Audio / SMART Reporting` 等。
- **没拿到 / 取舍**
  - `System Information` 各只读字段的「当前值」手册只给字段名不给值 → JSON 里填的是**示例值**（机型/Service Tag 等用参考占位，非真机数据）。
  - **手册只写描述、未给取值表的项**（如 `SATA Operation`、`Fastboot`、`AC Recovery`、`Wake on LAN/WLAN` 等）：
    取值数组采用 Dell 该平台的通用候选值（如 `SATA Operation = Disabled / AHCI / RAID On`），**这是本项目里唯一对你补全的取值**，已在 JSON `tip` 保留手册原文说明。
  - 子菜单内部项手册未展开的（`Password Configuration`、`Miscellaneous Devices`、`BIOS Events`、`Boot Sequence` 设备列表等）→ `sub.items` 留空数组。
  - `Intel Software Guard Extensions`：手册单独成表，真机位于 **Security** 之下，本文件按 Security 的**子菜单**放置。

---

## 2. `hp` — HP EliteBook 840 G5（Computer Setup / F10，完整度 ≈80%）

- **来源 URL**：`https://h10032.www1.hp.com/ctg/Manual/c06511071.pdf`
  （HP 官方 *Interactive BIOS simulator — HP EliteBook 800 G5 Notebook PC Series*）
  - 结构交叉核对：HP ProDesk / EliteBook / Z 系列 MSG「Using Computer Setup (F10) Utilities」章（`Main / Security / Advanced / UEFI Drivers` 四个页签）。
- **手段**：`curl` 下载官方 PDF → `pdftotext -layout` 提取 → 照抄。菜单栏与各页签项目名由 PDF **文本**明确给出（非 OCR 猜测）。
- **一级菜单（4）**：`Main / Security / Advanced / UEFI Drivers`
  （商用小机 F10 就这 4 个页签；退出项在 Main 里，没有单独 Exit 页签 —— 这是 HP 的真实版式）。
- **拿到**：
  - Main：`System Information / System Diagnostics / Update System BIOS / Change Date And Time / Set Machine Unique Data / System IDs /
    Replicated Setup / Save Custom Defaults / Apply Custom Defaults and Exit / Apply Factory Defaults and Exit / Ignore Changes and Exit / Save Changes and Exit / Suppress POST Errors`
  - Security：`Administrator Tools（Create BIOS Administrator Password / Create POST Power-On Password / Password Policy / Fingerprint Reset on Reboot）、
    Security Configuration（TPM Embedded Security / BIOS SureStart / HP Secure Platform Management (SPM) / Physical Presence Interface / Intel SGX）、
    Utilities（Hard Drive Utilities）、Absolute Persistence Module、System Management Command、Restore Security Settings to Factory Defaults`
  - Advanced：`Display Language / Scheduled Power-On / Boot Options / OS Recovery / Secure Boot Configuration / System Options /
    Built-In Device Options / Port Options / Option ROM Launch Policy / Power Management Options / Electronic Labels / MAC Address Pass Through /
    Remote HP PC Hardware Diagnostics(含 Settings / Execute ...)`
  - UEFI Drivers：`3rd Party Option ROM Management`
  - System Information 的字段与模拟器示例值（Product Name / Processor 1 / Memory Size / System BIOS / Born On Date / Serial Number / SKU / UUID …）。
- **没拿到**：各**二级子菜单内部**的逐项（模拟器只给缩略图 + 悬停帮助，PDF 文本未含逐项）→ `sub.items` 留空数组；
  取值表基本未给出，只有模拟器上直接显示默认值的少数项补了 `values`。

---

## 3. `asus` — ASUS UEFI BIOS（Advanced Mode，完整度 ≈85%）

- **来源 URL**
  - 菜单栏（官方手册）B85M-D：`https://dlcdnet.asus.com/pub/ASUS/mb/LGA1150/B85M-D/E9357_B85M-D.pdf`
  - 菜单栏（官方手册）H170I-PRO：`https://dlcdnets.asus.com/pub/ASUS/mb/LGA1151/H170I-PRO/E11627_H170I-PRO_UM_web_only.pdf`
  - 项目/取值（官方手册）ROG Z690 BIOS Manual：`https://dlcdnets.asus.com/pub/ASUS/mb/13MANUAL/E19569_ROG_Z690_Series_BIOS_Manual_EM_WEB.pdf`
  - **仓库内真机 IFR dump**：`apps/biossim/ref/asus_aptio_v_real_dump.json`（此前已抓的 ASUS Aptio V 实机导入串）
- **手段**：`curl` 下载官方 PDF → `pdftotext` → 照抄；真机 dump 用脚本提取页签/条目交叉核对项目名。
- **一级菜单（8）**：`My Favorites / Main / Ai Tweaker / Advanced / Monitor / Boot / Tool / Exit`
  —— 与 B85M-D、H170I-PRO 官方手册的「Menu bar」逐字一致；真机 dump 页签同样含这些项。
  （注：部分 ROG 板写作 `Extreme Tweaker`，本文件按任务要求与主流 PRIME/普通板用 `Ai Tweaker`。）
- **拿到**：
  - `Advanced` 下 15 个子菜单（官方 Z690 手册目录）：`Platform Misc / CPU Configuration / System Agent (SA) / PCH / PCH Storage /
    PCH-FW / Thunderbolt(TM) / PCI Subsystem Settings / USB / Network Stack / NVMe / HDD/SSD SMART Information / APM / Onboard Devices / Intel(R) Rapid Storage Technology`
  - `Boot`：`Fast Boot / Next Boot after AC Power Loss / Boot Logo Display / Post Delay Time / Post Report / Boot up NumLock State /
    Wait For 'F1' If Error / Option ROM Messages / Interrupt 19 Capture / AMI Native NVMe Driver Support / Setup Mode / CSM / Secure Boot / Boot Option Priorities`
  - `Tool`：`ASUS EZ Flash 3 Utility / ASUS Secure Erase / ASUS User Profile / ASUS SPD Information / MemTest86 / ASUS Armoury Crate / MyASUS`（官方手册目录）
  - `Exit`：`Load Optimized Defaults / Save Changes & Reset / Discard Changes & Exit / Launch EFI Shell from USB drives`
  - `Secure Boot` 取值：`OS Type = Windows UEFI mode / Other OS`、`Secure Boot Mode = Standard / Custom`、Key Management 的 Install/Clear/Save 变量。
- **没拿到 / 取舍**：`Ai Tweaker` 因主板/CPU 差异极大，取的是常见 Intel 平台项目（来自 Z690 官方手册）；真机 dump 是 **AMD 板**，
  仅用于核对菜单名与项目叫法，**未直接采用其 AMD 专属超频项**；`PCI Subsystem Settings / Onboard Devices /
  HDD/SSD SMART Information / NVMe` 等子菜单内部项未展开 → 空数组。

---

## 4. `ami` — AMI Aptio Setup Utility（Aptio V，完整度 ≈85%）

- **来源 URL**
  - 主源（官方风格手册，第 4 章「AMI BIOS Setup Utility」逐项）：`https://www.axiomtek.com/Download/Download/CEM860/CEM860%20User's%20Manual%20VA1_%2011-21-2011.pdf`
  - 菜单栏交叉核对：ASRock IMB-1235 `https://download.asrock.com/IPC/Manual/IMB-1235.pdf`、
    ADLINK AXE-7440GW 手册、Lenovo SD350 UEFI User Guide。
- **手段**：`curl` 下载官方 PDF → `pdftotext` → 照抄；多份不同厂商的 Aptio 手册交叉确认**菜单栏与子菜单名**。
- **一级菜单（6）**：`Main / Advanced / Chipset / Security / Boot / Save & Exit`（Aptio V 台机通用版式）。
- **拿到**：
  - `Main`：System Language / System Date / System Time / BIOS Vendor / Core Version / Build Date 等。
  - `Advanced`：`Launch PXE OpROM / Launch Storage OpROM` 及子菜单 `ACPI Settings / Trusted Computing / CPU Configuration /
    SATA Configuration / PCH-FW Configuration / USB Configuration / H/W Monitor / Super IO Configuration`（含 Serial/Parallel Port 子菜单）。
  - 明确取值：`ACPI Sleep State = Suspend Disabled / S1 / S3`、`SATA Mode Selection = IDE Mode / AHCI Mode / RAID Mode`、
    `Legacy USB Support = Enabled / Disabled / Auto`、`Setup Prompt Timeout`、`Bootup NumLock State = On / Off`。
  - `Chipset`：`System Agent (SA) Configuration（Graphics / Memory Configuration）`、`PCH-IO Configuration（Wake on LAN）`。
  - `Security`：Administrator / User Password、Set Password 动作、Secure Boot、Key Management。
  - `Boot`：Setup Prompt Timeout / Bootup NumLock State / Quiet Boot / Fast Boot / CSM16 Module Version / Boot Option Priorities。
  - `Save & Exit`：Save/Discard Changes(and Exit/Reset) / Restore Defaults / Save as User Defaults / Restore User Defaults / Boot Override。
- **没拿到**：手册对部分子菜单只给一句描述（Memory Configuration、PCH-FW、部分 Graphics 项）→ `sub.items` 空数组；
  具体型号的取值集合会与手册机型不同。

---

## 5. `award` — Award BIOS / Phoenix-Award（完整度 ≈80%）

- **来源 URL**（三份交叉，均为可公开访问的手册）
  - Award BIOS SETUP 章：`https://gl.manuals.plus/m/562ee3f9f59c7ed8db8bf52ce488d7d15c82077c6395f62f1caf72100e95b9c5`
  - Gigabyte 8IRXP 官方手册：`https://download1.gigabyte.com/Files/Manual/motherboard_manual_8irxp_other_e.pdf`
  - Biostar N4SIE-A7 官方手册：`https://www.biostar-usa.com/upload/Manual/N4SIE-A7_0531.pdf`
- **手段**：`WebFetch` 抓手册文本 → 照抄；三份交叉确认**主菜单 15 项**与各子屏幕的项目名、取值表。
- **一级菜单（本文件 9 个）**：`Standard CMOS Features / Advanced BIOS Features / Advanced Chipset Features /
  Integrated Peripherals / Power Management Setup / PnP/PCI Configurations / PC Health Status / Frequency/Voltage Control / Main Menu`
- **拿到**：上面 8 个设置屏幕的项目名与经典取值表（如 `Halt On`、`First Boot Device`、`Onboard Parallel Port`、`Resources Controlled By`、
  `Soft-Off by PWRBTN` 等）均来自手册。
- **没拿到 / 重要取舍**
  - Award 的真实 UI 是**单页主菜单 + 二级屏幕**，**没有页签**。为了对齐本次 schema，本文件把 8 个设置屏幕各自当作一个顶层菜单；
    主菜单右下角的 6 个动作（`Load Fail-Safe Defaults / Load Optimized Defaults / Set Supervisor Password / Set User Password /
    Save & Exit Setup / Exit Without Saving`）放进名为 **`Main Menu`** 的顶层菜单（`Main Menu` 正是 Award 主屏幕的官方叫法）。
  - 主菜单项**顺序/集合随主板差异较大**（手册样本多为 i815/i845 时代机型）；个别项在不同板子上可能不存在。
  - `PC Health Status` 的电压/温度读数是示例值（真机为动态读数）。

---

## 6. `huawei` — HUAWEI MateBook BIOS（完整度 ≈25–30%，**最低**）

- **来源 URL**（全部为华为官方支持文档，逐条列出）
  - 安全启动：`https://consumer.huawei.com/cn/support/content/zh-cn15883571`
  - 管理员密码 / 开机密码：`https://consumer.huawei.com/cn/support/content/zh-cn00694834/`
  - PXE 设备：`https://consumer.huawei.com/ae-en/support/content/en-us15811336/`
  - BIOS/EC 版本：`https://consumer.huawei.com/ae-en/support/content/en-us00694750`
  - 如何进入 BIOS：`https://consumer.huawei.com/cn/support/content//zh-cn16009003`
  - 社区官方帖（同安全启动）：`https://cn.club.vmall.com/mhw/consumer/cn/community/mhwnews/thread/id_1000030709929/`
- **手段**：`WebFetch` 抓官方支持页。华为官方**只以「某功能在哪个菜单下」的形式描述**（如「Security > Secure Boot」「Security > POST Password」），
  **未公开完整 BIOS 菜单树或中文菜单截图**。网络上大量「华为 BIOS 教程」经查为**第三方转载或 AI 生成**（个别页面文末自承 AI 写作），**未采用**。
- **能确证的（已写入 JSON）**：
  - 顶层 `Security`（安全）：`Secure Boot`、`SETUP Administrator Password`、`POST Password`
  - 顶层 `Boot`（启动）：`PXE Device Enable`、可启动设备顺序（选中设备后 `F5/F6` 移到首位）
  - 信息页显示 `BIOS Version` / `EC Version`
  - 界面语言可切换为「简体中文」（官方页与多来源一致）；`F10` 保存并退出
- **没拿到 / 诚实标注**
  - **中文一级菜单名**（`信息概览 / 安全设置 / 启动配置 / 退出`）是**按官方英文菜单（Information/Security/Boot/Exit）
    与官方中文功能说明的对应关系整理的**，**未获官方中文原图逐字确认** —— 这点请勿当"官方原文"使用。
  - 除上列条目外，各菜单的**具体项目与取值全部缺失**，已按纪律**留空 / 不写**（`信息概览` 仅 3 项、`启动配置` 的可启动设备列表 `sub.items` 为空）。
  - 建议后续改用真机 F2 截图逐页核对后再补全。

---

## 7. 纪律与可复现

- 全部输出只落在 `apps/biossim/ref/vendors/`，**未改动任何代码、未烧录设备**。
- 生成/校验方式（脚本在 `/tmp`，不进仓库）：
  ```bash
  # 1) 官方 PDF 取文本
  curl -sL -o hp.pdf "https://h10032.www1.hp.com/ctg/Manual/c06511071.pdf" && pdftotext -layout hp.pdf hp.txt
  # 2) 校验 6 个 JSON 均可解析
  python3 -c "import json,glob; [json.load(open(f)) for f in glob.glob('apps/biossim/ref/vendors/*.json')]"
  # 3) 用现成生成器函数实测可被消费（应输出 6 行、冲突均为“无”）
  python3 -c "import sys,glob,os; sys.path.insert(0,'tools'); import biossim_gen_official as g; \
    [g.build_one(os.path.basename(f)[:-5], f,'x','y') for f in sorted(glob.glob('apps/biossim/ref/vendors/*.json'))]"
  ```
- 未包含内容：UI 像素级版式/配色（本批只做**菜单数据**）；任何**编造**的菜单项（每一条都注明了来源类型）。
