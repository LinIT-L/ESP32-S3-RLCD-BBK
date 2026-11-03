# LinTOS 2.0 第三方应用体系（商店范围）

> **边界**：本文件只定义**应用商店里的第三方应用与资源包**。
> 内置 C 应用（`apps_registry.c` 的 33 条）**不参与**本体系，行为与身份保持不变。
>
> 状态标记：`[已实现]` 有代码且已编译/单测通过；`[进行中]` 代码在，链路未接通；`[未开始]` 只有约定。

---

## 1. 身份：app_id

### 1.1 规则

```
com.lintos.<name>            应用
com.lintos.resource.<name>   资源包
```

- 小写、点分段、≥2 段、≤63 字符；允许 `a-z0-9-_`
- **与中文名、文件名、URL、安装路径全部解耦**
- 升级/改名/换目录**都不改 ID**
- 重复 ID 拒绝安装（`pack_lpk.py --app` 会查 `store/index.json` 并告警）

### 1.2 现状对照（`[已实现]` 校验规则；`[未开始]` 设备端强制唯一）

| 应用 | 现身份 | 目标 app_id |
|---|---|---|
| 录音机 | `lua:录音机.lua` | `com.lintos.recorder` |
| 航班雷达 | `lua:航班雷达.lua` | `com.lintos.flight-radar` |
| 矢量地图 | `lua:矢量地图.lua` | `com.lintos.vector-map` |
| 自行车码表 | `lua:自行车码表.lua` | `com.lintos.bike-comp` |
| BIOS 模拟器 | `lua:BIOS模拟器.lua` | `com.lintos.biossim` |
| 人生指南 | `lua:人生指南.lua` | `com.lintos.lifeguide` |
| 英雄坛说 | `pkgs/wuxia.zip` | `com.lintos.wuxia` |
| 仙剑奇侠传 | `pal` + `pkgs/pal.lpk` | `com.lintos.sdlpal` |
| 10 个字体包 | `pkgs/font_*.zip` | `com.lintos.resource.font.<name>` |

> ⚠ 内置 `pal` 的 ID **不改**（属内置应用）；第三方侧用 `com.lintos.sdlpal` 表示"资源包+启动依赖"。

## 2. 元数据：manifest.json

规范见 `lpk_format.md` §4。要点：

- 唯一真相源是**包内 manifest**，不是商店索引，也不是文件名
- 商店索引只负责"分发信息"（大小/CRC/SHA/版本/build/URL），通过 `app_id` 关联
- 安装时**交叉核对**：索引的 `app_id`、`version`、`package_type` 必须与 manifest 一致，否则拒装

`[已实现]` 解析与校验：`components/services/lpk/lpk_manifest.c`（10 项主机用例全过）
`[未开始]` 安装器侧的"索引 ↔ manifest 交叉核对"

## 3. 目录规范

### 3.1 目标结构

```
/sdcard/apps/
├── installed/<app_id>/         应用解包后的文件（程序 + 图标 + 自有资源）
├── data/<app_id>/              用户数据（卸载默认保留）
├── cache/<app_id>/             可删缓存
├── staging/                    下载中 / 安装中（.part、临时解包）
├── quarantine/                 校验失败隔离区（取证用，可手动清）
└── registry/                   安装记录（app_id → 版本/build/来源/文件清单）
```

资源包：

```
/sdcard/resources/fonts|themes|shared|game/<resource_id>/
```

### 3.2 与现状的差异（**这是必须迁移的部分**）

| 现在 | 目标 | 影响 |
|---|---|---|
| `/sdcard/apps/{engine,app,ops}/*.lua` | `/sdcard/apps/installed/<app_id>/` | 分类不再由目录决定（现由 `fid` 决定，是已修过的双判据隐患来源） |
| `/sdcard/apps/data`（**共用**） | `/sdcard/apps/data/<app_id>/` | 现在所有 Lua 应用共用一个数据根，无隔离 |
| `/sdcard/fonts` | `/sdcard/resources/fonts` | 字体加载器要跟着改 |
| `/sdcard/app`（旧）| 已在 2026-10-25 迁到 `/sdcard/apps/data` | — |

`[未开始]` 迁移。**这一步会移动用户数据，属于不可逆操作，需单独授权后执行。**

## 4. 安装 / 升级 / 卸载

### 4.1 安装（`[未开始]`，规范已冻结）

```
下载 → staging/<app_id>.part
  → size / CRC32 / SHA-256
  → lpk_open + lpk_verify_pkg
  → lpk_mf_load（manifest 校验 + entry/icon 在包内）
  → 解包到 staging/<app_id>.tmp/
  → 复核文件数 + 总字节
  → rename → installed/<app_id>/
  → 写 registry/<app_id>.json
  → 注册到应用中心
```

