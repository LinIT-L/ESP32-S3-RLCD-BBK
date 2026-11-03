# LinTOS 2.0 发布后全项目代码质量审计报告

**审计日期**：2026-10-31
**审计性质**：只读（未修改任何业务源码 / sdkconfig / 引脚，未提交、未烧录）
**审计人**：工友

> 证据分级沿用项目规则：`【源码】`读源码确认 · `【文档】`来自现有文档 · `【主机测试】`主机脚本验证 ·
> `【真机】`设备实测 · `【推断】`有证据但未验证 · `【待核实】`证据不足

---

## 1 项目基线

| 项 | 值 |
|---|---|
| 仓库 | `/Users/linit/AI/LinTOS1.0`，分支 `main` |
| HEAD | `c55ad92`（tag **v2.0**，= `origin/main`） |
| 上一提交 | `59fc7c5 release: LinTOS 2.0` |
| 工作区 | 未提交 **30** 项（暂存 0、未跟踪 7）**【源码 git status】** |
| 版本 | `LINTOS_VERSION_PUBLIC "2.0"` / `LINTOS_VERSION_STR "2.0.0"` / `VERSION_DATE 2026-10-31` |
| 固件内版本串 | `2.0.0` 命中 【源码 `grep build/LinTOS.bin`】 |
| IDF | ESP-IDF **6.0.3**（`/Users/linit/esp/esp-idf-v60`） |
| 芯片 | `CONFIG_IDF_TARGET="esp32s3"` |
| Flash / PSRAM | 16 MB / **OCT PSRAM 80 MHz**（`SPIRAM_MODE_OCT=y`、`SPIRAM_SPEED_80M=y`） |
| 分区 | factory 5 MB / appdata 6.625 MB / apps 4 MB |
| 独立全量构建 | ✅ `exit=0`，`LinTOS.bin = 0x3EC790`（4,113,296 B）＝ **factory 的 78.5%** |
| 编译告警 | **136 条**（全量构建日志统计，均为既有） |

**未提交清单里包含实质改动**（不是纯文档）：
`am_store.c`(+127/−…)、`input.c`(+158/−…)、`os_core.c`(+6)、`board_shim.c`、`font_book.*`、
`lavax/core/*`、`svc_quickbar.c`、`page_book.c`、`main.c` 等。
⇒ **v2.0 tag 与当前工作区不是同一状态**，谁也不是"另一个"的副本。

---

## 2 模块覆盖清单

| 模块 | 本轮覆盖 | 方式 |
|---|---|---|
| 构建/配置/分区/版本 | ✅ 完整 | 独立目录全量构建 + `sdkconfig.json` 比对 |
| 内存（内部/PSRAM/最大块） | ✅ 完整 | 真机 `mem` / `blk` |
| 任务与栈 | ✅ 完整 | 真机 `tasks` / `stack` + 源码栈定义扫描 |
| 任务创建返回值 | ✅ 抽样 | 52 个调用点脚本统计 + 抽读 4 处 |
| 显示 / 灰阶 | ⚠ 部分 | 源码 + 上次真机切换验证，本轮未重测 |
| 应用中心 / 下载替换 | ✅ 部分 | 源码（上一轮自测 36 用例仍有效） |
| 音频 | ❌ 未走查 | 仅核对栈定义 |
| 蓝牙 / Wi-Fi 生命周期 | ❌ 未走查 | 仅核对栈定义 + 真机任务表 |
| 睡眠 / 唤醒 | ❌ 未走查 | — |
| 引擎（8 个）逐一 | ❌ 未走查 | — |
| 文件系统 / TF 卡异常 | ❌ 未走查 | — |
| 许可证 / 发布合规 | ✅ 完整 | `git ls-tree v2.0` + LICENSE 核对 |

**明确未覆盖**：20 次进出循环、Wi-Fi+BT 同时、下载+播放、游戏+快捷栏等交叉场景、
长时间播放/录音、异常拔卡、QEMU。**本轮不伪称覆盖过**。

---

## 3 问题清单

### P0（无法启动 / 数据损坏 / 安全）
**无。** 未发现此类问题。

