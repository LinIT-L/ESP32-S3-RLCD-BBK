# 硬件与 IO（三档板 · 一个固件通吃）

> 真相源 = `components/drivers/board_pins/board_pins.c`（三张表 + `board_pins_get()`）。
> 识别 = `components/services/hw_cfg/hw_cfg.c`（开机探 I2C 指纹）。
> 网表来源：`Netlist_Schematic1_1_2026-10-24.enet`（小智）/ `Netlist_Schematic1_2026-10-24.enet`（标准）；微雪商用板无原理图，按官方文档。

## 1. 引脚总表

| 项 | ① 小智一体版 | ② 微雪原版 | ③ 标准扩展版 |
|---|---|---|---|
| 主控 | ESP32-S3-AI-2E（U2） | 板载 ESP32-S3 | ESP32-S3-WROOM-1-N16R8（U4） |
| LCD DC/CS/SCK/MOSI/RST | 7/11/9/10/**不接** | 5/40/11/12/**41** | 5/40/11/12/**不接** |
| LCD SPI 时钟 | 33 MHz（分频后实际 26.7 MHz） | 同左 | 同左 |
| 触摸 SDA/SCL/INT/RST | 5/13/12/6 | **无** | 15/7/17/2 |
| 按键 电源/返回/确认 | 1/0/4 | 非 GPIO/0/18 | 1/0/18 |
| 电池 ADC | GPIO3（模组内部，板上无分压） | GPIO4 / ADC1_CH3 | GPIO4（200k/100k，÷3.0） |
| I2S MCLK/BCLK/WS | 40/39/45（模组内部） | 16/9/45 | **无**/9/45 |
| I2S DOUT/DIN | 48/21（模组内部） | 8/10 | 8/**无** |
| codec I2C SDA/SCL | 38/47（模组内部） | 13/14 | —（NS4168 无 I2C） |
| 功放使能 PA | 46（模组内部） | 46 | 46（外部 100k 上拉） |
| 震动 PWM | 8 | 48 | 48 |
| SD CLK/CMD/D0 | 16/15/17 | 38/21/39 | 38/21/39 |
| GPS TX/RX/EXTINT | **41/14**/18 | 43/44/47 | 43/44/47 |
| 充电状态 CHRG | 42（**待焊**） | 无 | 无 |
| 识别指纹（I2C） | 38/47 有 `0x18`(ES8311) | 13/14 有 `0x18`(ES8311) | 15/7 有 `0x15`(CST816) |
| 模块内置 | 充电/降压/功放/解码/麦克风 | 板载充电/降压 | **全部自行外加** |

## 2. 8 脚排针（U1 · PM254-1-08）

| 脚 | ① 小智一体版 | ③ 标准扩展版 |
|---|---|---|
| 1 | GND | GND |
| 2 | 3V3 | 3V3 |
| 3 | GPIO14 | GPIO6 |
| 4 | GPIO41 | GPIO16 |
| 5 | GPIO42 | GPIO3 |
| 6 | GPIO44（RXD0） | GPIO10 |
| 7 | GPIO43（TXD0） | GPIO13 |
| 8 | GPIO2 | GPIO14 |

## 3. 芯片 / 存储 / 分区

- ESP32-S3 + 8MB PSRAM（octal 80MHz）+ 16MB flash。
- 分区见 `partitions.csv`：nvs 256K@0x10000 · factory 5M@0x60000 · appdata 6.625M@0x560000（字库@0x960000）· apps 4M@0xC00000。
- 编译：ESP-IDF v5.5.5（`~/esp/esp-idf-v55`），工具链 esp-14.2.0。

## 4. 外设实测要点

**屏** ST7305 400×300 1bpp 反射，无背光。①③ 的 RST **不接 GPIO**、只有 10k 上拉（R2/R7）；② 官方板 RST=41。
⛔ `(1ULL<<rst)` 必须带 `>= 0` 保护，否则 abort ⇒ 重启循环。

**触摸** CST816/836U，I2C_NUM_1 @100kHz，addr `0x15`。单指；双指仅左右半屏各一点。② 无触摸。

**SD/TF** SDMMC 1-bit。上拉：CMD/D0 各 10k→3V3，卡座 CD/DAT3 10k→3V3。
软件：400kHz 探响应 → 25M/20M/5M/400k 梯度挂载。
⛔ 深睡会 hold SD 三脚 ⇒ 开机必须 `gpio_hold_dis`，否则认不到卡（只能断电）。

**音频** ① ES8311（喇叭）+ ES7210（麦克风）在模组内，I2S 与 codec I2C 都不引出；③ NS4168 纯功放（无 MCLK、无录音）。
功放使能 GPIO46 低有效。⛔ 46 是 strapping，禁内部上拉；③ 有 R15 100k 上拉 ⇒ 上电默认导通，固件要尽早拉低。

