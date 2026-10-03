# Award BIOS (Phoenix/Award CMOS Setup Utility) — BIOS Setup 菜单树

- 来源: https://gl.manuals.plus/m/562ee3f9f59c7ed8db8bf52ce488d7d15c82077c6395f62f1caf72100e95b9c5
- 抓取日期: 2026-10-02
- 数据源: Award BIOS 官方手册(AWARD BIOS SETUP 章) + Gigabyte 8IRXP / Biostar N4SIE-A7 官方主板手册 'Award BIOS CMOS Setup Utility' 主菜单与子菜单
- 顶层菜单: Standard CMOS Features / Advanced BIOS Features / Advanced Chipset Features / Integrated Peripherals / Power Management Setup / PnP/PCI Configurations / PC Health Status / Frequency/Voltage Control / Main Menu

# Standard CMOS Features   (条目 13, 子菜单 4)
  - Date (mm:dd:yy) = Fri, Jan 7, 2000   可选: [mm:dd:yy]
  - Time (hh:mm:ss) = 00:00:00   可选: [hh:mm:ss]
  + IDE Primary Master  [子菜单]
  + IDE Primary Slave  [子菜单]
  + IDE Secondary Master  [子菜单]
  + IDE Secondary Slave  [子菜单]
  - Drive A = 1.44M, 3.5 in.   可选: [None / 360K, 5.25 in. / 1.2M, 5.25 in. / 720K, 3.5 in. / 1.44M, 3.5 in. / 2.88M, 3.5 in.]
  - Drive B = None   可选: [None / 360K, 5.25 in. / 1.2M, 5.25 in. / 720K, 3.5 in. / 1.44M, 3.5 in. / 2.88M, 3.5 in.]
  - Video = EGA/VGA   可选: [EGA/VGA / CGA 40 / CGA 80 / MONO]
  - Halt On = All, But Keyboard   可选: [All Errors / No Errors / All, But Keyboard / All, But Diskette / All, But Disk/Key]
  · Base Memory = 640K  (只读)
  · Total Memory = 65536K  (只读)
  · Extended Memory = 64512K  (只读)

# Advanced BIOS Features   (条目 17, 子菜单 0)
  - Virus Warning = Disabled   可选: [Disabled / Enabled]
  - CPU L1 & L2 Cache = Enabled   可选: [Enabled / Disabled]
  - CPU L2 Cache ECC Checking = Enabled   可选: [Enabled / Disabled]
  - Quick Power On Self Test = Enabled   可选: [Enabled / Disabled]
  - First Boot Device = Floppy   可选: [Floppy / LS120 / HDD-0 / SCSI / CDROM / HDD-1 / HDD-2 / HDD-3 / ZIP100 / USB-FDD / USB-ZIP / USB-CDROM / USB-HDD / LAN / Disabled]
  - Second Boot Device = HDD-0   可选: [Floppy / LS120 / HDD-0 / SCSI / CDROM / HDD-1 / HDD-2 / HDD-3 / ZIP100 / USB-FDD / USB-ZIP / USB-CDROM / USB-HDD / LAN / Disabled]
  - Third Boot Device = LS120   可选: [Floppy / LS120 / HDD-0 / SCSI / CDROM / HDD-1 / HDD-2 / HDD-3 / ZIP100 / USB-FDD / USB-ZIP / USB-CDROM / USB-HDD / LAN / Disabled]
  - Boot Other Device = Enabled   可选: [Enabled / Disabled]
  - Swap Floppy Drive = Disabled   可选: [Disabled / Enabled]
  - Boot Up Floppy Seek = Enabled   可选: [Enabled / Disabled]
  - Boot Up NumLock Status = On   可选: [On / Off]
  - Gate A20 Option = Fast   可选: [Fast / Normal]
  - Typematic Rate Setting = Disabled   可选: [Disabled / Enabled]
  - Security Option = Setup   可选: [Setup / System]
  - OS Select For DRAM > 64MB = Non-OS2   可选: [Non-OS2 / OS2]
  - HDD S.M.A.R.T. Capability = Disabled   可选: [Enabled / Disabled]
  - Full Screen LOGO Show = Enabled   可选: [Enabled / Disabled]

# Advanced Chipset Features   (条目 10, 子菜单 0)
  - DRAM Timing Selectable = By SPD   可选: [By SPD / Manual]
  - CAS Latency Time = Auto   可选: [Auto / 2 / 2.5 / 3]
  - Active to Precharge Delay = Auto   可选: [Auto / 5 / 6 / 7]
  - DRAM RAS# to CAS# Delay = Auto   可选: [Auto / 2 / 3 / 4]
  - DRAM RAS# Precharge = Auto   可选: [Auto / 2 / 3 / 4]
  - System Memory Frequency = Auto   可选: [Auto / 100MHz / 133MHz]
  - AGP Aperture Size (MB) = 64   可选: [4 / 8 / 16 / 32 / 64 / 128 / 256]
  - On-Chip Frame Buffer Size = 1MB   可选: [1MB / 8MB]
  - On-Chip Video Window Size = 64MB   可选: [32MB / 64MB / 128MB]
  - Init Display First = PCI Slot   可选: [PCI Slot / Onboard/AGP]

