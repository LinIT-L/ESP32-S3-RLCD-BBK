# ESP32-BBK 模拟器项目交接文档（V1.0.61）

## 1. 项目是什么

ESP32-S3 复古学习机/游戏机：内置步步高电子词典（BBK）、GB/GBC、NES（红白机）、Arduboy 四个模拟器，支持蓝牙手柄、1bit 反射式 LCD、ES8311 音频、TF 卡存游戏。

## 2. 硬件

- MCU: ESP32-S3（240MHz，8MB PSRAM，16MB Flash）
- 屏幕: ST7305 400x300 1bit 反射式 LCD（SPI，与 TF 卡共用 SPI2 总线）
- 音频: ES8311 codec + I2S（MCLK=GPIO16, BCLK=9, WS=45, DOUT=8, PA=46）
- 输入: 物理 KEY(GPIO18)/BOOT(GPIO0) + BLE HID 手柄（Q36，作为 BLE Host 连接）
- 存储: TF 卡（/sdcard），游戏目录 /sdcard/{gam,gb,gbc,nes,ab}
- 串口: USB-Serial-JTAG，设备端 `/dev/cu.usbmodem101`（不固定，可能变化）

## 3. 构建与烧录

- ESP-IDF v5.5.5（`~/esp/esp-idf-v55`），工具链 esp-14.2.0_20260121
- 一键构建：`bash build.sh`（含 fullclean + 合并 16MB 固件）
- 增量构建：`python3 $IDF_PATH/tools/idf.py build`
- 烧录：`python3 $IDF_PATH/tools/idf.py -p /dev/cu.usbmodem101 flash`
- 抓日志：`CAP_SERIAL_PORT=/dev/cu.usbmodem101 python3 tools/cap_serial.py 300`
- 注意：烧录前先停掉占用串口的进程；端口可能从 1101 变成 101 等，用 `ls /dev/cu.usbmodem*` 确认

## 4. 引擎架构（核心）

- **BBK 步步高**：`components/gam4980/`（libretro 核心 + s6502）。V1.0.61 已按用户要求**删除 BBK 音频**（`GAM4980_ENABLE_AUDIO=0`，`audio_player_stop()` 防残留任务）；`cpu_rate=8.0` 是从桌面 `esp32-bbk-emu-lite-backup-20260808.zip` 还原的游戏速度（用户认可"正常速度"），sys_ram(32KB) 在内部 RAM。
- **GB/GBC**：`components/gbc_emu/`（esp-box-emu gnuboy 移植）。音频恢复官方 esp-box-emu 方式：**24000Hz 直喂、无重采样**；`cpu.c` 修复了 CGB 双速模式音频周期减半 bug（`sound_advance` 移到 `clen >>= cpu->speed` 之前）。
- **NES**：`components/nes_emu/`（nofrendo）。灰度帧 2bit 打包 → core0 视频任务拉伸/点对点 → LCD。
- **AB**：`components/arduboy_avr/`（simavr ATmega32u4）。
- **统一渲染/音频**：`components/board_shim/`（LCD 灰度绘制、视频任务）、`components/audio_player/`（MP3 + PCM 环形缓冲）。
- **菜单/设置**：`components/menu/`（分栏游戏菜单、每引擎独立显示模式/灰度、音量 0-9、按键映射）。

## 5. 重要机制（后续接手必读）

1. **显示刷新**：模拟器只出帧，core0 的 `board_video_flush_task` 做灰度转换+SPI。内部 RAM 暂存（s_fb_stage）避免逐像素写 PSRAM 拖垮 core0（否则看门狗/蓝牙掉线）。
2. **音频解耦**：MP3（audio_play_task）与游戏 PCM（audio_out_task）互斥——`audio_player_play` 先停 audio_out，`feed_pcm` 先停 MP3，防止双任务并发写 I2S 崩溃。
3. **蓝牙重连**：断开后**只扫描不盲连**（盲连不可达 MAC 会触发 Bluedroid/HID LoadProhibited 崩溃重启）；断开后 10 秒冷却。手柄开机后设备自动扫描连接，体验类似蓝牙鼠标。
4. **GB/GBC 音频（V1.0.61 修复）**：
   - 进游戏重启根因：旧 `gbc_emu_feed_audio` 在 `produced > want` 时 memcpy 越界写 `s_gbc_audio_out`（16KB→8KB），破坏 PSRAM 相邻数据 → 模拟任务卡死 → IWDT/TWDT 复位。现改为官方直喂（24000Hz，无重采样）。
   - GBC 声音慢一倍根因：`cpu.c` 指令循环先 `clen >>= cpu->speed` 再 `sound_advance`，CGB 双速时音频周期减半 → 每帧只产 ~367 样本（应 735）。已把 `sound_advance` 移到 `>>=` 之前。
   - LCDC 关闭时 `cpu_emulate(32832/3)` 只模拟 1/3 帧 → 开机动画音频慢+粗。已恢复整帧 `cpu_emulate(32832)`。
5. **WRAM 放内部 RAM**（32KB）：gnuboy CPU 热数据，帧耗时从 ~24ms 降到 ~7ms（60fps）。
6. **配置**：`/sdcard/system/config.cfg` + NVS，版本 v5，旧配置自动迁移。

## 6. 当前状态（已知问题）

- **BBK**：用户确认速度恢复正常（cpu_rate=8.0，59fps，retro_run≈12ms）；音频已删除。如需更快可调 `gam4980_emu.c` 的 `gam4980_cpu_rate`（8.0→10/12/16）。
- **GB/GBC**：声音速度正常（24000Hz 直喂），游戏不重启。显示仍为 30Hz 节流（board_shim 视频任务），如需更流畅可提刷新率。
- **TF 卡**：SD watcher 已精简（2s 检查、15s 补挂、游戏/MSC 期间暂停）；USB MSC"挂载到电脑"进入时暂停 watcher（防 deinit 冲突挂死）。
- **蓝牙**：断开后只扫描不盲连（防 LoadProhibited 崩溃）；游戏/拉伸时偶发掉线需继续观察。
- **拉伸模式**：转换已优化（O3+内联+增量索引），30Hz 刷新下 core0 余量充足，蓝牙不再因此掉线。
- **音量**：0-9 档（0 静音，9=100%）。
- **显示模式**：GB/GBC/AB 三档（点对点/全屏/拉伸），按引擎独立存储；NES 两档。