失败：删 `staging/*`，`installed/` 保持原样（**不留半装状态**）。

### 4.2 升级

- 同 `app_id` + `build` 更大 ⇒ 升级
- 新版本先落 `staging`，校验通过才 `rename` 覆盖；旧目录改名 `.old` 保留到新版本启动成功
- **用户数据不动**（`data/<app_id>/` 不参与替换）
- 顺序**不能**是"先删后下"

### 4.3 卸载

- 删 `installed/<app_id>/`、`cache/<app_id>/`、`registry/<app_id>.json`
- **默认保留** `data/<app_id>/`；另给"同时删除数据"选项
- 资源包卸载前检查反向依赖（谁在用这个字体/引擎），有引用则拒绝

## 5. 资源包

| 类型 | `package_type` | `resource_type` | 可否启动 |
|---|---|---|---|
| 应用 | `app` | — | 是 |
| 字体 | `resource` | `font` | 否 |
| 主题 | `resource` | `theme` | 否 |
| ROM | `resource` | `rom` | 否 |
| 游戏素材 | `resource` | `gamedata` | 否 |
| 共享组件 | `resource` | `shared` | 否 |

`lpk_mf_launchable()` 只对 `package_type=app` 返回 true ⇒ 资源包**结构上不可能**出现在可启动列表里。

## 6. 商店索引（schema 2）

### 6.1 目标

```json
{
  "schema_version": 2,
  "packages": [{
    "app_id": "com.lintos.recorder",
    "name": "录音机",
    "category": "software",
    "package_type": "app",
    "version": "2",
    "build": "202610310919",
    "size": 46080,
    "crc32": "d3e4e434",
    "sha256": "7b95ae7c…",
    "path": "packages/com.lintos.recorder.lpk",
    "min_firmware": "2.0.0"
  }]
}
```

### 6.2 与现状

| 项 | 现在 | 目标 |
|---|---|---|
| 顶层键 | `pkgs[]` | `packages[]`（`pack_lpk.py` 已两种都认） |
| 关联键 | `name`（**中文**） | `app_id` |
| 校验 | `size` + `crc32` | 追加 `sha256` |
| 类型 | `lua` / `asset` / `lpk` | `app` / `resource` + `runtime` |
| 包路径 | `pkgs/<中文名>.lua` | `packages/<app_id>.lpk` |

`[未开始]` 服务器与设备端同步切换。**两端必须同批上线**，否则商店会空列表。

## 7. 已实现 / 未实现（如实）

| 能力 | 状态 | 证据 |
|---|---|---|
| LPK 容器读写（含重名/越界/大小写冲突拒绝） | `[已实现]` | `lpk1.c`，原有 40 项用例 |
| manifest 解析 + 规则校验 + entry/icon 在包内 | `[已实现]` | `lpk_manifest.c`，10 项用例全过 |
| 打包工具 `--app`（校验 + CRC32 + SHA-256 + 索引记录 + 重复 ID 告警） | `[已实现]` | `tools/pack_lpk.py` |
| 真实应用包样例（录音机 46 KB，3 条目） | `[已实现]` | `/tmp/apps/com.lintos.recorder.lpk` |
| 固件完整编译 | `[已实现]` | `LinTOS.bin` 4,177,648 B，app 余 20% |
| 设备端安装器消费 manifest | `[未开始]` | 现有 `am_lpk.c` 只验包+写模式，不读 manifest |
| 新目录 `installed/<app_id>/` 与迁移 | `[未开始]` | 需授权（移动用户数据） |
| 设备端第三方注册层（按 app_id） | `[未开始]` | 现由 `am_lua.c` 按文件扫描 |
| 商店索引 schema 2 + 服务器 | `[未开始]` | 现为 `pkgs[]` + 中文 `name` |
| 现有 18 个商店包迁移 | `[未开始]` | 1 个样例已跑通 |

## 8. 下一步（按依赖顺序）

1. **安装器消费 manifest**：下载完成后 `lpk_mf_load()` → 校验索引一致性 → 解包到 `installed/<app_id>/`
2. **注册层**：扫 `installed/` + `registry/`，读 manifest 生成条目（不再按文件名猜）
3. **迁移工具**：把现有 `.lua`/`.zip` 包批量转成 `.lpk`（用 `pack_lpk.py --app`）
4. **索引 schema 2 + 服务器**（需授权）
5. **删除旧链路**：`type=lua` / `type=asset` 安装分支（确认无引用后）
6. **数据目录迁移**（需授权）

