# 硬件与 IO

> ESP32-S3-RLCD-4.2（步步高词典）模块化掌机的硬件平台信息。
>
> **板型说明**：本机为**自制打样 PCB**，大部分参考微雪官方
> ESP32-S3-RLCD-4.2 设计；**震动（TM6604）为自研新增功能**，官方板无震动。
> 官方板自带 ES8311 功放（I2S 音频），自制板沿用同一音频方案。
> 差异项在下文以 ⚠️ 标注。

## 芯片与存储
- **主控**：ESP32-S3-WROOM-1-N16R8（240MHz 双核 / 16MB Flash / 8MB PSRAM）
- **PSRAM**：8MB octal 80MHz（`CONFIG_SPIRAM_MODE_OCT` / `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`）— 占用 GPIO33-37，不可他用
- **Flash**：16MB QIO（`CONFIG_ESPTOOLPY_FLASHMODE_QIO`）— 占用 GPIO26-32（模组内部）

## 显示屏
- **ST7305 反射式 1-bit LCD**，400×300，帧缓冲 15KB（PSRAM）
- 引脚：`RLCD_DC=GPIO5`、`RLCD_SCK=GPIO11`、`RLCD_MOSI=GPIO12`、`RLCD_CS=GPIO40`、`RLCD_RST=GPIO41`

## 触摸
- CST816 / GT911 / FT6236（I2C_NUM_1，`tp_probe_chip` 自动识别）
- I2C：`SDA=GPIO15`、`SCL=GPIO7`、`INT=GPIO17`、`RST=GPIO2`
- ⚠️ 官方板无触摸，触摸为自制板新增（飞线面板）

## SD / TF 卡
- **SDMMC 1-bit 模式**（非 SPI），挂载 `/sdcard`
- 引脚：`CMD=GPIO21`、`CLK=GPIO38`、`D0=GPIO39`（D1-D3 NC）
- `CD=GPIO17`（卡检测，未启用；与触摸 INT 同脚，因未启用无冲突）

## 按键
| 键 | GPIO | 说明 |
|---|---|---|
| KEY | 18 | 左键/确认(短按)/返回(长按) |
| BOOT | 0 | 右键 |
| PWR | 1 | 软关机键/锁屏 |
| 其它方向键 | NC | 未物理连接 |

## 音频（⚠️ 官方板同款 ES8311 方案）
- **audio_player（I2S → ES8311 功放 → 喇叭）**：
  `BCLK=GPIO9`、`WS=GPIO45`、`DOUT=GPIO8`、`MCLK=GPIO16`、`PA_EN/NS=GPIO46`
- **ES7210 双麦克风阵列**（I2S RX）：`DIN=GPIO10`
- **tone_player（LEDC 方波 → 功放 IN+ 飞线，暴龙机音效）**：`GPIO6`
  - ⚠️ V1.0.69 由 GPIO48 改为 GPIO6：原 GPIO48 与震动 TM6604 EN 同脚，
    暴龙机启动时 tone_player_init 抢占 LEDC 导致震动使能失效，故移脚。

## 震动（⚠️ 自制板新增，官方板无此功能）
- **TM6604 线性驱动**（SOT23-6，PWM 直驱 LRA，无寄存器上电即用）：
  - `PWM=GPIO42`（原 DRV2605L SDA 焊盘，LEDC TIMER_1/CH1，190Hz = LRA 谐振）
  - `EN=GPIO48`（保持原 EN IO，新板震动开关，高有效）
  - OUT±（Pin1/3）接 LRA；Pin2 GND、Pin4 VDD=3.3V
  - 原 SCL=GPIO47 空置
- 强度：`out_set(0~100)` → 占空比 0~255（VIB_RES_MAX）

## 其它
- I2C_NUM_0（官方板 SHTC3 温湿度 / PCF85063 RTC 预留）：`SDA=GPIO13`、`SCL=GPIO14`
  （自制板当前未接 SHTC3/RTC，此 I2C 空闲保留）
- 电池 ADC：`GPIO4`（ADC1_CH3，200K/100K 分压）
- USB：内置 USB-Serial-JTAG（console 日志/烧录），GPIO19/20 物理接 Type-C

## GPIO 总览（自制板实际占用）
| GPIO | 用途 | GPIO | 用途 |
|---|---|---|---|
| 0 | BOOT 键(右) | 1 | PWR 键 |
| 2 | 触摸 RST | 4 | 电池 ADC |
| 5 | LCD DC | 7 | 触摸 SCL |
| 8 | I2S DOUT | 9 | I2S BCLK |
| 10 | I2S DIN(麦) | 11 | LCD SCK |
| 12 | LCD MOSI | 13 | I2C0 SDA(预留) |
| 14 | I2C0 SCL(预留) | 15 | 触摸 SDA |
| 16 | I2S MCLK | 17 | 触摸 INT |
| 18 | KEY 键 | 21 | SD CMD |
| 38 | SD CLK | 39 | SD D0 |
| 40 | LCD CS | 41 | LCD RST |
| 42 | 震动 PWM | 45 | I2S WS |
| 46 | 功放 PA/NS | 48 | 震动 EN |

## GPIO 空闲清单（自制板可复用）
以下 IO 当前**未被占用**，可自由飞线扩展：
| GPIO | 说明 |
|---|---|
| **6** | ⚠️ 已被 tone_player 占用（V1.0.69 起，声音飞线） |
| 3 | 空闲（strapping 引脚，上电需拉高，慎用作输出） |
| 19/20 | USB-OTG D-/D+（Type-C 物理脚，复用会占 USB） |
| 43/44 | 空闲（UART0 默认 TX/RX，console 走 USB 故未用） |
| 47 | 空闲（原 DRV2605L SCL 焊盘） |
| 22-34 | 模组未引出/Flash 专用，不可用 |
| 33-37 | PSRAM 专用，不可用 |

> 说明：GPIO22-25 未引出（WROOM-1 模组），GPIO26-32 内置 Flash，
> GPIO33-37 内置 PSRAM——均不可作普通 IO。

## 分区与存储布局（partitions.csv，16MB）
```
nvs       data nvs    0x9000  0x8000
phy_init  data phy    0x11000 0x1000
factory   app  factory 0x20000 0x400000   # 4MB 系统固件
appdata   data 0x40   0x420000 0xBE0000    # ~11.9MB 数据区
```
appdata 数据区内部布局：
| 段 | 内容 |
|---|---|
| 0~2MB | 步步高 8.BIN（词典字库） |
| 2~4MB | 步步高 E.BIN（屏幕点阵） |
| 4~8.625MB | 字库区（font_zh/light/16 + book* + lav_font） |
| 8.625~11.875MB | os_db 数据库 + 配置 + WASM 应用 + 共享数据 |

## 数据存取三档（恢复系统设计）
| 层级 | 位置 | 内容 |
|---|---|---|
| 重要 | recovery 分区尾部 128KB | 设备ID/注册/余额/时长/关键配置 |
| 普通 | appdata 分区 | 收藏/手柄映射/WiFi 等 |
| 大数据 | TF 卡 | 画板/白板等 |

## 编译环境
- **ESP-IDF v5.5.5**（`~/esp/esp-idf-v55`），工具链 esp-14.2.0
- Python：`idf5.5_py3.13_env`（`idf.py` 需在其 venv 内调用）
- 详见 [开发指南](开发指南.md)
