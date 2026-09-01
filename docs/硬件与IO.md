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

## 音频（两板一固件：同一路 I2S，零判断）

> V1.0.70 起：官方板 ES8311 与自制板 NS4168 **都吃 I2S 数字信号**，
> 固件按 `es8311_is_present()` 自动识别平台，数据路径完全一致，无需硬件跳线。

### 官方板（微雪 ESP32-S3-RLCD-4.2）
- **audio_player（I2S → ES8311 codec → AAXS2005 功放 → 喇叭）**：
  `BCLK=GPIO9`、`WS=GPIO45`、`DOUT=GPIO8`、`MCLK=GPIO16`、`PA_EN=GPIO46`（高=功放开）
- **ES7210 双麦克风阵列**（I2S RX）：`DIN=GPIO10`

### 自制板（NS4168 I2S 数字功放，⚠️ 本次新增接线）
- **audio_player（I2S → NS4168 直接放大，零配置）**：与官方板**同一路 I2S**
  `BCLK=GPIO9`→NS4168 BCLK、`WS=GPIO45`→NS4168 LRCLK、`DOUT=GPIO8`→NS4168 SDATA
  `CTRL=GPIO46`（高=工作/右声道，低=关断静音）
- ⚠️ NS4168 为 **I2S 数字输入**功放（ESOP8），**无需 MCLK**（GPIO16 可不接）、无输入耦合电容；
  数据直接来自 ESP32 I2S DOUT，芯片自动提取 BCLK/LRCLK/SDATA 解码。
- ⚠️ VDD（Pin6）直接接**电池 3.7V**（NS4168 工作范围 3.0~5.5V，3.7V 约出 1.3W@4Ω；如需最大 2.5W 可改接 5V）

**NS4168 ESOP8 引脚接线表（自制板）：**
| NS4168 引脚 | 接法 | 说明 |
|---|---|---|
| Pin1 CTRL | ← GPIO46 | 高=工作（右声道），低=关断（静音省电） |
| Pin2 LRCLK | ← GPIO45 (I2S_WS) | I2S 帧时钟 = 采样率 |
| Pin3 BCLK | ← GPIO9 (I2S_BCLK) | I2S 位时钟 |
| Pin4 SDATA | ← GPIO8 (I2S_DOUT) | I2S 数据（2's 补码） |
| Pin5 VoN | → 喇叭 - | 输出负端 |
| Pin6 VDD | ← 电池 3.7V | 电源（3.0~5.5V） |
| Pin7 GND | → GND | 地 |
| Pin8 VoP | → 喇叭 + | 输出正端 |

- **tone_player（按键音/开机音/暴龙机音效）**：V1.0.70 改走 **PCM 合成 → I2S**，
  不再用 GPIO6 方波飞线（GPIO6 已释放为**闲置 IO**）。
  官方板经 ES8311、自制板经 NS4168，同一路 I2S 均出声，不绑定任何引擎。
- ⚠️ V1.0.69 曾把 tone_player 从 GPIO48 移到 GPIO6（因 48 与震动 EN 同脚冲突）；
  V1.0.70 起 tone 走 I2S 后 GPIO6 空闲、GPIO48 归震动 EN 专用。

## 震动（⚠️ 自制板新增，官方板无此功能）
- **TM6604 线性驱动**（SOT23-6，PWM 直驱 LRA，无寄存器上电即用）：
  - `PWM=GPIO42`（原 DRV2605L SDA 焊盘，LEDC TIMER_1/CH1，190Hz = LRA 谐振）
  - `EN=GPIO48`（保持原 EN IO，新板震动开关，高有效）
  - OUT±（Pin1/3）接 LRA；Pin2 GND、Pin4 VDD=3.3V
  - 原 SCL=GPIO47 空置
- 强度：`out_set(0~100)` → 占空比 0~255（VIB_RES_MAX）