> 每一步都必须先有可回滚点：当前 `installed/` 与旧目录**并存**，注册层优先读新目录，
> 旧目录只读兜底 —— 直到全部迁移验证通过再删旧链路。

---

## 9. 大资源应用的性能规范（"仙剑方案"）

> **结论：以后凡是"单个大资源包 + 运行期随机读"，一律按这一套做。**
> 不是可选项，是默认做法。反过来，小文件应用**不要**套用（负收益）。

### 9.1 什么时候必须用

同时满足两条就要用：

1. 资源集中在一个**大文件**里（经验值：> 8 MB，且在 FAT 上占数百~上千簇）；
2. 引擎/应用会**按偏移随机读**（读索引 → 跳数据区 → 再跳回索引）。

典型：仙剑 SDLPal 读 `ABC.MKF` 的 `fseek(4*chunkNum) → fread(8B) → fseek(offset) → fread(chunk)`。
**同一个文件里来回跨整个文件跳两次**，每跳一次 FAT 簇链就要从头走。

### 9.2 方案（唯一正确解）

**每句柄 4 个 64 KB 预读窗 + LRU**，窗口从 **PSRAM** 分配：

```c
lpk_m_set_prefetch(m, 64*1024, 4*1024*1024, pref_psram_alloc, pref_psram_free, NULL);
```

- 索引窗常驻 + 数据窗跟着 chunk 走 ⇒ 来回跳都能命中
- 一次未命中就**顺序读满一整窗**，把"每 4 KB 走一次簇链"变成"每 64 KB 一次"
- 分配失败 **不报错**，退化为逐次直读（性能降级，功能不变）
- ⚠ **绝不回落到内部 RAM**：内部只剩 ~160 KB，16 句柄 × 256 KB 会直接打死系统
- 运行期开关：串口 `lpkstat 0|1`（关的时候立刻归还 PSRAM，用于 A/B 对照）

### 9.3 两个**错误**方案（都真机验证过，不要再碰）

| 错误做法 | 症状 | 原因 |
|---|---|---|
| open 时整资源预载进 PSRAM，且全程持全局锁 | **40 s 出不了首帧 + 全程没声音** | 读一个 1.5 MB 资源锁 3.5 s，游戏和音频一起排队 |
| 单窗口预读（一个大窗） | 比不预读**更慢** | 引擎每次跳转都重建窗口，多读 64 倍带宽 |

### 9.4 实测（同机、同卡、同资源）

| 模式 | STAGE2→3 | 首帧 |
|---|---|---|
| 目录（109 个散文件） | 6.5 s | 11.8 s |
| LPK 无预读 | 37.5 s | 40 s 内 0 帧 |
| **LPK + 4×64 KB 预读** | **6.8 s** | **13.8 s** |
| LPK + 4×128 KB | 6.9 s | ≈13.8 s（不值得，回退 64 KB） |

### 9.5 什么时候**不要**用

- 第三方 Lua 应用（录音机/航班雷达…）：安装时解包成小文件，一次 seek 只跨几簇。
  给小文件加 64 KB 预读 = 读 4 KB 搬 64 KB，**负收益**。
- 字体包/资源包：解包到目录后由字体加载器 mmap，同样不经过 `/lpk`。

### 9.6 新应用接入 checklist

```
□ 资源是否 > 8 MB 且集中在单文件？         否 → 用普通解包，别开预读
□ 是否按偏移随机读（而非纯顺序）？         否 → 用普通解包
□ 挂 /lpk（lpk_vfs_mount），确认 pal 式 FILE* 通路能走通
□ lpk_m_set_prefetch(64KB, 4MB, PSRAM 分配器)
□ 真机测：lpkstat r → 进应用 → 记 STAGE 耗时与 KB/s
□ 与"解包目录"模式对比首帧；差距 > 2 s 就调窗口，不要超过 128 KB
□ 失败退化路径必须验证：把 PSRAM 吃满，确认仍能跑（只是慢）
```

---

## 10. 上架前检查清单（2026-11-01 整理结果）

### 10.1 已完成并可复验

| 项 | 命令 | 结果 |
|---|---|---|
| LPK 容器 + manifest 校验 | `python3 tests/lpk/lpk_mf_cases.py` | 10/10 PASS |
| 全量 LPK 测试 | `SAN=0 sh tests/lpk/run_all.sh` | 全过 |
| 7 个新包过设备端规则 | 逐个 `lpk_mf_load` | 7/7 PASS |
| 索引 ↔ 包一致性 | size/CRC32/SHA-256 三方比对 | 7/7 PASS |
| 固件编译 | `bash build.sh` | 通过 |

