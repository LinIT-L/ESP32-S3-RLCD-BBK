# ESP32-S3-RLCD-BBK

**复古电子词典 / 掌上游戏机固件** — 基于 `ESP32-S3 + ST7305 反射式 1bit LCD(400×300)`。
内置步步高(BBK 4980 系列)电子词典模拟器，以及 GB / GBC / NES / MD / SMS / Arduboy / 文曲星 / 暴龙机
等多款掌机游戏引擎，还有科学计算器、电子书阅读器、密钥管理器、仿真键鼠、收藏系统等多个实用应用。

> **在线刷机**: https://linit-l.github.io/ESP32-S3-RLCD-BBK/flash/ （浏览器 WebSerial 直刷 16MB 固件，页面随仓库内置）
> **固件镜像**: `flash/merged_16mb.bin`（16MB 全量镜像）**/ Release 附件** `LinTOS_v1.3_16MB.bin`（内容一致）

---

## 隐藏功能

> **隐藏游戏**（GB / GBC / NES / MD / SMS / Arduboy）：

1. 主菜单 → **设置** → **「请作者喝杯水」**（全屏赞助图）
2. 赞助图上**连按确认键 5 次**
3. 返回主菜单即可看到这些引擎图标

> **隐藏设置**（音频方案 / 禁用触摸屏）：

1. 主菜单 → **设置** → **系统信息**
2. 在 **「BY: LinIT」** 一行**连点 5 次**

---

## 功能

### 模拟器 / 游戏引擎
- **BBK 电子词典**：内置词典与游戏，支持存档
- **GB / GBC**：经典掌机游戏
- **NES**：红白机游戏
- **MD / SMS**：世嘉五代 / 大师系统游戏
- **Arduboy**：掌机小游戏（另含内置迷你游戏，支持 TF 卡加载 .hex）
- **文曲星**：运行 `.lav` 游戏
- **暴龙机**：虚拟宠物养成

### 应用 / 工具
- **应用管理**：分类网格找应用，点开即用
- **科学计算器**：四则/幂/阶乘/三角/对数/常量 π·e·Ans，角度弧度切换
- **电子书阅读器**：TXT / FB2 / EPUB，后台索引秒开，目录识别，可调字体字号、旋转方向
- **密钥管理器**：统一 PIN 保护（≥4 位、错误锁定、加密派生）
- **修改机**：游戏数据修改
- **摩尔斯电码**：SOS 求救演示、编码 / 解码练习
- **网络检测**：局域网扫描，设备发现与连通性检测
- **WiFi 探针**：无线设备探测，MAC 厂商（OUI）识别
- **系统日志诊断**：设备运行日志查看 / 导出，排障用
- **闹钟**：定时提醒（震动 + 铃声）
- **仿真键鼠**：把设备当作 USB 键盘 / 鼠标使用
- **U盘模式**：TF 卡以 U 盘形式挂载到电脑读写
- **USB 网卡**：USB 网络适配，设备充当有线网卡
- **电脑维修思路诊断**：给出硬件故障的排查思路与解决步骤
- **收藏系统**：快速收藏常用游戏，各引擎独立保存
- **存储管理**：TF 卡挂载与查看
- **壁纸屏保**：星空 + 游戏壁纸（待机时跑游戏）
- **番茄钟 / MP3 播放 / 天气时钟 / 系统信息**

### 输入 / 交互
- 蓝牙手柄（BLE）、Wi-Fi 网页手柄（AP 直连 / 局域网两种模式）
- 屏幕虚拟按键 + 触摸手势 + 物理按键
- **震动反馈**：按键模拟 / 预设花样 / 随音乐震动

---

## 按键与触摸

### 物理按键

| 按键 | 短按 | 长按 |
|---|---|---|
| **KEY** (GPIO18) | 确认 | 多功能键（收藏等） |
| **BOOT** (GPIO0) | 右 | 返回 (BACK) |
| **PWR** (GPIO1) | 锁屏 | 0.5s=返回主菜单; **2s=关机** |

### 触摸手势

| 手势 | 效果 |
|---|---|
| **点击** | 确认（点哪进哪） |
| **左右滑** | 主菜单切图标 / 游戏内左右 |
| **上下滑** | 列表选择 |
| **底部上滑** | 返回（1s 内再滑一次=强制回主菜单） |
| **长按 ≥2s**（不移动） | 收藏 / 取消收藏 |

