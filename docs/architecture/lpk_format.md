# LPK 第三方应用包格式规范（v1）

> 适用范围：应用商店里的**第三方应用与资源包**。内置 C 应用不在本规范内。
> 实现：`components/services/lpk/`（`lpk1.c` 容器、`lpk_manifest.c` 元数据校验），
> 工具：`tools/pack_lpk.py --app <目录>`，测试：`tests/lpk/lpk_mf_cases.py`。
>
> 本规范只增加**约定**，不改变容器二进制格式 —— 老包（仙剑 pal.lpk）继续可读。

---

## 1. 一句话

一个 `.lpk` = 一个应用的完整分发单元：**程序 + 元数据 + 图标 + 资源**，卡上只存这一个文件，
运行时按偏移直接读，不解压、不散落。

## 2. 二进制布局（全部小端）

```
┌────────────────────────────┐ 0
│ HEADER (64 B)              │
├────────────────────────────┤ 64
│ LUA ENTRY (原始字节)        │  lua_off / lua_size（应用包可为 0）
├────────────────────────────┤
│ ENTRY TABLE                │  man_off / man_size
│   count × 32B 记录          │
│   + 文件名池 (UTF-8)        │
├────────────────────────────┤
│ RESOURCE DATA (原始字节)     │  512 B 对齐，无压缩
└────────────────────────────┘  total_size
```

### 2.1 HEADER（64 B）

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | char[4] | magic | `"LPK1"` |
| 4 | u16 | version | 1 |
| 6 | u16 | hdr_size | 64 |
| 8 | u32 | total_size | 必须等于真实文件大小（防截断） |
| 12 | u32 | lua_off | Lua 入口偏移（无则 0） |
| 16 | u32 | lua_size | |
| 20 | u32 | lua_crc | |
| 24 | u32 | man_off | 索引块偏移 |
| 28 | u32 | man_size | |
| 32 | u32 | count | 条目数（≤ 8192） |
| 36 | u32 | data_off | 资源区起点（512 对齐） |
| 40 | u32 | pkg_crc | 整包 CRC32（本字段按 0 参与计算） |
| 44 | u32 | flags | bit0 = 含资源 |
| 48 | u8[16] | reserved | 必须全 0 |

### 2.2 ENTRY（32 B）

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u32 | name_off（相对 `man_off`） |
| 4 | u32 | off（绝对） |
| 8 | u32 | size |
| 12 | u32 | crc32 |
| 16 | u16 | name_len |
| 18 | u8 | type（0 = raw） |
| 19 | u8 | reserved0 |
| 20–31 | u32×3 | reserved |

### 2.3 为什么不做压缩

- SD 卡裸读实测 **~430 KB/s**，`inflate` 在 S3 上约 2–4 MB/s ⇒ **解压没有收益**，
  却要付出"解压时占锁、占 RAM、可能被压缩炸弹打穿"的代价。
- 大文件（仙剑 41.7 MB）若整体解压，必须先落盘一遍，卡上写两遍 —— 实测白写 133 s。
- 结论：**LPK 不做文件级压缩**。将来真需要，`type` 字段已预留。

## 3. 应用包内容约定

```
recorder.lpk
├── manifest.json     必须，包内路径固定
├── icon.bin          可选（声明了就必须在包里，且正好 512 B）
├── app/entry.lua     runtime=lua 的入口
├── assets/…          应用自带资源（可空）
└── resources/…       resource 包的资源（可空）
```

> `manifest.json` / `icon.bin` 都是**普通条目**，用 `lpk_find_ci()` 按名字取，
> 与读 `ABC.MKF` 没有区别 —— 不需要任何格式扩展。

## 4. manifest.json

### 4.1 必填

| 字段 | 约束 |
|---|---|
| `schema_version` | 必须为 `1` |
| `app_id` | 小写、点分段、≥2 段、≤63 字符，如 `com.lintos.recorder` |
| `label` | 显示名（可中文） |
| `version` | 非空 |
| `package_type` | `app` 或 `resource` |

