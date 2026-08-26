# 规划书：独立 WASM 应用运行时 + 应用商店 + 共享数据层

> 目标设备：ESP32-S3（双核 240MHz / 8MB PSRAM / 16MB Flash）
> 屏幕：ST7305 反射式 1bit LCD 400×300；触摸 CST816/GT911/FT6236(I2C)
> TF 卡：SPI/SDMMC 1bit；三键 KEY(GPIO18)/BOOT(GPIO0)/PWR(GPIO1)
> 工程：ESP-IDF v5.5 组件化，`components/` 下 menu / engine_manager / board_shim / st7305 / input 等

---

## 0. 已核实工程现状（本规划依据）

- `game_run_engine_ops_t`（pause/resume/set_joypad/stop）+ `game_run_loop` **定义在 `components/menu/menu_system.c` 内部（static）**，不是 engine_manager 的公共接口。
- `components/engine_manager` 是统一 **load/unload 注册表**（`engine_desc_t{id,name,load,unload}`，当前已注册 gam4980/gb/nes/arduboy/lavax/vpet）。新增引擎只需在 `s_engines[]` 加一行。
- **wifi_manager 目前只有 STA 连接 API**（`wifi_manager_connect/status/...`），**没有 HTTP 下载 helper**，商店下载需自建或加一层。
- 有 `components/dns_server`（热点模式网页上传可复用)。
- 工程中**尚无任何 WASM 运行时/WAMR/wasm3**，需全新引入。
- 磁盘空间/分区：需核实是否有可放 catalog 缓存与下载临时区的分区或直接放 TF。

---

## 1. 设计决策（已与用户确认）

| 项 | 决策 |
|---|---|
| 应用接入形态 | **独立全屏应用页**（主菜单一个图标 → 全屏运行 → 退出回主菜单），不做进 GB/NES 列表分栏 |
| WASM 运行时 | **WAMR（wasm-micro-runtime）解释模式起步**，后续按需上 AOT |
| 商店下载源 | **已有 HTTP catalog 服务**（appId/name/version/download_url/sha256/icon） |
| 执行顺序 | **先整体模块化，再 WASM 商店** —— 商店作为 components/ui 骨架下的一个模块（见 `docs/规划_菜单模块化.md`） |
| 关键约束 | 不改坏现有 GB/GBC/NES/Arduboy/BBK 词典；大内存一律 PSRAM；依赖下载优先国内源 |

---

## 2. 总体架构（3 层）

```
┌─────────────────────────────────────────────────────────────┐
│  外壳 main.c（不变）                                          │
├─────────────────────────────────────────────────────────────┤
│  components/ui 骨架（模块化菜单，先做）                          │
│   ├─ 模块注册表 module registry（含运行时注册/卸载接口）          │
│   └─ 各应用页模块：statusbar / main_menu / settings /           │
│       select_game / book / mp3 / pomodoro / usb_hid / ...     │
│                      │  <-- 商店作为一个模块                    │
├─────────────────────────────────────────────────────────────┤
│  WASM 应用子系统（本规划书主体）                                 │
│   ├─ components/wasm_engine  (WAMR 封装 + Host API)          │
│   ├─ components/app_store    (商店 UI + 下载/解压/安装)        │
│   ├─ components/app_pkg      (manifest 解析 / .pak 解包)       │
│   ├─ components/shared_fs    (全局共享数据层 + 白名单)          │
│   └─ tools/wasm_pack         (宿主端打包工具)                  │
├─────────────────────────────────────────────────────────────┤
│  TF 卡数据区  /sdcard/                                       │
│   ├─ apps/<appId>/app.wasm + manifest.json + assets/ + save/ │
│   └─ shared/<ns>.json                                        │
└─────────────────────────────────────────────────────────────┘
```

### 分层职责
- **ui 骨架**：负责页面编排、弹窗、主菜单图标、focus/action 路由。WASM 应用以"应用页模块"身份被注册，进入时启动 wasm_engine，退出时 unload。
- **wasm_engine**：WAMR 实例管理 + Host API，对外只暴露 `wasm_engine_start(appId)/wasm_engine_stop()`。
- **app_store / app_pkg / shared_fs**：均做成模块，可被 ui 骨架作为页面打开，或被 wasm 应用调用。

---

## 3. 应用目录与沙箱约定

