# ESP32-S3-RLCD-BBK

**复古电子词典 / 掌上游戏机固件** — 基于 `ESP32-S3 + ST7305 反射式 1bit LCD(400×300)`,
内置步步高(BBK 4980 系列)电子词典模拟器,以及 GB / GBC / NES / Arduboy / 文曲星 / 暴龙机
等多款掌机游戏引擎,还有电子书阅读器、仿真键鼠、收藏系统等多个实用应用。

> **在线刷机**:https://bbk.linit.cn/flash/ (浏览器 WebSerial 直刷 16MB 固件)
> **发布与固件下载**:https://github.com/LinIT-L/ESP32-S3-RLCD-BBK/releases
>
> 💡 主菜单还有 **隐藏游戏(GB/GBC/NES/Arduboy)与隐藏设置**,解锁方法见文末「隐藏功能」。

---

## 功能

### 模拟器 / 游戏引擎
- **BBK 电子词典**:内置词典与游戏,支持存档
- **GB / GBC**:经典掌机游戏
- **NES**:红白机游戏
- **Arduboy**:掌机小游戏(另含内置迷你游戏)
- **文曲星**:运行 `.lav` 游戏
- **暴龙机**:虚拟宠物养成

### 应用 / 工具
- **应用管理**:分类找应用,点开即用
- **电子书阅读器**:支持 TXT / FB2 / EPUB,可调字体字号、旋转方向
- **仿真键鼠**:把设备当作 USB 键盘 / 鼠标使用
- **电脑维修思路诊断**:给出硬件故障的排查思路与解决步骤
- **收藏系统**:快速收藏常用游戏,各引擎独立保存
- **存储管理**:TF 卡挂载与查看
- **壁纸屏保**:星空 + 游戏壁纸(待机时跑游戏)
- **番茄钟 / MP3 播放 / 天气时钟 / 系统信息**

### 输入 / 交互
- 蓝牙手柄按键、Wi-Fi 网页手柄(热点直连即玩)
- 屏幕虚拟按键 + 触摸手势 + 物理按键

---

## 按键与触摸

### 物理按键

| 按键 | 短按 | 长按 |
|---|---|---|
| **KEY** (GPIO18) | 确认 | 多功能键(收藏等) |
| **BOOT** (GPIO0) | 右 | 返回 (BACK) |
| **PWR** (GPIO1) | 锁屏 | 0.5s=返回主菜单; **2s=关机** |

### 触摸手势

| 手势 | 效果 |
|---|---|
| **点击** | 确认(点哪进哪) |
| **左右滑** | 主菜单切图标 / 游戏内左右 |
| **上下滑** | 列表选择 |
| **底部上滑** | 返回(1s 内再滑一次=强制回主菜单) |
| **长按 ≥2s**(不移动) | 收藏 / 取消收藏 |

### 各功能操作
- **BBK 词典**:左侧选文件夹/收藏,右侧选游戏,点击启动;游戏中点=确认、滑=方向
- **电子书**:先到书架选书;阅读时点上半=上一页、下半=下一页、中间=设置
- **蓝牙手柄**:仅支持 BLE(蓝牙 4.0+),不支持传统蓝牙;实测闪玩 Q36 正常
- **WiFi 网页手柄**:开热点(AP:**BBK-WIFI-handle**)→ 手机连热点 → 浏览器打开 `http://8.8.8.8` 即可玩,映射见下图

  ![Wi-Fi 手柄映射](images/Wi-Fi手柄映射.jpeg)
- **壁纸 / 番茄钟 / MP3**:在应用管理或主菜单进入,按提示操作即可

---

## 隐藏功能

> **隐藏游戏**(GB / GBC / NES / Arduboy):

1. 主菜单 → **设置** → **「请作者喝杯水」**(全屏赞助图)
2. 赞助图上**连按确认键 5 次**
3. 返回主菜单即可看到这些引擎图标

> **隐藏设置**(音频方案 / 禁用触摸屏):

1. 主菜单 → **设置** → **系统信息**
2. 在 **「BY: LinIT」** 一行**连点 5 次**
3. 弹出隐藏设置(音频方案、禁用触摸屏)

---

## 硬件说明

MCU:**ESP32-S3**(240MHz, 8MB PSRAM, 16MB Flash)。

> ⚠️ **ESP32-S3 开发模组注意**:若使用裸模组/开发板无法启动,请确认 **EN 脚已上拉到 3.3V**
> (部分模组 EN 未做上拉,会导致完全无法启动),必要时外接 10kΩ 上拉到 3.3V。

### 路线一:微雪(Waveshare)ESP32-S3-RLCD-4.2 开发板

可直接烧录使用,自带屏幕。**该板无触摸屏**,需按键操作(建议配蓝牙手柄)。
原理图:`docs/ESP32-S3-RLCD-4.2-schematic.pdf`。

![微雪正面](images/微雪ESP32-S3-RLCD-4.2正面图片.jpeg)