**震动** TM6604 + 1508 线性马达，EN 靠电阻常开（① R4 10k→VBAT，③ R16 10k→VBAT）。
⛔ 禁 `gpio_reset_pin()`、禁浮空。

**电池/充电** ③ 分压 R14 200k / R13 100k → GPIO4，比例 ÷3.0（固件常量 3.10）；① 板上无分压，ADC 走模组内 GPIO3。
满电压取 30 天峰值，空 3300mV。充电器：① 模组内 LGS4056HDA · ③ TP4057（只接 LED）。
① 的 `LGS4056_CHRG` → 排针 5（GPIO42，开漏低有效，**尚未焊**）；未焊时用 `usb_hid_host_mounted()` 兜底。

**GPS** MAX-M10S-00B-01，UART1 重映射（不占 UART0）。
① 的 TX/RX 是 ESP32 自己的脚：模组 34 脚 = GPIO41 = GPS RXD，22 脚 = GPIO14 = GPS TXD；UART0（43/44）走排针，不冲突。
引脚 6 档自动波特率 38400 → 9600 → 115200 → 57600 → 230400 → 921600，谁先出 NMEA/UBX 用谁。
电源常通，省电靠 `UBX-RXM-PMREQ`（待机 ≈46µA）；⛔ backup 不保留配置 ⇒ 睡醒靠重认，不做 UBX-CFG-PRT 重配。
GSV 默认 5 秒一次 ⇒ ≥38400 时用 `UBX-CFG-VALSET` 提到 1Hz（9600 不提）。

## 5. 上拉实测

| 板 | 电阻 | 连接 |
|---|---|---|
| 小智 | R2 / R5 10k | 3V3 ↔ LCD_RESET / 模组 EN |
| 小智 | R4 10k | VBAT ↔ TM6604 EN |
| 小智 | R6 / R7 10k | 3V3 ↔ GPIO17(SD D0) / GPIO15(SD CMD) |
| 小智 | R8 10k · R1/R3 5.1k | 3V3 ↔ 卡座 CD/DAT3 · USB-C CC1/CC2→GND |
| 标准 | R7 / R4 10k | 3V3 ↔ LCD_RESET / EN |
| 标准 | R15 100k · R16 10k | 3V3 ↔ GPIO46（功放）· VBAT ↔ TM6604 EN |
| 标准 | R6 / R5 10k | 3V3 ↔ GPIO21(SD CMD) / GPIO39(SD D0) |
| 标准 | R14 200k + R13 100k | VBAT ↔ GPIO4 ↔ GND（÷3.0） |

## 6. 不能撞的脚

- **8 / 48 两板对调**：① 震动 8 + I2S DOUT 48；②③ 震动 48 + I2S DOUT 8。
- **GPIO40 双义**：②③ 是 LCD CS，① 是 I2S MCLK。
- **① SD 占 15/16/17，③ 占 38/21/39** —— 最易混：③ 的 15/17 只到 LCD FPC（触摸），① 触摸走 5/13/12/6。
- **① CHRG 未焊**：41/42 是 JTAG 脚 ⇒ 外部 JTAG 不可用，烧录走 USB-Serial-JTAG（19/20）。
- **② 的 PWR 不是 GPIO**：接电源锁存 IC（U3/ECJ23001），长按=硬件断电、单击=开机，固件读不到；能读的只有 BOOT(0) 与 KEY(18)。
  `btn_pwr=18` 只用于**深睡 EXT1 唤醒与门控**；`input` 发现与 KEY 同脚会自动禁用"固件电源键"语义。
- ⛔ 改任何引脚前先全工程搜"这个号还有谁在用" —— 撞脚现象与元凶通常毫无关系（EXTINT 设成 8 ⇒ 一直震；MCLK 设成 40 ⇒ 微雪全黑）。

## 7. ⚠ I2C 没有外部上拉

两块自研 PCB 的 I2C（触摸 5/13、codec 38/47）**没有外部上拉**，只靠模组内部弱上拉。
判据：I2C 偶发 NACK / 触摸失灵 / codec 探测失败先怀疑这条，补 4.7k 到 3V3。

## 8. 换板怎么做

1. **直接换板、开机即可** —— `hw_cfg_init()` 探 I2C 指纹自动定板型，不用重编、不用进设置选板。
2. 探不到（改线/换芯片）→ 设置 → 网络与系统 → 硬件管理 → **重新自检**；仍失败则沿用 NVS 上次成功的板型（不会变砖）。
3. 三块板共用 `dist/LinTOS_16mb.bin`，只维护这一个固件。