### P1（核心功能故障 / 崩溃 / 死锁）
**无确证。** 不以推断充数；下面 P2-3 若在真机复现栈溢出，会升级为 P1。

### P2

#### P2-1 快捷栏 Wi-Fi 与"系统与连接"不是同一套逻辑（**已确认**）
- **位置**：`main/main.c:151`、`components/system/os/services/svc_quickbar.c:521-528`、
  `components/apps/pages/page_settings.c:1694-1696`
- **现象**：设置页开关 = 三个动作；快捷栏只做最后一个。
  | 动作 | 设置页 | 快捷栏 |
  |---|---|---|
  | 引用计数 `os_hw_request/release(OS_HW_WIFI)` | ✅ | ❌ |
  | 持久化用户意愿 `wifi_manager_set_want_on()` | ✅ | ❌ |
  | 射频 `wifi_manager_enable/disable()` | ✅ | ✅ |
- **影响**：快捷栏关掉 Wi-Fi 后 `want_on` 仍是 true ⇒ **下次开机 autostart 又把 Wi-Fi 开回来**；
  且有页面持有 HW_WIFI 时，快捷栏的"开"没有计数，页面退出时的 release 可能把它关掉。
  这正是"不强制、和设置页不一样"的根因。
- **复现**：进系统与连接关 Wi-Fi → 重启 → 仍关闭；改用快捷栏关 Wi-Fi → 重启 → **又开了**。
- **修复**：让快捷栏走同一组调用（3 行），或把三条动作收敛成 `wifi_manager_set_user_enabled(bool)` 一个函数，两处都调它。
- **风险**：低。需确认 `os_hw_request` 重复调用是否幂等（`os_hw.c` 用 `s_active[]` 位图，需读一遍再定）。
- **验证**：改后各关一次 → 重启 → 看 `wifi_manager_is_enabled()` 与 `want_on` 是否都为 false。

#### P2-2 v2.0 tag 缺 LavaX 的 LICENSE 文件（**已确认**）
- **证据**：`git ls-tree -r v2.0` 里有 **25 个** `components/engines/lavax/` 源文件；
  但 `components/engines/lavax/LICENSE` 是**未跟踪文件**，**不在 tag 里**。
- **内容**：该 LICENSE 为 `MIT License / Copyright (c) 2015 李杰` —— MIT 明确要求**保留版权与许可声明**。
- **同时**：`docs/专题/第三方组件与许可证.md` 里搜不到 lavax / 文曲星相关条目（该文件本身也还是未提交状态）。
- **影响**：公开发布物缺少第三方许可声明；不是功能 bug，但属发布合规缺口。
- **修复**：`git add components/engines/lavax/LICENSE` + 在许可证清单里补一条；下次 tag 带上。
- **风险**：无（纯新增文件）。

#### P2-3 四处「栈大小」注释与实际值差 4 倍（**已确认**）
`StackType_t = uint8_t`（`portmacro.h:78`）⇒ `xTaskCreate(..., N, ...)` 的 N **就是字节数**，
但以下注释都按"字 → ×4"写，实际分配只有注释的 1/4：

| 文件:行 | 宏 | 注释声称 | 实际 |
|---|---|---|---|
| `services/bt_ctl/bt_manager.c:1277` | `INIT_TASK_STACK_WORDS 4096` | 16KB | **4 KB** |
| `ui/wallpapers/wallpapers.c:249` | `SS_SUPER_STACK 4096` | words(→16KB)，并称"实测仅用 7.6KB" | **4 KB** |
| `services/audio_player/audio_player.c:196` | `PCM_OUT_STACK_WORDS 6144` | 24576 字节 | **6 KB** |
| `apps/pages/page_netdect.c:105` | `ND_TASK_STACK 8192` | 32KB | **8 KB** |
- **真机对照**【真机】：`sse` 栈 4096 B，高水位**剩 2092 B**（用约 2 KB）。
  若真如注释所说"用 7.6KB"，这个任务早就该 CANARY 溢出重启 —— 说明那句历史结论与当前配置**已经脱节**。
