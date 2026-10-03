# Dell OptiPlex 7090 — BIOS Setup 菜单树

- 来源: https://www.dell.com/support/manuals/en-us/optiplex-7090-sff/d11_optiplex_7090_sff_sm/system-setup-options
- 抓取日期: 2026-10-02
- 数据源: Dell OptiPlex 7090 SFF Service Manual - 'System setup options' 章节 (官方手册表格, 逐表照抄)
- 顶层菜单: General / System Configuration / Video / Security / Secure Boot / Performance / Power Management / POST Behavior / Virtualization Support / Wireless / Maintenance / System Logs / SupportAssist System Resolution

# General   (条目 47, 子菜单 2)
  + System Information  [子菜单]
    --- System Information ---
    · BIOS Version = 1.28.0  (只读)
    · Service Tag = J4K2L83  (只读)
    · Asset Tag =   (只读)
    · Ownership Tag =   (只读)
    · Manufacture Date = 06/11/2024  (只读)
    · Ownership Date =   (只读)
    · Express Service Code = 12345678901  (只读)
    --- Memory Information ---
    · Memory Installed = 16384 MB  (只读)
    · Memory Available = 16000 MB  (只读)
    · Memory Speed = 3200 MHz  (只读)
    · Memory Channel Mode = Dual  (只读)
    · Memory Technology = DDR4  (只读)
    · DIMM 1 Size = 8192 MB  (只读)
    · DIMM 2 Size = 8192 MB  (只读)
    --- PCI Information ---
    · SLOT2 = Empty  (只读)
    · SLOT3 = Empty  (只读)
    · SLOT5_M.2 = M.2 PCIe SSD  (只读)
    --- Processor Information ---
    · Processor Type = Intel(R) Core(TM) i7-11700  (只读)
    · Core Count = 8  (只读)
    · Processor ID = 0x0000000000000000  (只读)
    · Current Clock Speed = 2500 MHz  (只读)
    · Minimum Clock Speed = 800 MHz  (只读)
    · Maximum Clock Speed = 4900 MHz  (只读)
    · Processor L2 Cache = 4 MB  (只读)
    · Processor L3 Cache = 16 MB  (只读)
    · HT Capable = Yes  (只读)
    · 64-Bit Technology = Yes  (只读)
    --- Device Information ---
    · SATA-0 = None  (只读)
    · SATA-1 = None  (只读)
    · M.2 PCIe SSD-0 = 512 GB  (只读)
    · LOM MAC Address = 00:00:00:00:00:00  (只读)
    · Video Controller = Intel(R) UHD Graphics 750  (只读)
    · Audio Controller = Realtek ALC3246  (只读)
    · Wi-Fi Device = Intel(R) Wi-Fi 6 AX201  (只读)
    · Bluetooth Device = Intel(R) Bluetooth  (只读)
  + Boot Sequence  [子菜单]
    - Boot Mode = UEFI   可选: [UEFI / Legacy]
    - Boot List Option = UEFI   可选: [Legacy / UEFI]
    · Boot Sequence = （可启动设备顺序列表：选中设备后用 +/- 或空格调整）  (只读)
  - UEFI Boot Path Security = Always, Except Internal HDD   可选: [Always, Except Internal HDD / Always / Never]
  - Date/Time = MM/DD/YY HH:MM:SS   可选: [MM/DD/YY HH:MM:SS]

# System Configuration   (条目 16, 子菜单 3)
  - Integrated NIC = Enabled with PXE   可选: [Disabled / Enabled / Enabled with PXE]
  - Enable UEFI Network Stack = Enabled   可选: [Enabled / Disabled]
  - SATA Operation = AHCI   可选: [Disabled / AHCI / RAID On]
  + Drives  [子菜单]
    - SATA-0 = Disabled   可选: [Enabled / Disabled]
    - SATA-1 = Disabled   可选: [Enabled / Disabled]
    - M.2 PCIe SSD-0 = Enabled   可选: [Enabled / Disabled]
  - SMART Reporting = Disabled   可选: [Enabled / Disabled]
  + USB Configuration  [子菜单]
    - Enable USB Boot Support = Enabled   可选: [Enabled / Disabled]
    - Enable Front USB Ports = Enabled   可选: [Enabled / Disabled]
    - Enable Rear USB Ports = Enabled   可选: [Enabled / Disabled]
    - Front USB Configuration = Enabled   可选: [Enabled / Disabled]
    - Rear USB Configuration = Enabled   可选: [Enabled / Disabled]
  - Audio = Enabled   可选: [Enabled / Disabled]
  + Miscellaneous Devices  [子菜单]

# Video   (条目 2, 子菜单 0)
  - Multi-Display = Enabled   可选: [Enabled / Disabled]
  - Primary Display = Auto   可选: [Auto]

