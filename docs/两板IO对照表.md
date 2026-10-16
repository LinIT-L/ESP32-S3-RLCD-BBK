# 三块板硬件 IO 对照表（逐项与 linit 核对过）

> 数据源：`components/system/board_pins/board_pins.c` 的板型表 + linit 逐项确认（2026-10-16）。
> 换板：改 `board_pins.h` 的选板宏 → 编译 → **整片 16MB 烧录**。

## 一、三个版本

| 版本 | 说明 |
|---|---|
| **① 小智** | ESP32-S3-AI-2E 模块 + 新载板（带触摸） |
| **② 微雪原版** | 官方 ESP32-S3-RLCD-4.2（暂无触摸） |
| **③ 微雪自定义 PCB** | linit 仿微雪自制（带触摸；NS4168 功放，**无录音**） |

## 二、引脚对照

| 信号 | ① 小智 | ② 微雪原版 | ③ 微雪自定义 PCB |
|---|---|---|---|
| LCD DC | 7 | 5 | 5 |
| LCD CS | 11 | 40 | 40 |
| LCD SCK | 9 | 11 | 11 |
| LCD MOSI | 10 | 12 | 12 |
| LCD RST | 不接（10k 上拉到 3.3V） | 41 | 41 |
| LCD SPI 时钟 | 60 MHz | 60 MHz | 60 MHz |
| 触摸 SDA | 5 | — | **15** |
| 触摸 SCL | 13 | — | **7** |
| 触摸 INT | 12 | — | **17** |
| 触摸 RST | 6 | — | **2** |
| 按键 PWR | 1 | 18 | **1** |
| 按键 RIGHT | 0 | 0 | 0 |
| 按键 LEFT（确认） | 4 | 18 | **1** |
| 电池检测 | GPIO3 / ADC1_CH2 | GPIO4 / ADC1_CH3 | GPIO4 / ADC1_CH3 |
| I2S MCLK | 40 | 16 | 无（NS4168 不需） |
| I2S BCLK | 39 | 9 | **9** |
| I2S WS/LRCK | 45 | 45 | **45** |
| I2S DOUT/SDATA | 48 | 8 | **8** |
| I2S DIN（录音） | 21 | 10 | **无（无录音）** |
| codec I2C SDA | 38 | 13 | — |
| codec I2C SCL | 47 | 14 | — |
| 功放使能 CTRL | 46 | 46 | **46** |
| 震动 PWM | 8 | 48 | **48** |
| SD CLK | 16 | 38 | 38 |
| SD CMD | 15 | 21 | 21 |
| SD D0 | 17 | 39 | 39 |
| GPS TX | 43 | 43 | 43（U0TXD） |
| GPS RX | 44 | 44 | 44（U0RXD） |
| GPS EXTINT | 18 | 47 | **47** |

## 三、不能错的三条

1. **确认键**：小智 **4** / 微雪 **18** —— 错则**按键全失灵**。
2. **8 与 48 两板对调**：小智 震动=**8**、音频 DOUT=**48**；微雪 震动=**48**、音频 DOUT=**8** —— 错则**震动乱震 / 没声音**。
3. **LCD RST**：小智**不接** ⇒ 驱动里 `(1ULL << rst_gpio)` 必须有 `>= 0` 保护，否则**反复重启**。
4. **解深睡锁列表**（`app_board.c` 的 `gpio_hold_dis`）必须跟板型一致，漏改 = **屏全黑但串口正常**。

## 四、烧录

```
PYTHONPATH= ./build.sh          # 看 build/LinTOS.bin 时间戳是否更新(别信 "Build completed!")
0x0    build/bootloader/bootloader.bin
0x8000 build/partition_table/partition-table.bin
0x60000 build/LinTOS.bin        # 只烧这段会 boot loop
```