## 6.1 V1.0.61 变更清单（本次会话）

- gam4980：从桌面 `esp32-bbk-emu-lite-backup-20260808.zip` 还原 BBK 引擎（cpu_rate=8.0 游戏速度、vTaskDelayUntil 节流、sys_ram 内部 RAM）；删音频；补 `gam4980_set_key_sound` 兼容空实现。
- gbc_emu/cpu.c：GB/GBC 音频改 24000Hz 官方直喂；修 CGB 双速周期减半；修 LCDC-off 1/3 帧。
- sd_scan.c：快速挂载（2 档速度）；watcher 轻量化（15s 补挂、无动态调速死代码）；USB MSC 进入时暂停 watcher。
- menu_system.c：MSC 进入/失败路径暂停/恢复 watcher；删除收藏栏 DIAG 日志。
- sdkconfig：恢复默认（IWDT 开、TWDT panic 关）。
- 备份：当前状态 = `../esp32-bbk-emu-lite-current-20260809-17h.tar.gz` 基础上替换上述文件；另存桌面 `esp32-bbk-emu-lite-backup-20260808.zip`（旧 BBK 参考）。

## 6.2 V1.0.62 电子书模式（本次会话）

- **电子书菜单**（镜像游戏双栏布局）：主菜单新增「阅读」项（自绘书本图标，icons_data.inc 索引 9）。
  - 左栏：0=显示设置 / 1=收藏书架 / 2..N=分类目录（/sdcard/books 下的子文件夹）/ 最后一个=临时目录（根目录散放的书，虚拟归集，不物理移动）。
  - 右栏：设置项 / 收藏书籍（长按多功能键或 KEY 收藏，/sdcard/system/fav_book.txt）/ 目录书籍。
  - 按键：左右切换栏，上下选择，确认打开/切换设置，BACK 回主菜单；阅读器内：右键/下键/确认=下一页，左键/上键=上一页，BACK=退出回书单。
- **电子书设置**（显示设置里循环切换，持久化到 config.cfg v6）：
  - 敲击翻页 开/关；敲击灵敏度 低/中/高（阈值 7x/5x/3x + 自适应噪声底）；夜间模式 开/关（硬件反色）；显示页码 开/关；旋转方向 上/下/左/右。
  - **敲击翻页语义：单击 = 下一页，双击（400ms 内两下）= 上一页**（语音翻页已按用户要求删除）。
  - **旋转方向**（config.cfg v7）：循环顺序 **0°→90°→180°→270°**（上→左→下→右），默认「上」(0°, 与主菜单一致)。电子书二级菜单也跟随旋转（主菜单/游戏固定横屏）。
  - **90°/270° 实现（V1.0.62 最终版，用户确认正确）**：不再切面板竖屏窗口（之前窗口值超 ST7305 范围导致黑屏），改为**纯软件旋转**：阅读器按竖屏布局（17 列×24 行，`book_reader.c` 的 `render_portrait` + `s_pfb` 逻辑缓冲）绘制，再映射写回横屏帧缓冲，面板扫描方向不变。
    - 映射（左右相差 180°）：左(rot2) `fx=399-Y, fy=X`；右(rot3) `fx=Y, fy=299-X`。
    - ⚠️ **历史教训：rot3 的 fy 曾误写成 `ST7305_WIDTH-1-X`（399-X），X 小时越界写坏帧缓冲，导致侧栏位置诡异、反复调不好。必须用 `ST7305_HEIGHT-1-X`（299-X）。**
    - 竖屏二级菜单（`render_book_two_cols_portrait`）：侧栏 4 字宽(96px)在分隔线左侧，内容列 184px 在右侧；左转侧栏画在竖屏左（x=4..100），右转画在竖屏右（x=8..104，即 mirror=true），配合各自映射后侧栏都显示在屏幕左侧。
  - 阅读器全屏无标题栏；页码(可关)右下角叠加；敲击指示灯左上角 6×6。180° 走 `st7305_flush_rotated(rot=1)` 纯软件变换；普通 `st7305_flush_from` 每次写回 MADCTL=0x48 防菜单错乱。
  - **无板载重力/加速度传感器**（板上只有 PCF85063 RTC + SHTC3 温湿度 + ES7210 双麦），无法自动按重力旋转；如需可外接 I2C 加速度计（如 LIS3DH/QMI8658）再写驱动。
- **多档 GB2312 字库（V1.0.64）**：`tools/generate_book_font.py` 用 macOS 苹果字体渲染。黑体（STHeiti Medium）= `book20/24/28.fnt`，宋体（Songti SC）= `song20/24/28.fnt`，每档含 GB2312 7445 字 + 常用符号/生僻字扩展（共 7464 字）。经 `target_add_binary_data` 嵌入 flash（XIP 只读，不占 RAM/PSRAM）。支持 UTF-8 / GBK / UTF-16 文本自动识别。
- **阅读器组件**：`components/book_reader/`（font_book.c + book_reader.c），文本缓冲放 PSRAM（上限 6MB），分页 24 列 x 16 行，页偏移表 PSRAM。
- **敲击翻页（麦克风）**：确认板载双麦克风 ES7210 ADC 在 **I2C 0x40**，I2S DIN=GPIO10。audio_player 增加 I2S RX 通道 + `audio_player_mic_start/stop/read`；`es7210.c` 初始化 16kHz/16bit 从模式。敲击检测：10ms 帧能量 + 自适应噪声底；短促冲击(20~120ms)判敲击，单击 400ms 后确认翻下页，400ms 内第二下判双击翻上页（左上角小黑块指示灯）。
- **回归注意**：I2S 现在开机就建 TX+RX 双通道（RX 默认不使能，仅麦克风启动时使能），改动前音频（MP3/游戏）已实测正常；ES7210 探测日志 `ES7210 麦克风 ADC 探测成功: 0x40`。
- **st7305**：普通 `st7305_flush_from` 现在每次会先写回 MADCTL=0x48（横屏），防止阅读器旋转后菜单错乱。