### 10.2 迁移结果

| 包 | app_id | 类型 | 大小 | 状态 |
|---|---|---|---|---|
| 录音机 | com.lintos.recorder | app | 46,592 | ✅ |
| 航班雷达 | com.lintos.flight-radar | app | 53,760 | ✅ |
| 矢量地图 | com.lintos.vector-map | app | 177,152 | ✅ |
| 自行车码表 | com.lintos.bike-comp | app | 22,528 | ✅ |
| BIOS 模拟器 | com.lintos.biossim | app | 223,232 | ✅ |
| 人生指南 | com.lintos.lifeguide | app | 391,168 | ✅ |
| 英雄坛说 | com.lintos.wuxia | app（含素材） | 1,891,328 | ✅ |
| 仙剑奇侠传 | com.lintos.sdlpal | resource(gamedata) | 41.7 MB | ⛔ 未迁：C 引擎读 `/lpk`，改格式等于重做安装链路 |
| 10 个字体包 | com.lintos.resource.font.* | resource(font) | — | ⛔ 未迁：**本地无源文件，只在服务器上** |

### 10.3 上架顺序（不能颠倒）

```
① 上传 store/packages/*.lpk  → 服务器 /bbk/packages/
② 烧新固件（0x60000，保 NVS）
③ 改线上 index.json：这 7 条的 path 指向 packages/<app_id>.lpk、type=lpk
④ 真机点一次下载 → 验证 安装/桌面图标/启动/卸载
```

**为什么不能先改索引**：旧固件没有 manifest 支持，遇到新包会报"安装失败"——比空列表更糟。

### 10.4 仍未实现（如实）

| 缺口 | 影响 | 处理建议 |
|---|---|---|
| 设备端解析 `packages[]`（schema 2） | `store/index.v2.json` 目前**设备读不了** | 先用 v1 的 `pkgs[]` + `type=lpk` 过渡；schema 2 需改 `am_store_json.c` |
| 资源包（字体）走新安装器 | 字体仍走老 zip 路径 | 先把字体包拉到本地再迁 |
| 真机安装验证 | 全部为代码 + 主机验证 | 烧录后按 10.3 第 ④ 步验 |
| 旧安装分支删除 | `type=lua` / `type=asset` 仍在 | 7 个包真机验证通过后再删 |

### 10.5 包名契约（商店判定依赖它，必须遵守）

```
索引 path 的文件主干  ==  manifest 的 app_id
例：packages/com.lintos.recorder.lpk  ↔  app_id = com.lintos.recorder
```

原因：商店判断 `type=lpk` 是否已安装时，从 `rp->path` 取主干，再查
`/sdcard/apps/registry/<主干>.txt`。主干与 app_id 不一致会导致
**装完了仍显示"未安装"、卸载找不到目标**。

`tools/pack_lpk.py --app` 输出时就按 `app_id` 命名，`migrate_store.py` 也强制如此；
新增包请勿手工改名。

## 11. 商店条目状态机（2026-11-01 定稿，唯一实现）

### 11.1 一条铁律

**商店里一行的状态只允许有一个来源**：`am_store_state_of()`（`pages/am_store_state.h`）。
按钮文案、第二行文案、详情弹窗、点击行为、卸载确认全部从它派生。
任何地方都不许再"看一眼文件在不在"就自己下结论 —— 2026-11-01 那轮两处状态错乱
就是这么来的（按钮按 target 目录判、点击按 .lua 文件判、卸载按 .lpk 后缀判，三套口径）。

### 11.2 状态表

| 实际状态 | 按钮 | 触发条件 |
|---|---|---|
| 未安装 | 下载 | `registry` 无记录且安装目录不存在 |
| 已安装·版本一致 | 卸载 | 本地版本 == 索引版本 |
| 已安装·商店有新版本 | 更新 | `am_st_ver_newer() > 0` |
| 下载中 / 安装中 / 更新中 | 同左 | 全局下载任务正在处理这一行 |
| 卸载中 | 卸载中 | 该 app_id 的卸载任务在跑 |

失败（下载/校验/安装）**不改最终状态**：下载校验失败只删 `.part`，安装失败回滚到旧版本，
按钮回到「下载」/「更新」，不会出现半装。

### 11.3 版本口径：以**索引**声明为准，而不是包内 manifest

`registry/<app_id>.txt` 同时记两套版本：

```
version=2              build=202610310919        ← 包内 manifest 自称的
store_ver=3            store_build=202611011048  ← 商店索引交付时的
```