# Security   (条目 17, 子菜单 3)
  · Admin Password = Not Set  (只读)
  · System Password = Not Set  (只读)
  · Internal HDD-0 Password = Not Set  (只读)
  + Password Configuration  [子菜单]
  - Password Change = Permitted   可选: [Permitted / Denied]
  - UEFI Capsule Firmware Updates = Enabled   可选: [Enabled / Disabled]
  + PTT Security  [子菜单]
    - PTT On = Enabled   可选: [Enabled / Disabled]
    - Clear = Disabled   可选: [Enabled / Disabled]
  - PPI ByPass for Clear Command = Disabled   可选: [Enabled / Disabled]
  - Absolute(R) = Enabled   可选: [Enabled / Disabled]
  - Admin Setup Lockout = Disabled   可选: [Enabled / Disabled]
  - Master Password Lockout = Disabled   可选: [Enabled / Disabled]
  - SMM Security Mitigation = Disabled   可选: [Enabled / Disabled]
  + Intel Software Guard Extensions  [子菜单]
    - Intel SGX Enable = Disabled   可选: [Enabled / Software Controlled / Disabled]
    - Enclave Memory Size = 128 MB   可选: [32 MB / 64 MB / 128 MB]

# Secure Boot   (条目 7, 子菜单 1)
  - Secure Boot Enable = Enabled   可选: [Enabled / Disabled]
  - Secure Boot Mode = Deployed Mode   可选: [Deployed Mode / Audit Mode]
  - Deployed Mode = Enabled   可选: [Enabled / Disabled]
  - Audit Mode = Disabled   可选: [Enabled / Disabled]
  + Expert Key Management  [子菜单]
    - Expert Key Management = Disabled   可选: [Enabled / Disabled]
    - Custom Mode Key Management = PK   可选: [PK / KEK / db / dbx]

# Performance   (条目 5, 子菜单 0)
  - Multi Core Support = All   可选: [All / 1 / 2 / 3]
  - Intel SpeedStep = Enabled   可选: [Enabled / Disabled]
  - C-States Control = Enabled   可选: [Enabled / Disabled]
  - Intel TurboBoost = Enabled   可选: [Enabled / Disabled]
  - HyperThread control = Enabled   可选: [Enabled / Disabled]

# Power Management   (条目 7, 子菜单 0)
  - AC Recovery = Last Power State   可选: [Power Off / Power On / Last Power State]
  - Enable Intel Speed Shift Technology = Enabled   可选: [Enabled / Disabled]
  - Auto On Time = Disabled   可选: [Disabled / Everyday / Weekdays / Selected Days]
  - USB Wake Support = Enabled   可选: [Enabled / Disabled]
  - Deep Sleep Control = Disabled   可选: [Disabled / Enabled in S4 and S5 / Enabled in S5 Only]
  - Wake on LAN/WLAN = Disabled   可选: [Disabled / LAN Only / WLAN Only / LAN or WLAN]
  - Block Sleep = Disabled   可选: [Enabled / Disabled]

# POST Behavior   (条目 6, 子菜单 0)
  - Numlock LED = Enabled   可选: [Enabled / Disabled]
  - Keyboard Errors = Report   可选: [Report / Do Not Report]
  - Fastboot = Thorough   可选: [Minimal / Thorough / Auto]
  - Extend BIOS POST Time = 0 seconds   可选: [0 seconds / 5 seconds / 10 seconds]
  - Full Screen Logo = Enabled   可选: [Enabled / Disabled]
  - Warnings and Errors = Prompt on Warnings and Errors   可选: [Prompt on Warnings and Errors / Continue on Warnings / Continue on Warnings and Errors]

# Virtualization Support   (条目 2, 子菜单 0)
  - Virtualization = Enabled   可选: [Enabled / Disabled]
  - VT for Direct I/O = Enabled   可选: [Enabled / Disabled]

# Wireless   (条目 4, 子菜单 1)
  + Wireless Device Enable  [子菜单]
    - WLAN = Enabled   可选: [Enabled / Disabled]
    - Bluetooth = Enabled   可选: [Enabled / Disabled]
    - WWAN = Disabled   可选: [Enabled / Disabled]

# Maintenance   (条目 6, 子菜单 0)
  · Service Tag = J4K2L83  (只读)
  - Asset Tag =    可选: []
  - SERR Messages = Enabled   可选: [Enabled / Disabled]
  - BIOS Downgrade = Enabled   可选: [Enabled / Disabled]
  > Data Wipe  <动作/弹窗>
  - BIOS Recovery = Enabled   可选: [Enabled / Disabled]

# System Logs   (条目 1, 子菜单 1)
  + BIOS Events  [子菜单]

# SupportAssist System Resolution   (条目 2, 子菜单 0)
  - Auto OS Recovery Threshold = 2   可选: [Off / 1 / 2 / 3]
  - SupportAssist OS Recovery = Disabled   可选: [Enabled / Disabled]