## 6.3 V1.0.62 关键修复：配置断电丢失

- **根因**：`menu_settings` 音量默认档位=10（100%），但 `config_valid()` 校验写死 `volume > 9 拒绝` → 每次开机读 TF/NVS 配置都被判无效 → 回默认值并 `menu_config_save()` 用默认值覆盖保存文件。旋转方向等**全部设置断电重启即还原**。
- **修复**：`config_valid()` 改为 `volume > 10 才拒绝`；`config_unpack` 对 volume 做 10 上限钳制。
- **验证**：开机日志由 `TF 配置 magic/version 不符 → 无保存配置` 变为 `加载配置: 来源=TF卡` + `配置已保存到 TF`。
- **注意**：修复前已被默认值覆盖过一次，用户需重新设置一次旋转方向等；之后断电重启可正常恢复。

## 6.4 V1.0.64 阅读器：方框修复 + 字体家族 + 生僻字（本次会话）

- **"每段前方框"根因（已 100% 定位并用主机端真实小说模拟复现）**：
  - `bf_layout` 把换行符作为该行最后一个字符（line_end = 换行符下标），旧绘制循环把 `BF_CH_NEWLINE` 也当成正文绘制：ASCII '\n' (0x0A) 无字形 → `draw_missing_glyph` 画空心方块。**每段结尾的前一行末尾都会多一个方框**，视觉上正好在下一段"两个空格"前面。
  - 另外旧字库把 U+FEFF/U+200B/U+202F 渲染成"空心方块"字形且收进字库；旧代码只在字形缺失时跳过空白，导致这些字符也画框。
  - **修复**：① 横屏/竖屏绘制循环跳过 `BF_CH_NEWLINE`/`BF_CH_SKIP`；② `is_ws_cp` 扩展（U+2000-200F、2028/2029、202A-202E、2060-206F、FEFF、FFFD、FFFE/FFFF、00AD、180E、变体选择符、标签字符等）并改为**无条件留白**（不依赖字形是否存在）；③ GBK 全角空格 A1A1 缺字形也留白；④ 生成脚本不再把不可见码点烤进字库。
- **阅读字体（V1.0.64 已简化）**：只保留黑体四档 20/24/28/32（见 6.6/6.9）；「字体」切换选项、宋体、SD 字体均按用户要求删除；菜单字体仍是独立的 `font_zh`（24×24 黑体）。
- **生僻字覆盖**：按用户正在读的《蛊真人》全文统计，补齐 30 个 GB2312 外生僻字（嫚屃黒赑槃窸窣跶嬛槑啰蝺豨渟莬烜勠蹚囧瞭咵蟅臜瘆廋齁兇暼嗐尨）到 `rare_cps`，全书 0 缺字、0 方框。
- **清理**：删除陈旧的 `assets/book16.fnt`（未烧录、与 book24 同规格的遗留文件，590KB）。
- **验证**：整本书 73384 页索引正常，翻页日志页首字节干净（`20 20 20 20 ...` = 段首 4 半角空格），配置 v9 从 TF 加载，蓝牙手柄连接正常，无重启。

## 6.5 V1.0.64 关键修复：BBK 进游戏 5% 卡死重启（PSRAM 被字库挤爆）

- **现象**：打开 BBK 游戏，进度条停在 5%（"准备中"）→ 设备重启；其他模拟器正常。
- **串口证据**：
  - `E (7176) INIT: PSRAM 分配失败: sys_flash=0x0 sys_rom_8=0x0 sys_rom_e=0x0 (空闲=3122368)` —— BBK 需要 6MB PSRAM（3×2MB），开机后堆只有 3.1MB。
  - 随后 `gam4980_emu_load` 用 `sys_flash+0x15000`（sys_flash=NULL → 0x15000）当 fread 目标 → `StoreProhibited`（EXCVADDR=0x00015000）→ 重启。
- **根因**：`CONFIG_SPIRAM_RODATA=y` 会把**全部只读数据（字库等 ~4.9MB）开机复制进 PSRAM** 并映射，堆从 ~8MB 掉到 3.1MB；V1.0.62 起加入阅读字库后 rodata 增大，BBK 的 6MB 就再也分不出来（V1.0.63 时其实已临界，只是用户没测 BBK）。
- **修复**：sdkconfig(.defaults) 关闭 `CONFIG_SPIRAM_RODATA` → 字库走 **Flash XIP 按需映射**，开机不占 PSRAM。
- **验证**：主菜单 PSRAM 空闲 3,122,488 → **7,980,920**；`GAM4980: 读取 .gam ... 加载成功`、`MENU: 游戏加载成功, 进入运行循环`；小说阅读正常（73384 页）。
- **注意**：菜单/阅读字库现在都是 Flash XIP（访问时才进 cache），**不占内部 RAM / PSRAM**。之前 HANDOVER 声称 XIP 其实没做到（SPIRAM_RODATA 把它复制进了 PSRAM），现已真正成立。

## 6.6 V1.0.64 字库瘦身

- **菜单字库 font_zh**：由 1.67MB C 源码 hex 文本（`font_zh_data.inc`，已删除）改为紧凑二进制 `components/menu/font_zh.bin`（252KB，`ZH1FNT01` magic + u32 count + 字符表 + 字形），`font_zh.c` 用 `target_add_binary_data` 嵌入 + 懒绑定指针。固件实际占用不变（编译后的 inc 本来也只有 ~258KB），但源码/编译更轻。
- **阅读字库（V1.0.64 最终）**：只保留**黑体**（STHeiti Medium）四档 20/24/28/32，共 ~2.75MB（RLE 压缩，flags bit0=1）；宋体与 SD 字体功能已按用户要求删除。全部 XIP，不占 RAM。
- **字库 RLE 压缩**：字形字节游程编码（0..127=1..128 个 0 字节；128..255=字面字节），`font_book.c` 解码到 `s_glyph_scratch`（32px 最大 128B）后直接 blit；压缩格式与旧格式按 header flags bit0 区分。
- **新字库生成**：`python3 tools/generate_book_font.py`（黑体，默认 20/24/28/32；第三参数可只生成单字号）；`python3 tools/generate_font.py`（菜单 font_zh.bin）。