- **影响**：不是当前崩溃点，但**余量判断全错**。同类问题已经出过一次真机事故：
  `am_store.c` 注释记录 `pkgscan` 4096 溢出 → 提到 8192 才修好。后人照注释调栈，会再次踩坑。
- **修复**：把 4 处注释改成真实字节数；`SS_SUPER_STACK` 那条"实测 7.6KB"的结论作废重测。
- **风险**：纯注释，零功能风险。**不要**据此改实际数值。

### P3

| 编号 | 问题 | 证据 | 影响 |
|---|---|---|---|
| P3-1 | 136 条编译告警 | `build_audit` 全量日志 | 主要是 `esp_sleep_get_wakeup_cause` 弃用、未使用函数/变量（`svc_statusbar`、`svc_quickbar`、`audio_player`、`vid_tune`）。不阻塞，但会淹没新告警 |
| P3-2 | 任务创建失败静默降级 | `xTaskCreate` 52 处，脚本判 12 处未见返回值检查；抽查 4 处，**2 处确为未检查**：`wifi_manager.c:341`（wifi_diag 4096）、`:554`（wifi_sup 3072）。`bt_manager.c:1633` 有检查（脚本误报） | 内存紧张时诊断任务/自愈任务静默不启动，无重试 |
| P3-3 | 设备上的固件版本无法确认 | 无 `ver` 类命令；昨晚刷的是当时的测试构建，今天 08:59 又出了 2.0 的 `build/` | "发布固件 = 源码"这条**目前无法证实**，需重烧 build/ 产物再对比 |
| P3-4 | `build/` 产物可能已落后工作区 | `build/LinTOS.bin` 08:59；工作区在之后仍有改动；我的独立构建是 `0x3EC790`，`build/` 是 4,114,304 B | 用 `build/` 打包发版会漏掉最新改动 |

### 4 内存与栈分析（真机实测）

**当前状态**（Wi-Fi 已连接、蓝牙关闭、未开应用）：

```
internal total=318723 free=106579 used=212144 largest=32768
psram    total=7558628 free=7123608 used=435020 largest=7077888
```

- Wi-Fi 栈（`wifi` / `wifi_sup` / `tcpip`）占了内部 RAM 大头 ⇒ free 从 184 KB 降到 106 KB，**属预期，不是泄漏**。
- ⚠ **最大连续块只剩 32,768 B**：Wi-Fi 开着时，任何单次 >32 KB 的内部 RAM 申请必败。
  当前代码里没有这种申请（fb 15 KB、netc 分块 8 KB），但这是容量硬边界，新增功能要守住。
- PSRAM 7.1 MB 空闲、最大块 6.75 MB ⇒ 大缓冲应继续往 PSRAM 放。

**任务栈高水位**（`tasks` 原文，单位字节）：

| 任务 | 剩余 | 任务 | 剩余 |
|---|---:|---|---:|
| remcon | 12,344 | wifi_sup | 2,492 |
| esp_timer | 3,536 | wifi | 2,468 |
| touch_wd | 3,432 | sse | 2,092 |
| gps_guard | 1,560 | main | 1,536 |
| sys_evt | 1,232 | **tcpip** | **1,188** |
| Tmr Svc | 840 | ipc0 / ipc1 | 584 / 512 |

**最紧的是 `tcpip`（剩 1,188 B）**——Wi-Fi 协议栈任务，属第三方，不建议动；记录为观察项。

### 5 模块交互风险（源码级，未实测）

| 组合 | 风险点 | 证据 |
|---|---|---|
| 快捷栏 × Wi-Fi 页面 | 快捷栏开 Wi-Fi 不计引用；页面退出 `os_hw_lease_release` 会关掉它 | P2-1 |
| 下载 × 存储扫描 | 上一轮已加 `netc_busy()` 互斥，扫描期间不碰 `.bak` | `am_store.c` |
| 屏保 × 刷屏 | 门禁 `st7305_set_flush_blocked` + `flush_override` 单一通路，已在关机路径用 | `main.c:191` |
| 游戏 × 快捷栏 | 快捷栏触摸命中区与虚拟手柄是否重叠，**本轮未走查** | 待核实 |

