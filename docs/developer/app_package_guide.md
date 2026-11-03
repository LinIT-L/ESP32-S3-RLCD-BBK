# 第三方应用打包指南（LinTOS 2.0）

> 你只需要交**一个 `.lpk` 文件**。图标、元数据、程序、资源全在里面，
> 不用单独传图标、不用传多个资源文件、不用在服务器上手工拼装。

---

## 1. 目录结构

```
myapp/
├── manifest.json        必须
├── icon.bin             建议（64×64 1bpp，正好 512 字节）
├── app/
│   └── entry.lua        必须（runtime=lua 时）
├── assets/              可选：应用自有资源
└── resources/           可选：resource 包的资源
```

## 2. manifest.json

```json
{
  "schema_version": 1,
  "app_id": "com.yourname.myapp",
  "name": "MyApp",
  "label": "我的应用",
  "description": "一句话说明",
  "category": "软件",
  "package_type": "app",
  "runtime": "lua",
  "entry": "app/entry.lua",
  "version": "1.0.0",
  "build": "202611011000",
  "author": "YourName",
  "icon": "icon.bin",
  "dependencies": [],
  "permissions": ["storage.read", "network"],
  "data_policy": "preserve",
  "min_firmware": "2.0.0"
}
```

### 字段速查

| 字段 | 必填 | 说明 |
|---|---|---|
| `schema_version` | ✅ | 固定 `1` |
| `app_id` | ✅ | `com.<你>.<应用>`，全小写，至少两段，**上线后永不改** |
| `label` | ✅ | 显示名（可中文） |
| `version` | ✅ | 语义化版本，如 `1.0.0` |
| `package_type` | ✅ | `app`（可启动）或 `resource`（资源） |
| `runtime` | app 必填 | `lua` / `native` / `engine` |
| `entry` | app 必填 | 包内相对路径，**必须真实存在** |
| `icon` | 建议 | 包内相对路径；声明了就必须存在且 512 字节 |
| `build` | 建议 | `YYYYMMDDHHMM`，升级判定用它（比 `version` 可靠） |
| `category` | 建议 | `软件`/`游戏`/`学习`/`运维`/`系统`/`壁纸`（也接受 `software` 等稳定 ID） |
| `permissions` | 建议 | `storage.read` `storage.write` `network` `audio.record` `audio.play` `input` `display` |
| `dependencies` | 可选 | `[{"id":"com.lintos.engine.gb","version":">=1.0.0"}]` |

> ⚠ `install_target` 只是**建议**，最终装哪儿由固件按包类型决定 —— 不能靠它写系统目录。

## 3. 图标

- 64×64 像素，**1bpp**，MSB first，**1 = 黑**
- 无文件头，**正好 512 字节**
- 从旧的 LAPP 头迁移：把 `--@app icon=<base64>` 解码出来就是 `icon.bin`，零转换

自检：

```bash
python3 - <<'PY'
d = open("icon.bin","rb").read()
print("字节数", len(d), "OK" if len(d)==512 else "✗ 必须 512")
PY
```

## 4. 打包

```bash
python3 tools/pack_lpk.py --app myapp/ -o myapp.lpk --list
```

工具会自动：

1. 校验 manifest（schema / app_id / 类型 / 运行时 / 入口 / 图标尺寸）
2. 校验入口与图标**真的在目录里**
3. 检查重复 `app_id`（对照 `store/index.json`）
4. 打包（无压缩、512 对齐）
5. 输出 `size` / `CRC32` / `SHA-256` / **可直接贴进商店索引的记录**

输出示例：

```
✅ 应用包打包完成: myapp.lpk
   app_id      : com.lintos.recorder
   label       : 录音机  (Recorder)
   package_type: app
   runtime     : lua   entry=app/entry.lua
   version     : 2   build=202610310919
   文件数      : 3
   大小        : 46080 字节
   CRC32       : d3e4e434
   SHA-256     : 7b95ae7c51289020…
   索引记录    : {"app_id": "com.lintos.recorder", …}
```

## 5. 验证

```bash
# 结构 + 逐条 CRC
python3 tools/pack_lpk.py --inspect myapp.lpk --verify

# manifest + 设备端同一套规则（10 项用例，含坏包）
python3 tests/lpk/lpk_mf_cases.py
```

失败会直接告诉你哪条规则不过，例如：

```
⛔ manifest 校验失败: icon 必须正好 512 字节（64×64 1bpp），实际 256
```

## 6. 常见错误

| 报错 | 原因 | 处理 |
|---|---|---|
| `缺少 manifest.json` | 目录里没有 | 补上，名字必须精确 |
| `app_id 非法` | 有大写 / 没点 / 含空格 | 改 `com.xxx.yyy` |
| `entry 指向的文件不在应用目录里` | 路径写错或文件漏放 | 核对相对路径 |
| `icon 必须正好 512 字节` | 尺寸不对 | 64×64 1bpp 重新导出 |
| `resource 包不应有 entry` | 资源包写了入口 | 删掉 `entry` 和 `runtime` |
| `重复资源路径` | 有仅大小写不同的两个文件 | 改名（设备端无法区分） |

## 7. 提交

1. 把 `.lpk` 放到服务器 `packages/<app_id>.lpk`
2. 把工具输出的**索引记录**加进 `index.json` 的 `packages[]`
3. 确认 `size` / `crc32` / `sha256` 与文件一致（不要手算）

> 设备端会依次校验 size → CRC32 → SHA-256 → 容器结构 → manifest → entry/icon 存在性，
> 任一不过即拒绝安装并删除临时文件。