## 6.7 V1.0.64 屏保期间关闭蓝牙（用户需求）

- **需求**：进入星空壁纸屏保时断开并关闭蓝牙；屏保期间 BT 保持关闭；退出屏保自动开启并重连已配对设备。
- **V1.0.64 实测崩溃与最终方案（重要）**：
  - 初版用 `bt_manager_disable()/enable()`（完整 deinit/reinit）。**按键唤醒即重启**，崩溃：
    `assert failed: spi_flash_disable_interrupts_caches_and_other_cpu cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())`。
  - 根因：`bt_manager_enable()` 用 **PSRAM 栈**（`s_init_task_stack` 是 `EXT_RAM_BSS_ATTR`）创建初始化任务，而 `bt_manager_init → bt_manager_load_paired → nvs_get_blob` 读 Flash 要禁用 cache，PSRAM 栈无法访问 → 断言。main.c 开机路径早就用内部 RAM 栈（有注释警告），但 enable() 路径漏了；且 BBK 引擎加载后内部 RAM 只剩 ~51KB，Bluedroid 重初始化内存也不够。
  - **最终方案：软挂起，不拆卸协议栈**：
    - `bt_manager_suspend()`：断开全部 HID 设备 + 停止扫描 + `s_suspended=true`；3 秒自动重连任务与菜单 5 秒历史重连都检查 `is_suspended()` 跳过。
    - `bt_manager_resume()`：清 `s_suspended`，直接发起对已配对设备的连接（配合 main.c 的 `bt_retry_next_ms=1` 扫描重连兜底）。
    - 屏保进出只调 suspend/resume，**完全不碰 enable/disable 重初始化路径**，稳定无重启。
  - 代价：壁纸期间蓝牙射频未真正断电（协议栈存活但无连接、无扫描）；如需真断电需先解决 enable() 的 PSRAM 栈问题（初始化任务栈改内部 RAM + BBK 内存余量），暂不做。
- **实现**（menu_system.c 屏保区）：
  - `screensaver_bt_off()`：置 `s_bt_suspended` 后调 `bt_manager_suspend()`。在屏保**超时进入**和**预览进入**两处调用。
  - `screensaver_bt_on()`：调 `bt_manager_resume()`。在屏保**输入唤醒**分支和 `screensaver_reset()` 调用（幂等，`s_bt_suspended` 守卫）。
  - main.c 原有退出逻辑 `bt_retry_next_ms=1` 会额外触发 5s 历史设备静默扫描重连，双保险。
- **加固**（bt_manager.c）：`bt_manager_disable()` 增加 `s_ctrl_init` 守卫，控制器未 init 时不再重复 `esp_bt_controller_disable/deinit`（防止屏保与设置里蓝牙开关交替触发时二次 deinit）。
- **注意**：屏保期间手柄无法唤醒（BT 已挂起，只能按物理 KEY/BOOT 退出），这是需求本身；退出后自动重连约 1~10 秒（含 10 秒断开冷却期）。

## 6.8 V1.0.64 壁纸菜单（用户需求）

- **主菜单新增「壁纸」项**（icon_wall，索引 10，96×96 手绘星空相框图标；icons_data.inc 手插数组，未走 generate_icons.py）。点击直接弹窗（手柄/存储同款 list_dialog，不再进全屏子页）。
- **壁纸设置弹窗**（open_wallpaper_dialog）：
  - 壁纸类型 → 子弹窗（* 标记当前）：内置星空 / TF动态图 / 游戏壁纸。
    - 内置星空：设模式即返回。
    - TF动态图：再弹「播放速度」子窗（慢 5fps / 标准 8fps / 快 12fps，config v11 持久化）。
    - 游戏壁纸：直接进游戏选择器（GB/GBC/NES/arduboy/步步高 → 游戏列表），**确认键或多功能键(KEY/F_FAV)长按 = 加入/移出列表**（`[W]` 标记）；持久化 `/sdcard/system/wallpaper_game.cfg`（`引擎|路径`，上限 6）。嵌套栈用到 4 层（设置→类型→引擎→游戏），恰好在 LIST_DIALOG_STACK_DEPTH=4 内。
  - 休眠时间 → 子弹窗（1/2/3/5/10/15/20/30 分钟，* 标记当前）。
  - 测试壁纸 → `s_screensaver_preview=true` 立即进入当前类型壁纸并关闭所有弹窗。
- **屏保行为**（screensaver_enter 按模式分发）：
  - 内置星空：原星空动画 + 挂起蓝牙。
  - TF动态图：循环播放 `/sdcard/wallpaper/` 下的 BMP 帧序列（1/8/24bpp、自动缩放 400×300、按文件名排序、约 8fps、15000B PSRAM 帧缓冲）；无文件回退星空。
  - 游戏壁纸：到时间自动轮换启动列表中的下一个游戏（**不挂起蓝牙**，游戏内可用手柄）；按任意设备按键（KEY/BOOT）强制退出（无确认框）→ 卸载引擎释放内存 → 回主菜单。实现：`s_wallpaper_game_mode`（game_run_loop 内任意 action 即 break）+ `gam4980_set_wallpaper_mode`（BBK 运行循环同理）。
- **注意事项**：游戏壁纸选列表每次进入壁纸轮换下一个（wallpaper_game_rot）；TF 动态图每帧从 SD 读文件解码，8fps 下占用低；壁纸游戏退出后 PSRAM 完全归还。

## 6.9 V1.0.64 精简与界面修复（用户需求）

