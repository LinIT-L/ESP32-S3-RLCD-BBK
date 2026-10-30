# 诊断探针管理（probes）

> 本文是 LinTOS 临时诊断代码的**唯一登记处**。
> 目的：日常运行日志保持精简；出问题时按模块开启探针；问题解决后按本文准确定位并清理。
>
> 最后更新：2026-10-29

---

## 一、先分清两类日志

| 类型 | 例子 | 处理方式 |
|---|---|---|
| **正式业务日志** | 初始化失败、睡眠失败、资源分配失败、状态机异常、超时重试、安全拦截 | **永远保留**，不受任何探针开关影响 |
| **临时诊断探针** | 每秒打印 FPS/帧耗时、环形缓冲水位、寄存器写入统计、一次性喂送快照 | 纳入本文管理，默认关闭 |

判断标准（满足任一即为探针）：

1. 周期性打印（每秒 / 每帧 / 每 N 轮），而非事件驱动；
2. 输出的是运行指标（耗时、速率、计数、水位、寄存器值），不是错误；
3. 注释里出现「排查 / 诊断 / 定位 / 统计 / 临时」；
4. 只为验证某一次实验而存在，问题定位后不再需要。

`ESP_LOGE` / `ESP_LOGW` 属于故障报告，**默认一律保留**，不在清理范围内。

---

## 二、开关机制

**全部为编译期开关**（`#define` 改值后重新构建生效，**不支持运行时切换**）。
关闭时用预处理器整体剔除，不产生字符串常量、不做格式化、参数不求值。

命名规范：`LIN_DIAG_<模块>`。

| 模块 | 宏名 | 定义位置 |
|---|---|---|
| 渲染 / 帧率 | `LIN_DIAG_RENDER` | `components/engines/gam4980/gam4980_emu.c` |
| 音频 | `LIN_DIAG_AUDIO` | `components/services/audio_player/audio_player.c` |
| 音频（步步高引擎） | `BBK_AUDIO_PROBE` | `gam4980_emu.c` 与 `libretro.c` 各一份，**必须同步改** |
| 引擎音频通路总开关 | `BBK_AUDIO_ENABLE` | `components/engines/gam4980/libretro.c` |

> 为什么不是统一头文件：`components/engines/gam4980` 与 `components/services/audio_player` 都不依赖 `os` 组件，
> 根 `CMakeLists.txt` 也没有全局 `include_directories`。为一个开关去改构建依赖关系，
> 风险大于收益。因此采用**局部定义 + 统一命名 + 集中登记**。

### 开启示例

```c
/* 排查游戏卡顿：只开渲染探针，不引入其他模块日志 */
#define LIN_DIAG_RENDER 1
```

```bash
idf.py -B build build      # 重新构建后才生效
```

### 关闭示例

```c
#define LIN_DIAG_RENDER 0
```

**验证关闭是否彻底**：对比构建后的 `build/LinTOS.bin` 大小，关闭后应减小；
若大小不变，说明该探针未被 `#if` 包住（漏改）。

---

## 三、探针登记表

| 编号 | 模块 | 代码位置 | 用途 | 开关 | 默认 | 影响评估 | 状态 |
|---|---|---|---|---|---|---|---|
| P-RENDER-01 | 渲染 | `gam4980_emu.c` `video_cb()` | 每帧渲染耗时（`Render time: N ms`） | `LIN_DIAG_RENDER` | **关** | 每秒 1 行串口 | 长期保留 |
| P-RENDER-02 | 渲染 | `gam4980_emu.c` `gam4980_emu_run()` 帧循环 | 游戏帧率与单帧耗时（`Game FPS=.., retro_run=..`） | `LIN_DIAG_RENDER` | **关** | 每秒 1 行串口 | 长期保留 |
| P-AUDIO-01 | 音频 | `audio_player.c` `audio_out_task()` | 环形缓冲水位/欠载/实际播放速率 | `LIN_DIAG_AUDIO` | **关** | 每 400 轮 1 行 | 长期保留 |
| P-AUDIO-02 | 音频 | `gam4980_emu.c` `bbk_audio_frame_finalize()` | 首帧喂送快照（game/want/wrote/muted） | `BBK_AUDIO_PROBE` | **关** | 一次性 1 行 | 待清理（见下） |
| P-AUDIO-03 | 音频 | `libretro.c` `bbk_audio_flush_frame()` | `stcon` / 定时器溢出 / 音频寄存器写入统计 | `BBK_AUDIO_PROBE` | **关** | 每秒 2 行 | 待清理（见下） |
| P-AUDIO-04 | 音频 | `libretro.c` 整个方波合成段 | 步步高引擎音频输出（**不实现**） | `BBK_AUDIO_ENABLE` | **关** | 关闭时不编译、不占 CPU | 已停用 |