### 路线二:拼多多「拼音学练机」改造(本项目实际硬件)

拆机换 ESP32-S3,接线如下:

![拼音学习机正面](images/拼音学习机正面图.jpeg)

![内部布局](images/内部布局图.jpeg)

| 外设 | 引脚 | 说明 |
|---|---|---|
| 屏幕 ST7305 | DC=5, CS=40, SCK=11, MOSI=12, RST=41 | SPI2, 400×300 1bit 反射式(**TE 不接**) |
| 触摸屏 | SDA=15, SCL=7, INT=17, RST=2 | 自动识别 GT911 / CST816 / FT6236 |
| TF 卡 | CMD=21, CLK=38, D0=39 | SDMMC 1bit |
| 按键 ×3 | BOOT=GPIO0, KEY=GPIO18, PWR=GPIO1 | 右/返回, 确认/多功能, 锁屏/关机 |
| 声音 | GPIO48 → AXS2005B 功放 → 喇叭(方波 PWM) | 也可接 ES8311 解码 |
| USB 电源 | AMS1117(5V→3.3V) + 充放电模块 | 供电 / 电池充电 |

> **接线提醒**:触摸 INT/RST 必须接(否则触摸失效);功放使能 PA=46;
> 若不接 ES8311 解码器,到「隐藏设置」把音频方案设为**「方波直驱(PWM)」**或**「禁用」**。

**主板测试点定义**(焊接参考):

![测试点定义图](images/测试点定义图.jpg)

---

## 目录结构

```
├── main/                    # 主循环: 渲染 / 输入 / 屏保 / 软关机
├── components/
│   ├── menu/                # 菜单、应用管理、设置、壁纸、收藏、蓝牙、诊断、配置
│   ├── engine_manager/      # 引擎统一管理(用时载入、退出释放)
│   ├── gam4980/             # BBK 电子词典引擎
│   ├── gb_emu/ gbc_emu/     # GB / GBC 引擎
│   ├── nes_emu/             # NES 引擎
│   ├── arduboy/ arduboy_avr/# Arduboy 引擎 + 内置迷你游戏
│   ├── lavax/               # 文曲星 LavaX 引擎
│   ├── vpet/                # 暴龙机(虚拟宠物)引擎
│   ├── book_reader/         # 电子书阅读器
│   ├── usb_hid/             # USB 仿真键鼠
│   ├── usbh_msc/            # TF 卡挂载
│   ├── self_test/           # 硬件自检 / 显示测试
│   ├── audio_player/ tone_player/  # 音频(解码 / 方波)
│   ├── st7305/ board_shim/  # LCD 驱动 / 画面缩放灰度
│   ├── touch_panel/ input/  # 触摸驱动 / 手势
│   ├── virtual_keys/        # 屏幕虚拟按键
│   └── web_gamepad/ dns_server/  # 网页手柄 / 热点
├── flash/                   # 在线刷写页面 + 16MB 固件镜像
├── system_image/            # BBK 词典系统 ROM
├── tools/                   # 图标 / 字库生成脚本、日志工具
├── docs/                    # 交接 / 架构文档
├── CHANGELOG.md             # 版本更新日志
├── sdkconfig.defaults       # 构建配置
├── partitions.csv           # 分区表
├── build.sh                 # 一键构建
├── merge_flash.sh           # 合并 16MB 镜像
└── pack_firmware.sh         # 固件打包
```

---

## 构建

要求 ESP-IDF **v5.5.x**:

```bash
idf.py build        # 构建
./build.sh          # 一键构建 (fullclean + 合并 16MB 镜像)
```

## 烧录

**方式一: 应用更新(分区不变)**
```bash
python -m esptool --chip esp32s3 -b 460800 write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x10000 build/esp32-bbk-emu.bin
```

**方式二: 完整 16MB 镜像(新设备/含系统 ROM)** — `./merge_flash.sh` 生成
`dist/merged_16mb.bin` 后烧 `0x0`。

## 系统 ROM

BBK 词典需要系统 ROM `8.BIN` + `E.BIN`(在 `system_image/`),组装后烧到 `0x900000`:

```bash
cat system_image/8.BIN system_image/E.BIN > system_image/system.bin
./merge_flash.sh
```

版本变更见 **[CHANGELOG.md](CHANGELOG.md)**。

---

## 版权与参考

感谢以下开源项目,以及感谢 AI 与 **DeepSeek**:

- **BBK 4988 内核**: [gam4980](https://github.com/ThisBoringWorld/gam4980)
- **GB/GBC、NES**: [esp-box-emu](https://github.com/espressif/esp-box-emu)(gnuboy、noFrendo)
- **Ab 模拟器适配**: 精简自 [UVE5](https://github.com/losehu/UVE5)
- 部分界面/模拟器思路参考 **esp32-s3-rlcd-gb-emulator**
- 开发交接文档见 `docs/HANDOVER.md`