# 两板硬件 IO 对照表

> 唯一数据源：`components/system/board_pins/board_pins.c`（`BP_XIAOZHI` / `BP_WEIXUE`）
> 换板：改 `board_pins.h` 里 `BOARD_IS_WEIXUE`（`0`=小智 / `1`=微雪）→ 编译 → 整片 16MB 烧录。

## 一、引脚对照

| 信号 | 小智（AI-2E 模块 + 新载板） | 微雪（ESP32-S3-RLCD-4.2） |
|---|---|---|
| LCD DC | 7 | 5 |
| LCD CS | 11 | 40 |
| LCD SCK | 9 | 11 |
| LCD MOSI | 10 | 12 |
| LCD RST | 不接 | 41 |
| LCD SPI 时钟 | 60 MHz | 60 MHz |
| 触摸 SDA | 5 | — |
| 触摸 SCL | 13 | — |
| 触摸 INT | 12 | — |
| 触摸 RST | 6 | — |
| 按键 PWR | 1 | 18 |
| 按键 RIGHT | 0 | 0 |
| 按键 LEFT（确认） | 4 | 18 |
| 电池 ADC | CH2 = GPIO3 | CH3 = GPIO4 |
| I2S MCLK | 40 | 16 |
| I2S BCLK | 39 | 9 |
| I2S WS | 45 | 45 |
| I2S DOUT | 48 | 8 |
| I2S DIN | 21 | 10 |
| codec I2C SDA | 38 | 13 |
| codec I2C SCL | 47 | 14 |
| 功放使能 | 46 | 46 |
| 震动 PWM | 8 | 48 |
| SD CLK | 16 | 38 |
| SD CMD | 15 | 21 |
| SD D0 | 17 | 39 |
| GPS TX | 43 | 43 |
| GPS RX | 44 | 44 |
| GPS EXTINT | 18 | 47 |

## 二、不能错的三条

1. **确认键**：小智 **4** / 微雪 **18** —— 错则**按键全失灵**。
2. **8 与 48 两板对调**：小智 震动=**8**、音频 DOUT=**48**；微雪 震动=**48**、音频 DOUT=**8** —— 错则**震动乱震 / 没声音**。
3. **LCD RST**：小智**不接** ⇒ 驱动里 `(1ULL << rst_gpio)` 必须有 `>= 0` 保护，否则**反复重启**。
4. **解深睡锁列表**（`app_board.c` 的 `gpio_hold_dis`）必须跟板型一致，漏改 = **屏全黑但串口正常**。

## 三、烧录

```
PYTHONPATH= ./build.sh          # 看 build/LinTOS.bin 时间戳是否更新(别信 "Build completed!")
0x0    build/bootloader/bootloader.bin
0x8000 build/partition_table/partition-table.bin
0x60000 build/LinTOS.bin        # 只烧这段会 boot loop
```