### 6 文档与配置一致性

| 检查 | 结果 |
|---|---|
| 固件版本串 vs `lintos_version.h` | ✅ 一致（2.0.0） |
| `build/config/sdkconfig.json` vs 工程 `sdkconfig` | ✅ 一致（esp32s3 / 16MB / OCT / 80M / BT=y / WiFi=y） |
| 分区表 vs bin 大小 | ✅ 78.5%，余 21.5% |
| 第三方许可证清单 | ❌ 缺 lavax（P2-2） |
| 栈容量说明 | ❌ 4 处注释错误（P2-3） |
| 测试组件是否误入正式固件 | ✅ 未误入：`components/test/lavax_bench` 只在 `-DLINTOS_LAVAX_BENCH=1` 时加入构建 |

---

## 7 优先修复顺序

| 顺序 | 项 | 理由 | 改动量 |
|---|---|---|---|
| 1 | **P2-1 快捷栏 Wi-Fi 统一** | 用户可感知的行为不一致，且会导致"关了又开" | 3 行 |
| 2 | **P2-2 补 lavax/LICENSE** | 发布合规，零风险 | 1 个文件 + 1 行清单 |
| 3 | **P3-3/P3-4 重烧 build/ 并核对** | 现阶段"发布固件=源码"无法证实 | 无代码改动 |
| 4 | P2-3 栈注释纠正 | 防止后人误判余量 | 纯注释 |
| 5 | P3-2 两处任务创建判返回值 | 静默失败 → 可见失败 | 各 2 行 |
| 6 | P3-1 告警清零 | 长期可维护性 | 分批 |

---

## 8 发布质量评价

**结论：可以用于发布，但建议出一个 2.0.1 补 P2-1 与 P2-2。**

理由（基于证据，不基于印象）：
- 构建干净（0 error），分区余量 21.5%，版本信息自洽；
- 真机内存与任务栈没有失控项，最紧的 tcpip 还剩 1.2 KB；
- 没有 P0/P1 级别确证问题；
- 但有**两处"用户能直接碰到"的缺口**：快捷栏关 Wi-Fi 不持久（P2-1）、发布包缺第三方许可（P2-2）。

同时必须承认本轮**没有覆盖**：音频、蓝牙、睡眠、8 个引擎的逐一走查与交叉场景。
这些模块此前的历史结论不能当本轮结论用。要继续收口，建议按 §2 的空白项分三轮做，
每轮只测 2–3 个模块并带真机日志。

---

## 附录 A 待修复问题清单（可直接勾选）

- [ ] **P2-1** 快捷栏 Wi-Fi 改为与设置页同一套（`os_hw_request/release` + `wifi_manager_set_want_on`）
      位置：`main/main.c:151` 或抽 `wifi_manager_set_user_enabled()`；验证：两处各关一次→重启→确认仍关闭
- [ ] **P2-2** `git add components/engines/lavax/LICENSE`；在 `docs/专题/第三方组件与许可证.md` 补 lavax 条目
- [ ] **P2-3** 修正 4 处栈大小注释（bt 4096 / sse 4096 / pcm_out 6144 / netdect 8192 均为**字节**）；
       重测 `sse` 真实用量，作废旧"7.6KB"结论
- [ ] **P3-2** `wifi_manager.c:341` / `:554` 判 `xTaskCreate` 返回值并记日志
- [ ] **P3-3** 用 `build/`（或重新构建）产物烧一次，确认设备版本串 = 2.0.0，再宣称"发布固件=源码"
- [ ] **P3-4** 明确发版用哪个构建目录，避免 `build/` 落后工作区
- [ ] **P3-1** 分批清 136 条告警（先清 `-Wunused-*`，再处理弃用 API）

## 附录 B 本轮未覆盖、需补测的模块

音频链路（ES8311/ES7210、I2S 反复开关）· 蓝牙 BLE 连接/重连/共存 · 深睡唤醒全链路 ·
8 个引擎逐一进出 · 异常拔卡 / 文件损坏 · 20 次页面进出与 20 次应用安装卸载 · 交叉场景 §5。
