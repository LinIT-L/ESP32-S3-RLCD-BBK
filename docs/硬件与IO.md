# 硬件与 IO

> ESP32-S3-RLCD-4.2（步步高词典）模块化掌机的硬件平台信息。

## 芯片与存储
- **主控**：ESP32-S3-WROOM-1-N16R8（240MHz 双核 / 16MB Flash / 8MB PSRAM）
- **PSRAM**：8MB octal 80MHz（`CONFIG_SPIRAM_MODE_OCT` / `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`）
- **Flash**：16MB QIO（`CONFIG_ESPTOOLPY_FLASHMODE_QIO`）

## 显示屏
- **ST7305 反射式 1-bit LCD**，400×300，帧缓冲 15KB（PSRAM）
- 引脚：`RLCD_DC=GPIO5`、`RLCD_SCK=GPIO11`、`RLCD_MOSI=GPIO12`、`RLCD_CS=GPIO40`、`RLCD_RST=GPIO41`

## 触摸
- CST816 / GT911 / FT6236（I2C，`tp_probe_chip` 自动识别）
- I2C：`SDA=GPIO13`、`SCL=GPIO14`

## SD / TF 卡
- **SDMMC 1-bit 模式**（非 SPI），挂载 `/sdcard`
- 引脚：`CMD=GPIO21`、`CLK=GPIO38`、`D0=GPIO39`（D1-D3 NC）
- CD=GPIO17（卡检测，未用）

## 按键
| 键 | GPIO | 说明 |
|---|---|---|
| KEY | 18 | 左键/确认(短按)/返回(长按) |
| BOOT | 0 | 右键 |
| PWR | 1 | 软关机键/锁屏 |
| 其它方向键 | NC | 未物理连接 |

## 音频
- ES8311（I2S）：本机未检测到（非该型号），软件层完整支持
- ES7210 麦克风 ADC（I2S RX, GPIO10）

## 外设映射（user_config.h）
- I2C：SDA=13、SCL=14
- 震动：TM6604 线性驱动（SOT23-6）PWM=GPIO42（原 DRV2605L SDA 焊盘，LEDC 190Hz）、EN=GPIO48（保持原 EN IO），OUT± 接 LRA；原 SCL=GPIO47 空置
- 电池 ADC：GPIO4（ADC1_CH3，200K/100K 分压）

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