```
/sdcard/apps/<appId>/
  app.wasm          # 唯一运行入口，实现统一导出（见 §5）
  manifest.json     # name/version/icon/说明/shared_write 白名单
  assets/           # 只读资源（图片/音效/字库/关卡）
  save/             # 应用私有可写（存档/配置），删除应用即删
/sdcard/shared/     # 全局共享库（不属于任何应用，重装不丢）
```

Launcher（菜单应用页）扫描 `/sdcard/apps/*/app.wasm`，依 manifest 展示图标/列表。

### 沙箱规则
- 应用只能经 Host API 访问文件，**绝不暴露裸路径**。
- `wfile_*` 路径强制拼接为 `/sdcard/apps/<appId>/<相对路径>`，`..` 与绝对路径一律拦截（防止越权读别的应用）。
- `wshared_*` 写必须过 manifest `shared_write` 白名单校验。

---

## 4. WASM 运行时（components/wasm_engine）

### 选型
- **WAMR**（wasm-micro-runtime）解释器，接入 ESP-IDF（WAMR 官方支持 ESP-IDF，放 `components/wasm_engine/wamr` 或 managed_component）。
- wasm 线性内存 / 实例堆 / Host 事件缓冲：一律 PSRAM（`MALLOC_CAP_SPIRAM` 或 `EXT_RAM_BSS_ATTR`）。
- 所有应用**共用一个运行时实例**，按需 load(从 TF 读 app.wasm)→start→run→unload。

### 初始化与内存预算
- 首次启动：`wasm_runtime_init()` 分配运行时堆（PSRAM）；极简配置以控制体积。
- 内存预算（估算）：WAMR interpreter core ~150–300KB（PSRAM）；单 app 线性内存按 manifest 需求给，默认 128KB–512KB；共享帧缓冲 400×300 1bpp = 15KB（PSRAM）。
- 启动前用 `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` 校验 `hello` 留下足空间。

---

## 5. Host API（wasm import / 宿主 native）

> V2 定稿（2026-08-26）：系统为全自研、信任度高，**全套接口均暴露给 WASM 应用**，仅排除「重启 / 擦写分区」两项危险操作。

应用导出（wasm export，供宿主调用以控制帧节奏）：
```
app_init()
app_tick(dt)          # 每帧被调用一次
app_exit()
```
宿主注册的 import（wasm 侧 `import "env"..."`）—— 全集：
```
显示:    wdraw(x,y,w,h,像素缓冲) / wflip() / wclear()
输入:    wbtn(i) / wbtnp(i) / wtouch() -> {x,y,down}
音频:    wbeep(freq, dur) / wfeed_pcm(buf, len, sr)
震动:    wvib(dur, strength)
Wi-Fi:   wwifi_connect(ssid,pass) / wwifi_status() / wwifi_scan()
蓝牙:    wbt_status() / wbt_pad_state()              # 只读查询
文件:    wfile_read(path, buf, len) -> int
         wfile_write(path, buf, len) -> int          # 仅本应用目录
         wdir(path) -> 列表
数据库:  wshared_get(ns, key, buf, len) / wshared_set / wshared_del / wshared_list(ns)
时间:    wmillis() / wdate()
系统信息: wsys_battery() / wsys_mem()
HTTP:    whttp_get(url) / whttp_post(url, body)
弹窗:    wtoast(msg)                                 # 调系统统一弹窗 os_dialog
应用协作: wlaunch(app_id) / wexit()
音量:    wvolume_get() / wvolume_set(v)
随机:    wrand(max)
```

### 像素格式说明
ST7305 为 1bit 反射屏。建议 `wdraw` 收 **1bpp 行主序位图**（400×300 ⇒ 15000 字节/帧），宿主再逐像素写到 st7305。小应用可仅画部分区域 + 局部 flush。

---

## 6. 全局共享数据层（components/shared_fs）

两条并立：
- **私有**：`/sdcard/apps/<appId>/save/`，裸路径，仅本应用。
- **共享**：`/sdcard/shared/<ns>.json`，key 格式 `"ns:key"`（progress:lvl3 / score:best / inventory:coins），值支持 int/float/string/短 JSON。

设计要点：
1. **读写都经 wshared_\* API**，绝不把裸路径暴露给 wasm。
2. **读开放所有应用；写必须通过 `manifest.json.shared_write` 白名单**（如 `["progress","score"]`），宿主校验未声明命名空间即拒绝。
3. 每命名空间一 JSON；写采用 **写临时文件 → fflush/fsync → rename 原子替换**（单 JSON 幂等弱序，防掉电损坏）。
4. 无需事务/锁——一次仅运行一个应用，写冲突极小。

---

