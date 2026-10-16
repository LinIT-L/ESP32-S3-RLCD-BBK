# AMI Aptio Setup Utility (Aptio V) — BIOS Setup 菜单树

- 来源: https://www.axiomtek.com/Download/Download/CEM860/CEM860%20User's%20Manual%20VA1_%2011-21-2011.pdf
- 抓取日期: 2026-10-02
- 数据源: AMI Aptio BIOS Setup 官方风格手册 (Axiomtek CEM860 User's Manual, Ch.4 'AMI BIOS Setup Utility' 逐项); 菜单栏结构另参 ASRock IMB-1235 / ADLINK AXE-7440GW 手册
- 顶层菜单: Main / Advanced / Chipset / Security / Boot / Save & Exit

# Main   (条目 9, 子菜单 0)
  · BIOS Vendor = American Megatrends Inc.  (只读)
  · Core Version = 5.x  (只读)
  · Compliancy = UEFI 2.x; PI 1.x  (只读)
  · Project Version = Aptio V  (只读)
  · Build Date = 01/01/2020  (只读)
  · Access Level = Administrator  (只读)
  - System Language = English   可选: [English / 简体中文]
  - System Date = 03/17/2019   可选: [MM/DD/YY]
  - System Time = 02:55:20   可选: [HH:MM:SS]

# Advanced   (条目 26, 子菜单 11)
  - Launch PXE OpROM = Disabled   可选: [Disabled / Enabled]
  - Launch Storage OpROM = Enabled   可选: [Disabled / Enabled]
  + ACPI Settings  [子菜单]
    - ACPI Sleep State = S3 (Suspend to RAM)   可选: [Suspend Disabled / S1 (CPU Stop Clock) / S3 (Suspend to RAM)]
  + Trusted Computing  [子菜单]
    - TPM Support = Enabled   可选: [Enabled / Disabled]
  + CPU Configuration  [子菜单]
    · Brand String = Intel(R) Core(TM) Processor  (只读)
    - Intel Virtualization Technology = Enabled   可选: [Enabled / Disabled]
    - Hyper-Threading = Enabled   可选: [Enabled / Disabled]
    - Active Processor Cores = All   可选: [All / 1 / 2 / 4]
  + SATA Configuration  [子菜单]
    - SATA Controller(s) = Enabled   可选: [Enabled / Disabled]
    - SATA Mode Selection = AHCI Mode   可选: [IDE Mode / AHCI Mode / RAID Mode]
  + PCH-FW Configuration  [子菜单]
  + USB Configuration  [子菜单]
    - Legacy USB Support = Enabled   可选: [Enabled / Disabled / Auto]
    - USB 3.0 Support = Enabled   可选: [Enabled / Disabled]
  + H/W Monitor  [子菜单]
  + Super IO Configuration  [子菜单]
    + Serial Port 0 Configuration  [子菜单]
      - Serial Port = Enabled   可选: [Enabled / Disabled]
    + Serial Port 1 Configuration  [子菜单]
      - Serial Port = Enabled   可选: [Enabled / Disabled]
    + Parallel Port Configuration  [子菜单]
      - Parallel Port = Disabled   可选: [Enabled / Disabled]

# Chipset   (条目 9, 子菜单 4)
  + System Agent (SA) Configuration  [子菜单]
    + Graphics Configuration  [子菜单]
      - Primary Display = IGFX   可选: [IGFX / PEG / PCI / SG]
      - Primary IGFX Boot Display = CRT   可选: [CRT / DDI1 / LFP / DDI3 / DDI2]
      - Secondary IGFX Boot Display = Disabled   可选: [Disabled / CRT / DDI1 / LFP / DDI3 / DDI2]
      - LCD Panel Type = VBIOS Default   可选: [VBIOS Default]
    + Memory Configuration  [子菜单]
  + PCH-IO Configuration  [子菜单]
    - Wake on LAN = Disabled   可选: [Disabled / Enabled]

# Security   (条目 6, 子菜单 1)
  · Administrator Password = Not Installed  (只读)
  · User Password = Not Installed  (只读)
  > Set Administrator Password  <动作/弹窗>
  > Set User Password  <动作/弹窗>
  - Secure Boot = Disabled   可选: [Enabled / Disabled]
  + Key Management  [子菜单]

# Boot   (条目 6, 子菜单 1)
  - Setup Prompt Timeout = 1   可选: [1 / 3 / 5 / 65535]
  - Bootup NumLock State = On   可选: [On / Off]
  - Quiet Boot = Disabled   可选: [Enabled / Disabled]
  - Fast Boot = Disabled   可选: [Enabled / Disabled]
  · CSM16 Module Version = 07.64  (只读)
  + Boot Option Priorities  [子菜单]

# Save & Exit   (条目 10, 子菜单 1)
  > Save Changes and Exit  <动作/弹窗>
  > Discard Changes and Exit  <动作/弹窗>
  > Save Changes and Reset  <动作/弹窗>
  > Discard Changes and Reset  <动作/弹窗>
  > Save Changes  <动作/弹窗>
  > Discard Changes  <动作/弹窗>
  > Restore Defaults  <动作/弹窗>
  > Save as User Defaults  <动作/弹窗>
  > Restore User Defaults  <动作/弹窗>
  + Boot Override  [子菜单]

