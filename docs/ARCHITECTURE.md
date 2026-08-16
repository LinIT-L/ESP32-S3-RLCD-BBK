# esp32-bbk-emu-lite 架构文档与模块化路线图

> 目的: 说清当前模块划分、耦合问题、目标架构与分步解耦路线,
> 为后续"模块化 + 性能/内存 + 自动化测试"提供统一依据。
>
> 参考项目:
> - [esp-cpp/esp-box-emu](https://github.com/esp-cpp/esp-box-emu)
>   （ESP32-S3 掌机: Cart 引擎接口 / 事件总线 / 共享内存池 / 堆诊断）
> - [ducalex/retro-go](https://github.com/ducalex/retro-go)
>   （retro-core 组件化: launcher / 引擎 / 平台适配分离）

## 1. 现状统计

一阶代码（非第三方内核）:

| 模块 | 文件 | 行数 | 职责 |
| --- | --- | --- | --- |
| menu | menu_system.c | ~10.0K | 菜单/弹窗/状态栏/屏保/番茄钟/按键映射 全部揉在一起 |
| menu | wallpapers.c | ~1.1K | 内置壁纸程序（独立良好） |
| menu | sd_scan.c / bt_manager.c / favorites.c | ~2.6K | SD 监控 / 蓝牙 / 收藏 |
| audio_player | audio_player.c | ~1.0K | I2S+ES8311 输出 / MP3 / 游戏音频直喂 |
| board_shim | board_shim.c | ~0.4K | 引擎显示/音频 HAL + 异步刷新任务 |
| engines | engine_manager + gb/gbc/nes/arduboy/gam4980 适配 | — | 引擎生命周期 |
| book_reader | book_reader.c | ~1.8K | 电子书（libunibreak/miniz 为第三方） |

第三方内核（约 65 万行）: nofrendo / peanut-gb / gnuboy / simavr / gam4980 /
libunibreak / miniz。**这些不做架构改动**，只通过适配层接入。

## 2. 当前耦合问题

1. `menu_system.c` 是上帝模块: 同时持有 菜单状态、输入处理、状态栏绘制、
   屏保/壁纸状态机、游戏壁纸启动、番茄钟、按键映射、收藏、配置持久化。
   任何改动都可能波及无关功能（历史多起回归都源于此）。
2. `main.c` 直接初始化所有硬件（LCD/SD/NVS/蓝牙/音频/电池），顺序敏感，
   无法在本地模拟环境复用。
3. 引擎通过 `engine_manager` 已初步抽象（对应 esp-box-emu 的 Cart），
   但 `board_shim` 仍是全局函数式 HAL，无显式接口头文件。
4. 缺少统一事件通道: 蓝牙/电池/音量等状态由各模块轮询互查（esp-box-emu
   用 event_manager 发布/订阅解耦）。
5. 无自动化测试: 回归只能靠真机手动验证。

## 3. 目标架构（分层 + 接口化）

```text
main (组合根: 只做 init 顺序 + 主循环)
  |
  +-- app_board      硬件初始化封装 (LCD/SD/输入/NVS/BT/音频/电池)   [本次落地]
  +-- ui/menu        菜单/弹窗/状态栏（拆分路线图目标）              [待拆]
  +-- screensaver    壁纸状态机（从 menu 抽出）                     [待拆]
  +-- hal/            st7305 / board_shim / audio_player
  +-- engines/        gb/gbc/nes/arduboy/gam4980 + engine_manager(Cart 接口)
  +-- services/       bt_manager / sd_scan / favorites / wifi / book_reader
  +-- self_test      机载自动化测试（本次落地）
```

关键接口（参照 esp-box-emu）:
- **引擎接口** = 现有 `game_run_engine_ops_t` + `engine_manager`:
  `load(engine, path) -> start -> run -> stop -> unload`，统一生命周期。
- **显示 HAL** = `board_shim`（视频帧源 + 异步刷新任务），后续收敛成
  `board_rlcd_t` 接口头。
- **事件总线**（路线图）: 电池/音量/蓝牙状态用发布-订阅替换互查轮询。
- **内存策略**（沿用现状并文档化）:
  - 帧缓冲/大数组 → PSRAM（`EXT_RAM_BSS_ATTR` / `MALLOC_CAP_SPIRAM`）
  - DMA 描述符/关键栈 → 内部 RAM
  - 引擎显存不足时自动回退 PSRAM（20260812 已修 NES/GBC 视频缓冲）

## 4. 分步解耦路线图

| 步骤 | 内容 | 风险 | 状态 |
| --- | --- | --- | --- |
| R1 | 抽出 `app_board`（硬件初始化） | 低 | 本次 |
| R2 | 壁纸/屏保状态机 → `screensaver` 组件（API 回调注入） | 中 | 待做 |
| R3 | 状态栏/通用绘制 → `ui_common` | 中 | 待做 |
| R4 | 事件总线（电池/音量/蓝牙） | 中 | 待做 |
| R5 | 按键映射模块独立 | 中 | 待做 |
| R6 | 配置持久化独立组件 | 低 | 待做 |
| R7 | 本地模拟：host 编译 + stub RTOS/驱动，渲染 PNG | 高 | 待做 |

每步都要求: 先备份 → 自测通过 → 真机回归 → 才允许进入下一步。

## 5. 性能与内存基线（优化方向）

已做（20260812）:
- I2S DMA 6×576 → 8×576；PCM 预填充 4096→8192 帧（防欠载）
- NES/GBC 视频缓冲内部 RAM 不足自动回退 PSRAM（修"100% 加载卡住"）
- NES 共享池双重释放修复
- g_menu(菜单状态, 33.7KB) 移到 PSRAM —— 内部 RAM 腾出 33.7KB
  （开机基线: 内部空闲 25KB→159KB；运行时蓝牙+音频后约 55-60KB）
- gam4980 的 sys_ram(32KB, BBK 引擎热数据) 保留内部 RAM 保性能

持续优化方向:
- 壁纸/动态图 PSRAM 缓冲退出即释放（本次）
- 堆诊断: 启动基线 / 引擎卸载后对比（已有 `[MEM]` 日志，本次补壁纸释放）
- 减少 `menu_system.c` 内部静态缓冲（PSRAM BSS 已迁移大部分）
- 60FPS 主循环已用 `vTaskDelayUntil` 精确节流，避免忙等

## 6. 自动化测试（本次落地: 机载自测）

`components/self_test`:
- S1 堆基线（内部/PSRAM 空闲与最大块）
- S2 主菜单连续渲染 N 帧不异常
- S3 状态栏渲染（时间居中/左侧图标/右侧电池布局回归）
- S4 壁纸状态机: 立即进入 → 渲染数帧 → 输入退出（回归 20260812 修复）
- S5 NES 引擎 load/run/stop + 内存回收（SD 有内置玛丽时执行）
- 输出 `SELF-TEST [PASS|FAIL]` 逐条 + 汇总，串口可读，后续可接 CI。

本地模拟（路线图 R7）: 用 stub 替换 st7305/FreeRTOS/外设，host 编译菜单逻辑，
输出 400×300 PNG + 脚本化按键事件，把 S1-S5 搬到电脑上跑。