## 7. 应用商店（components/app_store）— 已有 HTTP catalog 服务

目录项：
```
{
  "appId": "snake",
  "name": "贪吃蛇",
  "version": "1.0.0",
  "download_url": "http://host/snake.pak",
  "sha256": "…",
  "icon": "data:image/… 或 url"
}
```

安装流程：
1. WiFi：`wifi_manager_connect`（复用现有加密键盘/状态）。
2. ESP HTTP Client 拉取 catalog → 列表页。
3. 选中的应用：下载单个 `.pak(zip)` 到临时区 → 校验整包 sha256 → 解压到 `/sdcard/apps/<appId>/` → 校验 app.wasm + manifest 存在 → 加进 Launcher 列表 → 可运行。
4. 网络优先 HTTP 落地；本机热点 + 网页上传作为 fallback（复用 dns_server）。

---

## 8. 分阶段任务（每步可编译、可单独验收）

**Phase 0 — 接入 WAMR**
- `components/wasm_engine` 最小封装：初始化 + 从 `/sdcard/apps/hello/app.wasm` 加载 + `app_tick` 循环。
- 验收：串口输出 `[WASM] OK`，且画 1 像素；`wmillis/console` 可用。

**Phase 1 — 打包工具 tools/wasm_pack**
- C 源码（wasi-sdk/emcc 或 Go tinygo）→ app.wasm；产出 `manifest.json`。
- 验收：产出可在设备上运行的单目录应用。

**Phase 2 — wasm_engine 接入模块化骨架**
- 实现游戏循环 ops 对接 ui 骨架应用页；Launcher 扫描 `/sdcard/apps`；启动/退出/卸载释放内存。
- 验收：菜单能列出多个 WASM 应用并逐个切换、退出内存回落到基线。

**Phase 3 — 补齐 Host API（wdraw/wbtn/wbeep/wfile/wmillis/wshared\*）**
- 验收：内置一个小游戏 + 一个小工具跑通；共享 API 在 A/B 应用间读写（A 写 progress，B 读到）。

**Phase 4 — 共享库原子写 + shared_write 白名单**
- 验收：越权写被拒绝；原子写掉电不损坏 JSON。

**Phase 5 — 商店（catalog→下载→校验→解压→安装→刷新）**
- 依赖本规划 §7；验收：能从在线 catalog"安装"一个应用并运行。

**Phase 6 — 打磨**
- 版本/更新/删除、图标、PSRAM 内存基线、崩溃隔离与恢复。

> 注意：Phase 0-1 可先于"整体模块化"并行推进（wasm_engine 是独立组件，不动 menu 核心）；从 Phase 2 起依赖 components/ui 骨架就绪。

---

## 9. 与既有接口的冲突点与对策

| 现有 | 规划 | impact |
|---|---|---|
| `game_run_engine_ops_t` 是 menu 私有 static | 不强制复用；wasm_engine 走组件级独立 app 页接口 | 零侵入 menu |
| `engine_manager` 只有 load/unload | 新增 `ENGINE_WASM` 作为可选项（不做进游戏分栏） | 低 |
| `wifi_manager` 无 HTTP 下载 | 商店模块自带 `app_http_download()`（基于 esp_http_client） | 低 |
| PSRAM 紧张 | wasm 线性内存/堆/帧缓冲全走 PSRAM；hello 预留校验 | 需持续监控 |

---

## 10. 风险清单

- **WAMR 体积/内存**：解释模式在 ESP32 上 footprint 需实测；PSRAM 空闲量是关键门槛 → 先 hello 留足空间。
- **1bit 屏性能**：整帧 wdraw 高频 flush 可能拉低 FPS → 支持局部 flush。
- **在线下载失败/断点**：HTTP 需超时重试与尾部验证；下载失败不污染已有应用。
- **沙箱越权**：路径拼接 + 白名单双保险；需写单测覆盖 `..`、绝对路径、未申请命名空间。
- **与现有逻辑冲突**：全程新组件接入，不修改 menu_system 既有模拟器路径。

---

## 11. 交付物（本规划完成后）
- 修改/新增文件清单（每 Phase 落一个 `docs/` 说明）。
- 每阶段自测方法（含串口期望输出 / 菜单操作路径）。
- 内存占用数据（内部 RAM / PSRAM 基线，WASM 运行时 + 应用内存）。

---

> 当前阶段：**规划书 v1 待确认**。按已确认决策，下一步进入菜单模块化（`components/ui` 骨架），WASM 商店排在其后。