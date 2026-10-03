# ASUS UEFI BIOS (Advanced Mode) — BIOS Setup 菜单树

- 来源: https://dlcdnets.asus.com/pub/ASUS/mb/LGA1151/H170I-PRO/E11627_H170I-PRO_UM_web_only.pdf
- 抓取日期: 2026-10-02
- 数据源: 华硕官方手册(B85M-D E9357 / H170I-PRO E11627 菜单栏; ROG Z690 BIOS Manual E19569 项目细节) + 仓库真机 IFR dump ref/asus_aptio_v_real_dump.json
- 顶层菜单: My Favorites / Main / Ai Tweaker / Advanced / Monitor / Boot / Tool / Exit

# My Favorites   (条目 0, 子菜单 0)

# Main   (条目 7, 子菜单 1)
  - System Language = English   可选: [English / 简体中文]
  - System Date = 01/13/2022   可选: [MM/DD/YYYY]
  - System Time = 17:57:00   可选: [HH:MM:SS]
  · Access Level = Administrator  (只读)
  + Security  [子菜单]
    > Administrator Password  <动作/弹窗>
    > User Password  <动作/弹窗>

# Ai Tweaker   (条目 14, 子菜单 5)
  - Ai Overclock Tuner = Auto   可选: [Auto / Manual / XMP I / XMP II / D.O.C.P.]
  - ASUS MultiCore Enhancement = Auto   可选: [Auto - Keep booting with ASUS optimized settings / Disabled - Enforce All limits / Enabled - Remove All limits]
  - BCLK Frequency : DRAM Frequency Ratio = Auto   可选: [Auto / 100:133 / 100:100]
  - DRAM Frequency = Auto   可选: [Auto / DDR4-2133MHz / DDR4-2666MHz / DDR4-3200MHz]
  - Performance Core Ratio = Auto   可选: [Auto / Sync All Cores / By Core Usage]
  - CPU SVID Support = Auto   可选: [Auto / Enabled / Disabled]
  + DRAM Timing Control  [子菜单]
  + DIGI+ VRM  [子菜单]
  + Internal CPU Power Management  [子菜单]
  - CPU Core/Cache Voltage = Auto   可选: [Auto / Manual Mode / Offset Mode / Adaptive Mode]
  - DRAM Voltage = Auto   可选: [Auto]
  + TDP Configuration  [子菜单]
  + Precision Boost Overdrive  [子菜单]
  - GPU Boost = Auto   可选: [Auto / Disabled]

# Advanced   (条目 32, 子菜单 19)
  + Platform Misc Configuration  [子菜单]
  + CPU Configuration  [子菜单]
    · Brand String = Intel(R) Core(TM) Processor  (只读)
    - Intel (VMX) Virtualization Technology = Enabled   可选: [Disabled / Enabled]
    - Hyper-Threading = Enabled   可选: [Enabled / Disabled]
    - Active Processor Cores = All   可选: [All / 1 / 2 / 4]
  + System Agent (SA) Configuration  [子菜单]
    + Graphics Configuration  [子菜单]
      - Primary Display = Auto   可选: [Auto / IGFX / PEG / PCIE]
    + Memory Configuration  [子菜单]
  + PCH Configuration  [子菜单]
  + PCH Storage Configuration  [子菜单]
    - SATA Mode Selection = AHCI   可选: [AHCI / RAID]
    + SATA Configuration  [子菜单]
  + PCH-FW Configuration  [子菜单]
  + Thunderbolt(TM) Configuration  [子菜单]
    - Discrete Thunderbolt(TM) Support = Disabled   可选: [Disabled / Enabled]
  + PCI Subsystem Settings  [子菜单]
  + USB Configuration  [子菜单]
    - Legacy USB Support = Enabled   可选: [Enabled / Disabled / Auto]
    - XHCI Hand-off = Enabled   可选: [Enabled / Disabled]
    + USB Single Port Control  [子菜单]
  + Network Stack Configuration  [子菜单]
    - Network Stack = Disabled   可选: [Disabled / Enabled]
  + NVMe Configuration  [子菜单]
  + HDD/SSD SMART Information  [子菜单]
  + APM Configuration  [子菜单]
    - ErP Ready = Disabled   可选: [Disabled / Enabled(S4+S5) / Enabled(S5)]
    - Power On By PCI-E = Disabled   可选: [Disabled / Enabled]
    - Restore AC Power Loss = Power Off   可选: [Power Off / Power On / Last State]
  + Onboard Devices Configuration  [子菜单]
  + Intel(R) Rapid Storage Technology  [子菜单]