- **敲击翻页/敲击灵敏度功能彻底删除**：书设置里两个选项移除；book_reader 的敲击任务、麦克风启动、指示灯、poll 全部删除（book_reader_poll 恒 false，book_reader_knock_active 恒 false）；audio_player 麦克风 RX 代码保留但不再启动。
- **字体只保留黑体**：书设置「字体」选项删除；song 家族 4 个文件与 CMake 条目删除；SD 字体扫描/家族 API 删除；font_book 只留 hei 四档。
- **字号档 20/24/28/32（直接显示数字）**：`fsize_name = {"20","24","28","32"}`，循环切换 %4；config book_fontsize 0..3（默认 1=24）。
- **设置值靠右 + 竖屏数字修复**：书设置行格式改为 `标签|值`，横屏 draw_book_game_pane 与竖屏 render_book_two_cols_portrait 都按 `|` 拆分：标签靠左、值右对齐；竖屏新增 `mpfb_draw_ascii8/mpfb_draw_text`（ASCII 8x12），修复竖屏下"字体大小 20"数字看不见的问题（原 mpfb_draw_zh24 跳过 ASCII）。
- **两端对齐（左右边距始终相等）**：`line_justify()` 把每行剩余宽度均匀分到字间距（段落末行/单字行不拉伸），横屏+竖屏渲染都应用；任何字号/边距/字距下左右边距一致；纯视觉处理，不影响分页索引。
- **分区表**：工厂分区 8M → 0x8F0000（8.94MB），system 仍 0x900000；固件从 8.4MB 降到 **5.3MB**，余量充足。

## 6.10 V1.0.64 GBC 颜色反了修复 + 项目备份

- **GBC 颜色反转根因**（esp-box-emu gnuboy `src/lcd.c` `updatepalette`）：CGB 调色板字是硬件 **BGR 序**（bit 0-4=蓝、5-9=绿、10-14=红），旧代码按 RGB 解析 → 红蓝对调 → 1bit 灰度下亮暗反转。DMG 路径 `pal_write_dmg` 已按 RGB 打包所以不受影响（这解释了"GB 正常、GBC 反了"）。
- **修复**：`updatepalette` 按 `hw.cgb` 分支解析——CGB 用 BGR 序，DMG 保持 RGB 序；已 build+flash+串口验证（正常启动、蓝牙连接、无崩溃）。
- **备份（2026-08-10）**：`~/Desktop/esp32-bbk-emu-lite-backup-20260810/`
  - `esp32-bbk-emu-lite-source-20260810.tar.gz`（11.7MB，精简源码：排除 build/managed_components/dist/.git/.engine_backup）
  - `merged_16mb-20260810.bin`（完整 16MB 固件，SHA256 见同名 .sha256）

## 6.11 V1.0.64 番茄钟 + 10 个内置壁纸程序（用户需求）

- **番茄钟**（主菜单新增「番茄钟」，icon_pomo 索引 11）：弹窗设置工作分钟（5..120 步进 5）/休息分钟（1..60）/开始/返回；运行中全屏倒计时 MM:SS + 进度条 + 阶段（工作/休息）循环，任意按键返回主菜单；运行中不进入屏保；工作/休息分钟持久化（config v12）。
- **内置壁纸程序选择**（壁纸 → 壁纸类型 → 内置壁纸 → 程序列表，config v12 `wallpaper_program` 0..10）：
  0 星空（原） / 1 无限楼梯（彭罗斯旋转台阶+攀爬点） / 2 粒子钟（粒子每分钟聚成 HH:MM） /
  3 二进制海浪（0/1 字符随波起伏） / 4 生命花园（温和康威 B3/S23+播种） / 5 板块漂移（Voronoi 板块+海拔密度） /
  6 雾中巨物（旋转立方体+边缘雾） / 7 电路板信号（网格+焊点+脉冲拖尾） / 8 风吹麦浪（密度波） /
  9 赫尔曼栅格（幽灵灰点） / 10 腐蚀重生（5 态细胞群落）。
- **实现**：`components/menu/wallpapers.c`（共享 400x300 1bpp PSRAM 缓冲 + 2x2 密度块 0..4 图案 + flush），大数组全部 EXT_RAM_BSS_ATTR（否则内部 RAM 溢出）；screensaver 按 `wallpaper_program` 分发（`screensaver_render_builtin`），程序 0 走原星空。
- **注意**：屏保循环 50ms/帧（~20fps）驱动动画；粒子钟用 esp_timer 分钟边界；程序状态（网格/粒子/板块）跨帧保持。

## 6.12 V1.0.65 触摸屏支持（用户需求）

- **接线**（TP-VDD=3.3V，TP-GND=GND，SDA/SCL 若面板不带需外接 4.7~10k 上拉到 3.3V）：
  - TP-SDA → GPIO15，TP-SCL → GPIO7（独立 I2C_NUM_1，与音频 ES8311 的 I2C_NUM_0(GPIO13/14) 完全隔离）。
  - TP-INT → GPIO17（输入+上拉，仅用于 GT911 复位时序，读点走轮询不依赖中断）。
  - TP-RESET → GPIO2（输出，GT911 上电复位时序；CST816/FT6236 只需拉高）。
- **芯片自动识别**：`components/touch_panel/`（新组件），启动时做 GT911 复位时序后按地址探测 GT911(0x5D/0x14)→CST816(0x15)→FT6236(0x38)，串口打印 `触摸芯片: GT911/CST816/FT6236`；未检测到则打印警告且 read 恒 false（零开销）。
- **手势 → 按键映射**（在 `components/input/input.c` 的 `touch_gesture_poll` 状态机里做，只作用于菜单导航，不参与游戏）：
  - 单击 = 确认(CONFIRM)；**长按状态栏(按下点 y<24, 600ms 几乎不动) = 返回(BACK)**；**屏幕底部中间(按下点 x∈[100,300] 且 y≥270)往上滑 ≥50px = 返回(BACK)**；上下左右滑动 = 方向键（按增量判向）。
  - 边沿触发语义与物理键/手柄一致；物理键/手柄优先，触摸仅在两者无动作时投递。
  - 阈值：`TOUCH_TAP_MAX_MS=300`、`TOUCH_SWIPE_MIN=40`、`TOUCH_LONG_MS=600`、`TOUCH_STILL_MAX=20`、`TOUCH_STATUS_H=24`、`TOUCH_BACK_EDGE_Y=270`、`TOUCH_BACK_EDGE_MINX=100/MAXX=300`、`TOUCH_BACK_SWIPE_DY=50`。
  - 注意：CST816 为**单点触控**，不支持多指手势（三指收缩等做不到）。