### 4.2 按类型追加

- `package_type=app`：
  - `runtime` ∈ `lua` / `native` / `engine`
  - `entry` 必填，包内相对路径，**且必须真实存在于包里**
  - `icon` 可选；声明则在包里且长度 = 512
- `package_type=resource`：
  - `resource_type` 必填（`font` / `theme` / `rom` / `gamedata` / `shared`）
  - **不允许** `entry`（资源包不可启动）

### 4.3 可选

`name`、`description`、`category`、`build`、`author`、`dependencies`、`permissions`、
`data_policy`、`min_firmware`、`install_target`。

### 4.4 分类

程序只认稳定 ID，中文只是输入别名（`lpk_mf_cat_id()` 归一）：

| 中文 | 稳定 ID |
|---|---|
| 软件 / 程序 / 工具 | `software` |
| 游戏 | `games` |
| 学习 | `learning` |
| 运维 | `operations` |
| 系统 | `system` |
| 壁纸 | `wallpaper` |

## 5. 安全限制（缺一即拒装）

| 项 | 规则 | 拒绝码 |
|---|---|---|
| manifest 缺失 | 包里没有 `manifest.json` | `-3` |
| manifest 过大 | > 8 KB | `-4` |
| JSON 非法 | 顶层不是对象 / 未闭合 | `-5` |
| 字段非法 | schema/app_id/type/runtime | `-6` |
| 路径非法 | 绝对路径、`..`、`\\`、`//`、控制字符、结尾 `/` | `-7` |
| 入口缺失 | 声明的 entry 不在包内 | `-8` |
| 图标缺失 | 声明的 icon 不在包内 | `-9` |
| 图标尺寸 | ≠ 512 B | `-10` |
| 重名条目 | 完全同名 | `lpk1` 打开即拒 |
| 大小写冲突 | 仅大小写不同 | `lpk1` 打开即拒 |
| 越界偏移 | 条目越过文件尾 | `lpk1` 打开即拒 |
| 整包损坏 | `pkg_crc` 不符 | `lpk_verify_pkg()` |

**安装目标由固件决定**，不接受 manifest 里指定任意绝对路径（`install_target` 仅作建议值，固件另有白名单）。

## 6. 图标规范

- 64×64，**1bpp**，MSB first，**1 = 黑**
- 原始数据，无文件头，固定 **512 字节**
- 与 LAPP 头里的 `icon=` base64 解码后同格式（迁移零转换）
- 无图标 → 用系统默认图标（按分类），不允许出现黑块 / 越界读

## 7. 校验流程（安装器必走）

```
下载 → .part 落 staging
  → size 比对（索引）
  → CRC32 比对（索引）
  → SHA-256 比对（索引，新增）
  → lpk_open()：头/索引/重名/越界/大小写冲突
  → lpk_verify_pkg()：整包 CRC
  → lpk_mf_load()：manifest 解析 + 规则校验 + entry/icon 在包内
  → 解包/安装
  → 复核后原子切换
```

任意一步失败 ⇒ **删除临时文件**并报明确原因，不得留下"看起来装好了"的残骸。

## 8. 与 LAPP（`.lua` 头）的关系

LAPP 头 `--@app name=… cat=… icon=<b64>` 是**历史格式**，仍可读；
新包把它转进 `manifest.json`，图标从 base64 解成 `icon.bin`。字段对应：

| LAPP | manifest |
|---|---|
| `name` | `label` |
| `cat` | `category` |
| `desc` | `description` |
| `version` | `version` |
| `build` | `build` |
| `author` | `author` |
| `need=wifi` | `permissions: ["network"]` |
| `icon=<b64>` | `icon.bin` + `"icon": "icon.bin"` |

## 9. 限制

- 条目数 ≤ **8192**
- 单条路径 ≤ **191** 字符
- manifest ≤ **8 KB**
- `app_id` ≤ **63** 字符
- 当前**不压缩**、**不加密**、**不支持增量升级**（升级 = 整包替换，见 `app_system.md`）