# Monitor   (条目 10, 子菜单 2)
  · CPU Temperature = 36°C  (只读)
  · Motherboard Temperature = 32°C  (只读)
  · CPU Fan Speed = 1200 RPM  (只读)
  · Chassis Fan Speed = N/A  (只读)
  · CPU Core Voltage = 1.163 V  (只读)
  · 3.3V Voltage = 3.312 V  (只读)
  · 5V Voltage = 5.040 V  (只读)
  · 12V Voltage = 12.192 V  (只读)
  + Q-Fan Configuration  [子菜单]
  + Chassis Fan(s) Configuration  [子菜单]

# Boot   (条目 22, 子菜单 4)
  - Fast Boot = Disabled   可选: [Disabled / Enabled]
  - Next Boot after AC Power Loss = Normal Boot   可选: [Normal Boot / Fast Boot]
  - Boot Logo Display = Auto   可选: [Auto / Full Screen / Disabled]
  - Post Delay Time = 0 sec   可选: [0 sec / 3 sec / 5 sec / 10 sec]
  - Post Report = 5 sec   可选: [1 sec / 5 sec / 10 sec / Until Press ESC]
  - Boot up NumLock State = On   可选: [On / Off]
  - Wait For 'F1' If Error = Enabled   可选: [Disabled / Enabled]
  - Option ROM Messages = Force BIOS   可选: [Force BIOS / Keep Current]
  - Interrupt 19 Capture = Disabled   可选: [Enabled / Disabled]
  - AMI Native NVMe Driver Support = Enabled   可选: [Disabled / Enabled]
  - Setup Mode = Advanced Mode   可选: [Advanced Mode / EZ Mode]
  + CSM (Compatibility Support Module)  [子菜单]
    - Launch CSM = Disabled   可选: [Enabled / Disabled]
  + Secure Boot  [子菜单]
    - OS Type = Windows UEFI mode   可选: [Windows UEFI mode / Other OS]
    - Secure Boot Mode = Standard   可选: [Standard / Custom]
    + Key Management  [子菜单]
      - Install Default Secure Boot keys = No   可选: [Yes / No]
      - Clear Secure Boot keys = No   可选: [Yes / No]
      - Save all Secure Boot variables = No   可选: [Yes / No]
  + Boot Option Priorities  [子菜单]
  · Boot Override = （列出可用设备，回车即从所选设备启动）  (只读)

# Tool   (条目 11, 子菜单 7)
  + ASUS EZ Flash 3 Utility  [子菜单]
  + ASUS Secure Erase  [子菜单]
  + ASUS User Profile  [子菜单]
  + ASUS SPD Information  [子菜单]
  + MemTest86  [子菜单]
  + ASUS Armoury Crate  [子菜单]
  + MyASUS  [子菜单]
  - BIOS Image Rollback Support = Enabled   可选: [Enabled / Disabled]
  - Publish HII Resources = Disabled   可选: [Disabled / Enabled]
  - Flexkey = Reset   可选: [Reset / Aura On/Off / DirectKey / Safe Boot]
  - Setup Animator = Disabled   可选: [Disabled / Enabled]

# Exit   (条目 4, 子菜单 0)
  > Load Optimized Defaults  <动作/弹窗>
  > Save Changes & Reset  <动作/弹窗>
  > Discard Changes & Exit  <动作/弹窗>
  > Launch EFI Shell from USB drives  <动作/弹窗>