更新判定用 `store_*`。原因（2026-11-01 实测）：

线上索引把 6 个 LPK 的 `ver/build` 统一抬到 `202611011048`，但**包体没有重新打包**，
包内 manifest 仍是旧值（录音机 ver=2/build=202610310919，SHA-256 与索引一致 ⇒ 确实是同一份）。
若按包内版本比 → 用户"更新成功"后仍显示「更新」，点一次下一次，永远退不掉。
所以安装时把索引版本一起记下来，两套不一致时 `am_lpkg_info()` 打 WARN：

```
版本口径不一致 com.lintos.recorder: 包内 2/202610310919 vs 商店 3/202611011048
（按商店口径判定；服务器索引建议重新生成）
```

**发版规范**：改索引的 `ver`/`build` 必须同时重新打包（`pack_lpk.py --app` 会写 manifest），
否则每次都会留下这个 WARN。见 §10.3 上架顺序。

### 11.4 已安装判定（两套体系并存，按归属路由）

| 包来源 | 判据 | 卸载 |
|---|---|---|
| 新体系（包内带 manifest） | `registry/<app_id>.txt` 或 `installed/<app_id>/manifest.json` | `am_lpkg_uninstall(app_id, keep_data=true)` |
| 老体系（无 manifest，如 `pal.lpk`） | `<target>/.installed.<stem>` + `.pkg.<stem>.lst` 清单回验 | 按清单删 / 整目录删 + 清 `.install_mode` |
| zip 资源包（字体） | 同上（标记 + 清单逐文件回验） | `am_asset_uninstall()` 只删本包文件 |

**stem 只去最后一个扩展名**：`com.lintos.recorder.lpk → com.lintos.recorder`。
早期实现截到第一个 `.`（得到 `com`），导致装好的应用永远"未安装"、卸载删不到东西 —— 
`am_lpkg_stem_of()` 与 `am_lpk_stem()` 两份实现必须保持同一语义。

### 11.6 安装/更新的提交顺序（失败必定回到旧版本）

```
① 解包 → staging/<app_id>.new          逐条写盘 + 字节数核对，失败直接清掉
② 掉电自愈：installed/<id> 不在、staging/<id>.old 在 → 先把它 rename 回去
③ installed/<id> → staging/<id>.old    （旧版先让位，**先不删**）
④ staging/<id>.new → installed/<id>    （新版就位；失败则把 .old 改回来）
⑤ 写 registry：.tmp → 回读校验 → registry/<id>.txt（原记录先改名 .bak 保底）
⑥ ⑤ 成功才删 .old 与 .bak              （⑤ 失败：删新目录 + 恢复旧目录/旧记录）
```

任何一步失败的结果只有两种：**旧版本（显示「更新」，可重试）** 或 **新版本（显示「卸载」）**。
不存在"文件新、记录旧"或"记录新、文件旧"的中间态 —— `tests/installer/` 用真文件真改名
把这几条跑了一遍（含 `chmod 0555` 制造记录写失败、手工制造掉电现场）。

### 11.7 测试入口

| 测试 | 命令 | 覆盖 |
|---|---|---|
| 状态判定 | `sh tests/store_state/run_all.sh` | 版本比较、状态表、registry 解析、异步结果防覆盖（含 ASan） |
| 安装器动作 | `sh tests/installer/run_all.sh` | 首装/更新/回滚/掉电自愈/冷启动恢复/老记录兼容（含 ASan） |
| LPK 容器与 manifest | `SAN=0 sh tests/lpk/run_all.sh` | 容器格式、manifest 校验、模式判定 |

宿主机会话里跑安装器测试的办法：用 `-DLPKG_ROOT='"/tmp/xxx"'` 把安装根指到临时目录，
编译同一份 `am_lpkg.c`（生产不传该宏，路径与以前逐字节一致）。

### 11.5 列表刷新：双缓冲，不允许空白

```
后台扫描 → 写 s_pkg_bg[]（静止数据不动）→ 扫完置 s_bg_ready
UI tick  → 换指针（4 字节原子写）→ 通知重建 → 一次性显示新数据
```

- 扫描**不预先清空**读侧，`am_store_ready()` 在"已有数据"时恒为 true；
- 索引内容变化用 `am_rstore_gen()`（内容代次）判断，不是比条数 ——
  条数不变但内容换了（换版）时旧视图会带着错的 `ridx`，版本/路径全错位；
- 状态缓存带代次 `s_st_gen`：扫描换缓冲、索引变化、安装/卸载完成都 +1，
  旧异步结果带着老代次回来会被拒绝（`am_st_cell_put` 的规则，有主机测试）。