### P-AUDIO-04 停用依据（2026-10-29 定案）

不是"关闭功能"，而是查证后确认**该功能从未被实现过**：

| 证据 | 内容 |
|---|---|
| 上游 `gam4980` | `audio_cb` 只赋值、**从不调用**；`retro_set_audio_sample_batch` 是空函数 |
| BBKEmu（Rust） | 写了 `play_tone()`，但 `emulator.rs` 中**从未调用** |
| 本机实测 | `stcon=0x01`（只开 ST1 按键扫描定时器）；ST1 每帧溢出 <2 次 ⇒ 只能凑出 60Hz 方波（听感为"爆炸声"）；`PB(0x21b)` 全程仅写 1 次；`AUDCON(0x23f)` 全程仅写 1 次（0xC0，一次性使能） |

结论：**没有可用音频源**，强行合成只会产生噪音。
将来若要做，须先反汇编 `E.BIN` 的音频服务例程取得调用约定，再走 HLE 或寄存器级合成。

---

## 四、待确认 / 暂不清理

| 项 | 原因 | 清理条件 |
|---|---|---|
| P-AUDIO-02 / 03（`BBK_AUDIO_PROBE`） | 音频问题**未解决**，只是暂缓；这两处是唯一能证明"卡在哪一环"的观测点 | 决定彻底放弃引擎音频时，连同 `BBK_AUDIO_ENABLE` 一起删除 |
| `gam4980_emu.c` 中 `s_feed_diag_done` 静态标志 | 随 P-AUDIO-02 一起受 `BBK_AUDIO_PROBE` 控制 | 同上 |

---

## 五、新增探针规范

1. 宏名必须为 `LIN_DIAG_<模块>`，默认值 **0**；
2. 一律用 `#if` 包住整段（含变量声明），不要只包 `ESP_LOG` 那一行——
   否则关闭后仍会留下未使用变量告警；
3. 高频探针必须限频（≥1000ms 或每 N 轮），或改为"状态变化才打印"；
4. 日志正文需能自证模块与含义，不要只打数字；
5. 新增后在**本文第三节表格登记**，写清用途、影响、删除条件；
6. 不新增运行时开关（本工程无对应基础设施，避免"以为能热切换"的误解）。

---

## 六、验证记录

| 日期 | 操作 | 结果 |
|---|---|---|
| 2026-10-29 | 关闭 `LIN_DIAG_RENDER` / `LIN_DIAG_AUDIO` / `BBK_AUDIO_PROBE` / `BBK_AUDIO_ENABLE` 后构建 | ✅ 0 error / 0 warning，`LinTOS.bin` 4,044,336 → **4,038,992** 字节（−5,344），证明代码被编译期剔除 |
| 2026-10-29 | 临时开启 `BBK_AUDIO_PROBE` 抓串口 | ✅ 探针有效：输出 `[diag] 静音: stcon=0x01 cb=1 PB=1/s`、`[areg] AUDCON=1/0xC0` |

> 未验证项：关闭探针后的实际日志量与功耗变化**未做设备实测**（需烧录后长时间观察才能给数据），
> 本文不提供推测数值。