### 各功能操作
- **BBK 词典**：左侧选文件夹/收藏，右侧选游戏，点击启动；游戏中点=确认、滑=方向
- **电子书**：先到书架选书；阅读时点上半=上一页、下半=下一页、中间=设置
- **蓝牙手柄**：仅支持 BLE（蓝牙 4.0+），不支持传统蓝牙；实测闪玩 Q36 正常
- **WiFi 网页手柄**（两种模式，在「蓝牙手柄/WiFi手柄」设置里切换）：
  - **AP 直连模式**（默认）：设备自建热点 `BBK-WIFI-handle`（无密码）→ 手机连上热点后浏览器**自动弹出** `http://8.8.8.8` 手柄页
  - **局域网模式**（WiFi 共享）：设备先连接路由器（设置 → 连接 WiFi）→ 同一局域网内的手机/电脑访问 `http://<设备IP>/` 即可玩
  - 按键映射见下图

  ![Wi-Fi 手柄映射](images/Wi-Fi手柄映射.jpeg)
- **壁纸 / 番茄钟 / MP3**：在应用管理或主菜单进入，按提示操作即可

---

## 硬件说明

MCU：**ESP32-S3**（240MHz 双核，16MB Flash，8MB PSRAM）。

> ⚠️ **ESP32-S3 开发模组注意**：若使用裸模组/开发板无法启动，请确认 **EN 脚已上拉到 3.3V**
> （部分模组 EN 未做上拉，会导致完全无法启动），必要时外接 10kΩ 上拉到 3.3V。

### 路线一：微雪（Waveshare）ESP32-S3-RLCD-4.2 开发板

可直接烧录使用，自带屏幕。**该板无触摸屏**，需按键操作（建议配蓝牙手柄）。

![微雪正面](images/微雪ESP32-S3-RLCD-4.2正面图片.jpeg)

### 路线二：拼多多「拼音学练机」改造（本项目实际硬件）

拆机换 ESP32-S3，接线如下：

![拼音学习机正面](images/拼音学习机正面图.jpeg)

![内部布局](images/内部布局图.jpeg)

| 外设 | 引脚 | 说明 |
|---|---|---|
| 屏幕 ST7305 | DC=5, CS=40, SCK=11, MOSI=12, RST=41 | SPI2, 400×300 1bit 反射式（**TE 不接**） |
| 触摸屏 | SDA=15, SCL=7, INT=17, RST=2 | 自动识别 GT911 / CST816 / FT6236 |
| TF 卡 | CMD=21, CLK=38, D0=39 | SDMMC 1bit |
| 按键 ×3 | BOOT=GPIO0, KEY=GPIO18, PWR=GPIO1 | 右/返回, 确认/多功能, 锁屏/关机 |
| 声音 | I2S GPIO8/9/45 → ES8311 或 NS4168 功放（同路 I2S，自动识别） | PA_EN=GPIO46 |
| 震动 | PWM=GPIO42, EN=GPIO48（TM6604 线性驱动，190Hz LRA） | 自制板新增 |
| USB 电源 | AMS1117(5V→3.3V) + 充放电模块 | 供电 / 电池充电 |

> **接线提醒**：触摸 INT/RST 必须接（否则触摸失效）；功放使能 PA=46；
> 若不接解码器，到「隐藏设置」把音频方案设为**「方波直驱(PWM)」**或**「禁用」**。
> 完整 GPIO 占用、分区布局见 [docs/硬件与IO.md](docs/硬件与IO.md)。

**主板测试点定义**（焊接参考）：

![测试点定义图](images/测试点定义图.jpg)

---

## 目录结构