- **点哪进哪（hit-test）**：单击时把原始坐标映射到 400x300 屏幕坐标，再交给 `menu_handle_touch(state,x,y)` 做命中判定，命中后移动选中并复用 `menu_handle_action(CONFIRM)` 进入；未命中回退普通确认。已覆盖所有菜单 UI：
  - 主菜单：按 `render_main` 布局（icon_spacing=100, 中心 y=150, 选中100px/邻70/50/40）对图标做欧氏距离命中（+15px 容差），命中即选中并进入。
  - 列表弹窗（设置/手柄/存储/壁纸/番茄钟等）：按 `list_dialog_calc_geom` 的 content_y0/footer_y/line_h 定位行，点中即选中+确认（底部"返回"行同理）。
  - 二级菜单双栏（电子词典/GB/电子书书单）：左栏 2..96、右栏 104..396、行高 32、起点 y=30，点左栏=选中并切焦点到右栏，点右栏=选中并启动/打开对应项。
  - MP3 播放器：左菜单列表（宽 195、行高 26、起点 y=28）点歌即选中+播放。
  - 全屏居中列表页（render_sub：音量/游戏设置/系统信息等）：行高 32、起点 y=30（有提示时 60），整行可点。
  - 例外：电子书左/右竖屏旋转（book_rot=2/3，坐标经软件旋转变换）不做点选，回退普通确认/滑动导航。
  - 坐标映射：GT911 从寄存器 0x8048/49、0x804A/4B 读分辨率；CST816/FT6236 用 `TP_DEFAULT_RES_X/Y`（见 `touch_panel.h`）。本机实测这块 CST816 = **400x300**（与屏幕 1:1，无需缩放）。串口会打印 `分辨率=WxH`。
- **CST816 寄存器实测（重要）**：本机这块 CST816 变体（ID A7=00 A8=01 A9=01 AA=13）**比标准 CST816S 右移一位**：点数在 0x02（标准在 0x01），X 在 0x03/0x04，Y 在 0x05/0x06。驱动已按此修正。
- **主菜单跟手拖动（V1.0.66）**：主菜单图标排支持手指拖动跟手平移（`main_drag_offset` 叠加到图标 x），松手超过半格(50px)吸附到相邻图标并触发 cover-flow 动画，否则回弹；主菜单的 swipe 横向切换被拖动接管忽略，轻点(tap)仍走 hit-test 进入。实现：`input_get_touch_pos()` 暴露实时触摸屏幕坐标，main.c 主循环做拖动状态机，`render_main` 加偏移。
- **接线后验证**：开机串口应出现 `触摸芯片: ... (分辨率=WxH)`；主菜单点图标进入、拖动图标跟手、弹窗点行选中、游戏/书单点两栏、长按返回。若点击位置整体偏移，多半是分辨率不符，改 `touch_panel.h` 的 `TP_DEFAULT_RES_X/Y`。
- **回归注意**：触摸只接进 `input_get_action()`（60fps 菜单循环），游戏内仍走 `input_get_held_gb_joypad()`，不受触摸影响；未接触摸屏时芯片探测失败自动跳过，零开销。

## 6.13 V1.0.67 整体优化（本轮）

- **壁纸精简**：内置壁纸程序只保留星空（`wallpaper_type_dialog_on_select` 的 progs 数组只含 `WP_PROG_STARS`），游戏壁纸 / TF 动态图保留；config 加载把旧 `wallpaper_program` clamp 到 0（星空）。`wallpapers.c` 的 7 个程序代码暂留但 UI 不再暴露。
- **NTP 校时**：`wifi_manager.c` 在 `IP_EVENT_STA_GOT_IP` 后启动一次 `esp_netif_sntp`（`esp_netif_sntp.h`，server pool.ntp.org），异步同步后自动 settimeofday。需连外网 WiFi 才生效。
- **WiFi 虚拟键盘重排**：单大小写字母 + Shift 键切换大写（`wifi_kb_shift` 字段），字符 5 行网格 + 动作行(SSID/密码/删除/连接/取消 每键 2 格)，导航重写为"字符区网格 + 动作区横向"专用逻辑（menu_handle_action 里）。
- **网页手柄（WiFi AP）**：新增 `components/web_gamepad/`（移植 tigerxu255-lgtm/esp32-s3-rlcd-gb-emulator，但 ESP-IDF v5.5 的 esp_http_server 已移除 WebSocket，改用 HTTP POST `/joypad?state=N`）。AP=ESP32-BBK/12345678，浏览器打开 http://192.168.4.1 出虚拟手柄（方向键+A/B/Start/Select）。手柄设置弹窗新增「WiFi 手柄」开关项（gamepad_list_build idx3）。joypad 状态在 `input_get_held_gb_joypad()` 里合并（web_gamepad_is_running 时 `j &= web_gamepad_get_joypad_state()`）。
- **CMake**：顶层 EXCLUDE_COMPONENTS 移除 esp_http_server/esp_https_server（供 web_gamepad 用）；menu/input 组件 REQUIRES 加 web_gamepad + esp_http_client。
- **天气时钟壁纸**：`menu_system.c` 里 `weather_clock_render`（7 段数码管大号 HH:MM + 日期 + 温度）。天气用 Open-Meteo 免 key 接口（`weather_http_fetch`，esp_http_client 同步 GET，手动解析 temperature/weathercode），`weather_maybe_fetch` 在 WiFi 连接且数据过期(30min)/无效时用一次性任务异步拉取。壁纸程序列表 progs 加 `WP_PROG_WEATHER`(8)；`wallpapers.h` 加 WP_PROG_WEATHER=8、WP_PROG_COUNT=9；config 加载接受 STARS/WEATHER。默认坐标北京(39.9,116.4)。
- **已知/待办**：① 网页手柄目前只喂游戏 joypad，不参与菜单导航；② NTP/网页手柄/天气需真机联网验证；③ 天气坐标写死北京，后续可做成设置项。

## 7. 交接给下一个 AI 的注意事项