### V1.4.x 触发方式（三档互斥，后发起停前者）
1. **单次 tap**（`vibrator_tap`）：按键/菜单导航/拖动反馈，保持快速响应。
2. **预设花样**（`vibrator_play_pattern`）：段式序列（时长+强度），非阻塞定时器逐段推进。
   内置花样库（`vibrator.h` 直接引用）：
   | 花样 | 内容 | 用途 |
   |---|---|---|
   | `VIB_PAT_TICK` | 60ms 短促 | 轻反馈 |
   | `VIB_PAT_DOUBLE` | 50+60+50ms 双敲 | 确认 |
   | `VIB_PAT_HEARTBEAT` | 90+60+140ms 强弱心跳 | 提示 |
   | `VIB_PAT_SUCCESS` | 三连渐强 | 番茄钟完成 |
   | `VIB_PAT_ERROR` | 低鸣两下+长震 | 报错 |
   | `VIB_PAT_SOS` | 摩尔斯 SOS（3短3长3短） | 求救演示 |
   | `VIB_PAT_RAMP_UP/DOWN` | 5 段渐强/渐弱 | 音量/特效 |
   | `VIB_PAT_ALARM` | 300+180ms **无限循环** | 闹钟（关铃即停） |
   | `VIB_PAT_CLICK` | 18+22+16ms 按下+释放双脉冲 | 按键模拟（触摸点击） |
4. **齿轮震动**（主菜单，V1.4.x）：拖动或惯性滑行中每跨过一个居中图标
   触发一次 10ms/100 短促高强脉冲（"咔哒"手感）；覆盖 fling 松手后的
   惯性动画段（此前全程静默），快速甩动次数与划过的图标数一致；
   25ms 防抖合并同帧跳格，脉冲清晰不粘连。
   - 甩动动画用**抛物线缓出** `1-(1-t)^2`（速度线性衰减），每格 130ms、
     上限 1600ms → 末段减速滑行更长，最后还能慢慢滑过 2-3 格才停。
   - **收尾渐慢**：动画剩余 ≤650ms 进入尾段，跨格震动间隔 = 上一格实际
     间隔 ×1.5 自适应递增（限 90~300ms）——动画快自动拉长间隔、动画慢
     跟格即震，从 ~100ms 平滑渐至 ~300ms，收尾"哒…哒…哒"渐慢不突停。
5. **按键模拟**（`vibrator_click`，V1.4.x）：**按下瞬间立即震动**（TG_IDLE
   按下沿触发，零延迟），模拟机械按键"按下去"的段落感（苹果触控板式）；
   按住 >0.5s 松手再补一次"弹起"震动（`TOUCH_HOLD_VIB_MS 500`），快速
   点击只震一声。覆盖所有触摸按下（页面/弹窗按钮/主菜单图标/拖动起点）。
   设置页"声音震动 → 按键模拟"可关（关后回退为普通 UI tap），NVS
   `vib_key` 持久化；跟随 UI 震动总开关。
   - **上下文屏蔽**：主菜单/软件管家页**直接内容**不震（拖动图标/管理列表
     保持干净）——os_core `os_tick` 每帧按 `current_page + modal_active`
     驱动 `input_set_key_sim_ctx_block`；弹窗覆盖时自动恢复（弹窗内点击
     仍有按键感），二级页面/软件程序照常震动。
3. **随音乐震动**（`vibrator_music_mode`）：MP3/游戏 PCM 两路喂振幅表
   （`audio_player_get_meter`，0..1000），内部任务每 20ms 采样，死区 6% 后
   线性映射 PWM 强度——听歌/游戏时马达随节奏震动。
   MP3 播放页新增"随音乐震动"按钮（循环按钮右侧，黑底=开，NVS `mp3/vib_music` 持久化）。

## 两板接线对照（一份固件，两板通吃）
| 信号 | 官方板（微雪） | 自制板 |
|---|---|---|
| 音频数据 | I2S → ES8311 → AAXS2005 功放 | I2S → NS4168 数字功放（同路 I2S） |
| BCLK / WS / DOUT | 9 / 45 / 8 | 9 / 45 / 8（直连 NS4168） |
| MCLK | GPIO16（ES8311 需要） | 可不接（NS4168 无 MCLK 脚） |
| GPIO46 | PA_EN 功放使能 | NS4168 CTRL（高=工作/右声道） |
| GPIO42 | 空闲 | TM6604 PWM（震动） |
| GPIO48 | 空闲 | TM6604 EN（震动） |
| GPIO6 | 闲置（tone 已改走 I2S） | 闲置（同左） |
| 触摸 | 无 | 飞线面板（SDA=15/SCL=7/INT=17/RST=2） |
| 麦克风 | ES7210（GPIO10 I2S RX） | 可选接 INMP441 直读（代码已支持） |

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
| 46 | 功放 PA/NS CTRL | 48 | 震动 EN |

## GPIO 空闲清单（自制板可复用）
以下 IO 当前**未被占用**，可自由飞线扩展：
| GPIO | 说明 |
|---|---|
| **6** | ✅ V1.0.70 起闲置（旧 tone_player 方波脚已废弃，tone 改走 I2S） |
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