# Integrated Peripherals   (条目 19, 子菜单 3)
  + OnChip IDE Device  [子菜单]
    - On-Chip Primary PCI IDE = Enabled   可选: [Enabled / Disabled]
    - On-Chip Secondary PCI IDE = Enabled   可选: [Enabled / Disabled]
    - IDE Primary Master PIO = Auto   可选: [Auto / Mode 0 / Mode 1 / Mode 2 / Mode 3 / Mode 4]
    - IDE Primary Master UDMA = Auto   可选: [Auto / Disabled]
    - IDE Primary Slave UDMA = Auto   可选: [Auto / Disabled]
    - IDE Secondary Master UDMA = Auto   可选: [Auto / Disabled]
    - IDE Secondary Slave UDMA = Auto   可选: [Auto / Disabled]
  + Onboard Device  [子菜单]
    - USB Controller = Enabled   可选: [Enabled / Disabled]
    - USB Keyboard Support = Disabled   可选: [Enabled / Disabled]
    - AC97 Audio = Auto   可选: [Auto / Disabled]
    - Onboard LAN = Enabled   可选: [Enabled / Disabled]
  + SuperIO Device  [子菜单]
    - Onboard FDC Controller = Enabled   可选: [Enabled / Disabled]
    - Onboard Serial Port 1 = 3F8/IRQ4   可选: [3F8/IRQ4 / 2F8/IRQ3 / 3E8/IRQ4 / 2E8/IRQ3 / Disabled]
    - Onboard Serial Port 2 = 2F8/IRQ3   可选: [3F8/IRQ4 / 2F8/IRQ3 / 3E8/IRQ4 / 2E8/IRQ3 / Disabled]
    - Onboard Parallel Port = 378/IRQ7   可选: [378/IRQ7 / 278/IRQ5 / 3BC/IRQ7 / Disabled]
    - Parallel Port Mode = SPP   可选: [SPP / EPP / ECP / ECP+EPP]

# Power Management Setup   (条目 9, 子菜单 0)
  - ACPI Function = Enabled   可选: [Enabled / Disabled]
  - ACPI Suspend Type = S1(POS)   可选: [S1(POS) / S3(STR)]
  - Power Management = User Define   可选: [User Define / Min Saving / Max Saving]
  - Video Off Method = DPMS   可选: [DPMS / Blank Screen / V/H SYNC+Blank]
  - MODEM Use IRQ = 3   可选: [3 / 4 / 5 / 7 / 9 / 10 / 11 / NA]
  - Soft-Off by PWRBTN = Instant-Off   可选: [Instant-Off / Delay 4 Sec]
  - Wake-Up by PCI Card = Disabled   可选: [Disabled / Enabled]
  - Power On by Ring = Disabled   可选: [Disabled / Enabled]
  - Resume by Alarm = Disabled   可选: [Disabled / Enabled]

# PnP/PCI Configurations   (条目 4, 子菜单 1)
  - Reset Configuration Data = Disabled   可选: [Disabled / Enabled]
  - Resources Controlled By = Auto(ESCD)   可选: [Auto(ESCD) / Manual]
  + IRQ Resources  [子菜单]
  - PCI/VGA Palette Snoop = Disabled   可选: [Disabled / Enabled]

# PC Health Status   (条目 8, 子菜单 0)
  · System Temperature = 34°C/93°F  (只读)
  · CPU Temperature = 45°C/113°F  (只读)
  · CPU Fan Speed = 3200 RPM  (只读)
  · Vcore = 1.72 V  (只读)
  · Vcc3 = 3.34 V  (只读)
  · Vcc5 = 5.02 V  (只读)
  · +12V = 12.16 V  (只读)
  - Shutdown Temperature = Disabled   可选: [Disabled / 60°C/140°F / 65°C/149°F / 70°C/158°F]

# Frequency/Voltage Control   (条目 3, 子菜单 0)
  - CPU Clock Ratio = Auto   可选: [Auto / 8 X / 9 X / 10 X / 11 X / 12 X]
  - Auto Detect PCI Clk = Enabled   可选: [Enabled / Disabled]
  - Spread Spectrum = Disabled   可选: [Disabled / Enabled]

# Main Menu   (条目 7, 子菜单 0)
  - Select Language = English   可选: [English / 中文]
  > Load Fail-Safe Defaults  <动作/弹窗>
  > Load Optimized Defaults  <动作/弹窗>
  > Set Supervisor Password  <动作/弹窗>
  > Set User Password  <动作/弹窗>
  > Save & Exit Setup  <动作/弹窗>
  > Exit Without Saving  <动作/弹窗>