- 改动后**必须 build+flash+串口实测**，不要只编译；重点回归：三引擎启动/退出、蓝牙连接与断开重连、MP3 播放、拉伸显示。
- 诊断埋点（可临时用，交付前清理）：
  - `nes_emu`：`NES diag`（fps/emu_avg/copy_avg）
  - `gbc_emu`：`GBC diag`（fps/frame_avg/pcm.pos）、`GBC joypad`
  - `board_shim`：`LCD flush`（conv/spi/pack/scale 分段耗时）
  - `audio_player`：`PCM ring`（欠载次数/实际播放 Hz）
- 已知崩溃模式：core0 满载→看门狗（IDLE0）；蓝牙重连盲连→LoadProhibited；MP3 与游戏音频并发写 I2S。遇到重启先看这几条日志。
- `.engine_backup/` 是旧引擎备份，勿动；`build/`、`managed_components/`、`dist/` 不提交。

## 8. 本机环境

- 项目路径：`/Users/linit/AI项目/esp32-bbk-emu-lite`
- ESP-IDF：`~/esp/esp-idf-v55`，Python 环境 `~/.espressif/python_env/idf5.5_py3.13_env`
- 系统 ROM（BBK 8.BIN/E.BIN）在 `~/Desktop/BA4988词典模拟器v1.2.2`（build.sh 自动组装 system.bin）

---

## 7. V1.0.68 变更清单（2026-08 本次会话，接手必读）

### 7.1 触摸屏（CST816, I2C_NUM_1: SDA=GPIO15 SCL=GPIO7, INT=17 RST=2）
- 手势: 点击=CONFIRM, 上下左右滑动=方向, 状态栏长按3s=HOME, 底部中间上滑=BACK(物理坐标不随旋转), 其他区域长按800ms=LONG_PRESS(松手时判定, 拖动过不算长按)。
- 主菜单 cover-flow 跟手拖动(松手吸附, 环绕取最短路径不横穿); BOOT 短按=RIGHT 已放行。
- 游戏/电子书内容页: 整个列表跟手移动、选中项保持、松手固定位置(`select_game_drag_fix` 渲染跳过选中钳制)。
- 列表弹窗(番茄钟时间列表等)同样支持整列跟手拖动(`list_dialog_drag_*`); 自定义渲染弹窗(时间设置)除外。
- 弹窗/全屏覆盖期间 `menu_modal_active()` 禁止背景拖动。

### 7.2 软关机键 (GPIO1)
- 点按=锁屏休眠(`menu_screensaver_activate`); 0.5s=弹"返回菜单"提示, 松手返回主菜单; 2s="正在关机"深度睡眠; 关机状态按0.5s开机(EXT1唤醒)。
- 锁屏后任意按键立即退出。

### 7.3 阅读器
- 菜单: 无标题, 32px 大字居中, 选项=添加书签/书签列表/返回书库/继续阅读(临时切 font size 3, 后台分页字号已快照不受影响)。
- BOOT长按(BACK)在阅读页=退出回书库二级菜单; 触摸翻页(上=上一页,下=下一页,中=设置)。
- 按键翻页跟随旋转: 左旋 KEY=下一页/BOOT=上一页; 右旋对调。

### 7.4 隐藏设置（系统信息 → "BY: LinIT" 连点5次, 物理键确认也计数）
- 音频方案 3 态: 0=解码输出(默认, ES8311) / 1=方波直驱(GPIO48 PWM, tone_player) / 2=禁用音频(删除音量弹窗里的禁用音频项, 主菜单隐藏MP3)。
- 禁用触摸屏: 卸载触摸 I2C 驱动释放内存(可物理键回来重开)。
- 方波方案下可"试听音效"(6种电子音循环切换)。

### 7.5 tone_player 组件 (`components/tone_player/`)
- GPIO48 LEDC PWM → AXS2005B 功放 → 喇叭。`tone_beep(freq,ms)` / `tone_play_effect()` / `tone_play_theme()`(开机上行音/小星星/生日快乐) / `tone_play_melody()`。
- 开机旋律/关机下行音/物理按键音(仅方波方案下生效)。