```
├── main/                    # 主循环 + remcon 串口远程控制台 + 用户配置
├── components/              # 40 个模块组件（内核/引擎/应用/连接/驱动）
│   ├── os/                  # 微内核：页面注册/弹窗/应用管家/状态栏
│   ├── engine_manager/      # 引擎注册表（用时载入、退出释放）
│   ├── gam4980/ gb_emu/ gbc_emu/ nes_emu/ md_emu/ sms_emu/
│   │   └── arduboy/ arduboy_avr/ lavax/ vpet/    # 游戏引擎
│   ├── calc/ cheater/ keyvault/ book_reader/ favorites/
│   │   └── audio_player/ tone_player/            # 系统应用
│   ├── wifi_manager/ wifi_probe/ macoui/ dns_server/ web_gamepad/
│   │   └── bt_ctl/ usb_hid/ usbh_msc/ usb_net/ usb_bt/  # 连接能力
│   ├── st7305/ board_shim/ board_battery/ touch_panel/ input/
│   │   └── vibrator/ fonts/ wallpapers/          # 驱动/资源
│   └── ...（其余组件）
├── system_image/            # BBK 词典系统 ROM（8.BIN + E.BIN，固件必需）
├── flash/                   # 在线刷机页面 + 16MB 全量固件镜像
├── images/                  # 硬件接线 / 手柄映射图
├── tools/                   # 图标 / 字库生成脚本 + 烧录脚本
├── docs/                    # 开发指南 / 架构约定 / 硬件IO / 系统设计 / 规划记录
├── build.sh                 # 一键构建（fullclean + 合并 16MB 镜像）
├── merge_flash.sh           # 合并 16MB 镜像
├── pack_firmware.sh         # 发布固件打包
├── partitions.csv           # 分区表
└── sdkconfig.defaults       # 构建配置
```

---

## 构建

要求 ESP-IDF **v5.5.x**（本项目在 v5.5.5 验证，工具链 `esp-14.2.0_20260121`）：

```bash
bash build.sh        # 一键构建（fullclean + 编译 + 合并 16MB 镜像到 dist/）
```

也可手动：`idf.py build` 增量编译，再 `bash merge_flash.sh` 合并镜像。

---

## 烧录

**方式一：全量 16MB 镜像（新设备 / 含系统 ROM，推荐）** — 烧 `dist/merged_16mb.bin`（即 Release 附件 `LinTOS_v1.3_16MB.bin`）到 `0x0`：

```bash
python -m esptool --chip esp32s3 -b 460800 write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 dist/merged_16mb.bin
```

**方式二：应用更新（分区不变，小改快刷）** — 烧 `build/LinTOS.bin` 到 `0x20000`：

```bash
python -m esptool --chip esp32s3 -b 460800 write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x20000 build/LinTOS.bin
```

> 波特率固定 **460800**（921600 长写入会中途掉线）；`--after hard_reset` 写完自动重启。

---

## 系统 ROM

BBK 词典引擎需要系统 ROM `8.BIN` + `E.BIN`（已随仓库附于 `system_image/`）。
`build.sh` 会在本机存在 `4988.font` + `0E00.DAT` 时自动重建（`SYS_ROM_DIR`），否则沿用仓库自带。

---

## 文档

- [docs/开发指南.md](docs/开发指南.md) — 新功能/新页面全流程 + UI 规范
- [docs/架构与工程约定.md](docs/架构与工程约定.md) — 分层/契约/内存/编码约定
- [docs/硬件与IO.md](docs/硬件与IO.md) — 引脚/外设/分区/数据存储
- [docs/系统设计.md](docs/系统设计.md) — Recovery 恢复系统 + remcon 控制台
- [docs/开发记录与规划.md](docs/开发记录与规划.md) — 历史排障记录 / 未来规划

版本变更见 **[CHANGELOG.md](CHANGELOG.md)**。

---

## 版权与参考

本项目因内含 GPLv2 代码（gb_emu/gnuboy），整体按 **GPLv2** 发布，详见 [LICENSE](LICENSE)。

- **BBK 4988 内核**： [gam4980](https://github.com/ThisBoringWorld/gam4980)
- **GB/GBC、NES**： [esp-box-emu](https://github.com/espressif/esp-box-emu)（gnuboy、noFrendo）
- **MD/SMS**： [gwenesis](https://github.com/RafaGarciaM/gwenesis)、smsplus
- **Arduboy 适配**： 精简自 [UVE5](https://github.com/losehu/UVE5)
- 部分界面/模拟器思路参考 **esp32-s3-rlcd-gb-emulator**
- 感谢 AI 与 **DeepSeek** 在重构中的协助