### 7.6 修复
- **TF 格式化**: `sd_scan.c` `sdmmc_card_init(&host, &card)` 之前传 NULL 导致从未真正格式化, 已修(需提供 card 结构体)。
- **天气时钟**: HTTPS 无证书校验导致抓取失败, 已启用 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` + `esp_crt_bundle_attach`, 任务栈 32KB。
- **BBK 全屏 EPX**: 之前只在点对点模式生效; 现全屏模式先 EPX 2x 到 PSRAM 缓冲(320x192)再 1.25x 缩放。
- 主菜单标签下移10px; 状态栏日期改为 YYYY-MM-DD。

### 7.7 按键映射
- 10 键: 上/下/左/右/确认/返回/返回菜单/多功能键/Start/Select (FUNC_MAX=10)。
- Start/Select 接入 GB 手柄 (input.c `input_get_held_gb_joypad`); 已删除所有补充按键映射。

### 7.8 图标
- 新增「独立游戏」入口图标(index 15, 游戏手柄); GBC/NES/Arduboy 图标按 GB 风格重画(`components/menu/icons_data.inc` + `generate_all_icons.py`)。

### 7.9 进行中
- **独立游戏引擎移植中**: TamaFi(240x240彩屏宠物, cifertech/TamaFi) + ESP32Pet(拓麻歌子, dishishshawn/ESP32Pet)。
  - 方案: 新建 pet_engine 组件, TFT_eSPI/Adafruit GFX 模拟层渲染到 PSRAM RGB565 缓冲→1bit 缩放 ST7305; 主菜单「独立游戏」→ 游戏列表。
  - 源码已下载到 `/tmp/petsrc/` (tamafi: TamaFi.ino/ui.cpp/StoneGolem.h/background.h/effect.h/egg_hatch.h; esp32pet: src/*)。

## 8. V1.0.68 追加修复（2026-08-16，最新，接手必读）

### 8.1 GBC 颜色再次修复（纠正 6.10 的错误结论）
- **6.10 的结论是错的**：CGB 调色板 15 位色是 **bit0-4=红、5-9=绿、10-14=蓝**（红在低位，与 DMG `pal_write_dmg` 打包顺序一致），并非"BGR 序"。
- 上一版"修复"把 `updatepalette` 的 CGB 分支改成 蓝/绿/红 解析 → 红蓝对调 → 灰度下红色（高亮度权重）变近黑、蓝色变中灰 → GBC 颜色错乱。
- **本次修复**：`components/gbc_emu/third_party/gnuboy/src/lcd.c` `updatepalette` CGB 分支改回 `r=c&0x1f; g=(c>>5)&0x1f; b=(c>>10)&0x1f`，与 DMG 分支一致。

### 8.2 手柄/物理键 UP/DOWN 控制二级菜单内容页（main.c）
- 旧代码在电子词典/GB/电子书(横屏)内容页**无条件吞掉** UP/DOWN（注释称"拖动期间忽略"，实际不管是否拖动都丢弃）→ 手柄方向键上下完全失效。
- 现改为仅在「动作来自触摸滑动 且 本帧处于拖动中(含松手帧)」时抑制；手柄/物理键 UP/DOWN 始终放行（`content_drag_was_active` + `input_touch_last_action()`）。

### 8.3 音量曲线（menu_system.c `volume_step_to_percent`）
- 仅音频方案=0（解码输出/当前 ES8311 硬件）时启用：新 1 档 = 旧 5 档(50%)，2-10 档在 50%-100% 间平均分布（每档 +50/9%）。
- 切换其他解码/方案自动回退旧线性 10%/档。

### 8.4 状态栏蓝牙图标
- 从左侧（日期后）移到右侧：**电池 → 蓝牙图标 → 喇叭+音量 → WiFi**（`menu_draw_status_bar`）。未连接不占位。

### 8.5 NES 黑白极性核查结论
- 已逐环节验证 NES 灰度链（nofrendo 调色板→s_shade_lut→2bit 打包→board_shim 抖动→FB）方向与 GB/GBC 完全一致（亮→白、暗→黑），**代码层面无反色 bug**。
- 若用户仍反馈 NES 反色，需提供具体游戏名针对性分析，或按用户确认翻转 `s_shade_lut` 方向。

### 8.6 备份（2026-08-16）
- 本目录即最新备份：`firmware/`（含新 16MB 合并镜像 + 4 文件）+ `source/`（精简源码）。
- 压缩包：`esp32-bbk-emu-handoff-20260816-*.zip`（项目根目录 + ~/Desktop）。

## 9. V1.1 变更清单（2026-08-16 晚间，最新固件，接手必读）

### 9.1 抗锯齿恢复旧版算法（修复文字缺线）
- V1.0.68 手写的"2x EPX 中间缓冲 + 1.25x + 抠角"算法会吃掉文字笔画（缺线）。
- 已整体恢复桌面旧版 `~/Desktop/esp32-bbk-emu-lite`（V1.0.60）的渲染实现：
  - **全屏模式**：覆盖率抗锯齿（`fb_classify_aa`/`fb_fill_block_aa`，V1.0.58 "保粗"方案：斜线/端点实心不删像素，仅拐角圆滑）。
  - **点对点模式**：查表 EPX（Scale2x/AdvMAME2x，`s2x_lut_E0/E1` + `scale2x_1bit`）。
- 删除了 `s_epx_buf`（320x192 PSRAM 中间缓冲）及手写拐角逻辑。

### 9.2 退出游戏 5~10 秒卡顿修复（串口日志实锤）
- 现象：请求退出后按确认无效，干等 10s 超时才退出。
- 根因：V1.0.68 进 BBK 游戏时 `input_set_gamepad_nav_enabled(false)` 禁用了手柄导航键，
  退出确认弹窗只读 `input_get_action()` → BT 手柄 F_CONFIRM 永远不产生确认动作。
- 修复：`gam4980_exit_confirm_dialog` 直接轮询 F_CONFIRM 上升沿（与 GB 弹窗一致），
  确认键立即退出。日志实测：233702 请求退出 → 234182 按确认（旧固件无反应）→ 243710 超时退出。

### 9.3 存档回退为同步（保留旧版行为）
- 曾改为后台异步存档（bbk_save_task），但换回旧版 gam4980_emu.c 后恢复同步写；
  串口实测 32KB 存档写 SD 仅 85ms，同步无感知延迟，无需后台。

### 9.4 删除独立游戏（宠物 ESP32Pet/TamaFi）
- 用户反馈问题多，已删除：主菜单「独立游戏」项、MENU_PAGE_PET_GAME、整个 `components/pet_engine/` 组件。
- 固件体积回到 5.5MB。

### 9.5 版本号
- 系统信息页「固件: 1.0」→「固件: 1.1」（`menu_system.c` sysinfo_build）。

### 9.6 备份（2026-08-16 晚间）
- 16MB 合并镜像 + 精简源码 zip 放桌面（esp32-bbk-<日期>.zip）。

## 10. 遗留计划（按用户确认的顺序）
1. 壁纸菜单改造：删除「TF动态图」，二级菜单改为 内置壁纸/游戏壁纸/休眠时间/测试壁纸/返回。
2. RLCD-4.2 时钟界面：参考 `/Users/linit/AI项目/esp32-s3-rlcd-4.2-ref/`（main/ui/ 的
   ui_weather_board_text.cpp、ui_flip_clock_runtime.cpp），只取时间+天气+简单布局，
   替换天气时钟壁纸、保留"天气时钟"名字；不要温湿度采样和小智AI页面。
3. NC2000 文曲星模拟器：`/tmp/NC2000`（5MHz 6502 SoC + 512KB NOR + 32MB NAND + 160x80 LCD），
   评估内核能否塞进固件（NAND 模拟/ROM 来源是主要问题），能则集成。

## 11. V1.1 追加: 删除 SD 卡系统 ROM 部署 (2026-08-16)
- 删除 components/system_rom/ 组件及开机把 8.BIN/E.BIN 写到 /sdcard/system/gam4980/ 的逻辑
  (app_board.c 调用 + main/app_board CMakeLists REQUIRES)。
- emulator 直接读 flash system 分区 (libretro.c), 与 SD 无关, 功能不受影响。
