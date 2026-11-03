# 投稿平台与 Lua 第三方安全模型

> 覆盖：投稿站架构、审核/发布/下架/回滚流程、管理员操作手册、第三方 Lua 权限表、
> 自动化测试方法、故障恢复。**与源码同源**，改协议或改权限时同步改本文件。
>
> 源码入口：
> `server/upload/`（`lib.php` / `lib_scan.php` / `review.php` / `submit.php` / `admin_cli.php`）
> `components/apps/lua_bridge/lua_sandbox.c`、`lua_path_guard.c`、`lua_quota.c`
> `components/services/apppack/apppack.c`、`components/apps/pages/am_store_json.h`

---

## 1. 一张图看数据流

```
开发者                投稿站(PHP+SQLite)                    商店(静态目录)        固件
  │                        │                                    │                  │
  │ POST /submit.php       │                                    │                  │
  ├───────────────────────>│ 校验(大小/UTF-8/NUL/头部/图标/版本) │                  │
  │                        │ 落盘 data/pkgs/<sha256>.lua        │                  │
  │                        │ 入库 subs(status=pending)          │                  │
  │                        │ 词法扫描 → scan/verdict            │                  │
  │                        │                                    │                  │
  │   管理员 /admin_login  │                                    │                  │
  │                        │ review.php 通过 ── store_publish ─>│ pkgs/<name>.lua  │
  │                        │  10 项前置校验 + flock + 原子写    │ index.json(ver+1)│
  │                        │  versions 记一条 + audit_log       │                  │
  │                        │                                    │  GET index.json  │
  │                        │                                    │<─────────────────┤
  │                        │                                    │  GET pkgs/x.lua  │
  │                        │                                    │<─────────────────┤
  │                        │                                    │  校验 crc32+size │
  │                        │                                    │  落地 LAPP → 运行│
```

固件侧不认任何新协议：`index.json` 的字段集合、`pkgs/<name>.lua` 路径写法、
`crc32`+`size` 校验、`build`（12 位定长数字，空则退回比 `ver`）全部保持原样。

---

## 2. 目录与权限（生产）

```
/www/wwwroot/bbk.linit.cn/
├── bbk/                       商店（对公网可读）
│   ├── index.json             ← 唯一入口，mirrors[] + pkgs[]
│   ├── pkgs/<name>.lua        应用包（应用 ID = 文件名主干）
│   └── pkgs_backup/           历史包（发布时自动备份，不对外列目录）
└── upload/                    投稿站
    ├── config.php             ★ 只读、不进 git
    ├── lib.php lib_scan.php   公共库（发布事务、扫描器）
    ├── index.php spec.php     用户页 / 开发规范
    ├── auth.php submit.php    用户登录 / 投稿
    ├── admin_cli.php          ★ 只能 CLI 跑：建/重置管理员
    ├── admin_login.php        管理员登录、首登改密、退出
    ├── review.php             审核后台（会话+角色+CSRF）
    ├── download.php           下载原始投稿包（仅管理员）
    └── data/                  ★ 必须 nginx deny
        ├── app.db             SQLite（WAL）
        ├── pkgs/<sha256>.lua  投稿包原始存储（审核/下载/重发都靠它）
        ├── tmp/               回滚暂存（**不要**和 pkgs 混用，见 §7）
        └── backup/index-*.json  索引快照，保留最近 20 份
```

nginx 只需两条：`/upload/data/` deny；**不要**给 `/upload/` 加 `location ^~`
（会把 .php 当静态文件下发源码）。

---

## 3. 管理员

### 3.1 初始化（只能命令行）

```bash
cd /www/wwwroot/bbk.linit.cn/upload
php admin_cli.php init boss      # 生成 24 位随机口令，只显示一次
php admin_cli.php list           # 看管理员（不含任何口令信息）
php admin_cli.php passwd boss    # 忘了就重置，下次登录强制改密
```

规则：网页上没有任何注册入口；`init` 在已有管理员时**拒绝执行**（重跑不会重置账户）；
口令用 `password_hash()`（bcrypt）存；首次登录强制改成 ≥10 位的新口令。

### 3.2 登录与限频

- 连续 5 次错 → 该账号锁 15 分钟；同一 IP 10 分钟内 10 次失败 → 该 IP 停 10 分钟。
- 登录成功 `session_regenerate_id`；空闲 30 分钟（`admin_idle_ttl`）失效。
- 提示语永远只有"用户名或密码不对"，不区分账号是否存在。

### 3.3 审核后台（`review.php`）

| 动作 | 说明 | 状态变化 |
|---|---|---|
| 开始审核 | 认领，避免两个人同时处理 | pending → reviewing |
| 通过并发布 | 走完整发布事务，写 `index.json` | → published |
| 通过但暂不发布 | 只标记审核通过 | → approved |
| 驳回 | **必须填原因**，会展示给投稿者 | → rejected |
| 保存展示信息 | 只改商店怎么显示（label/cat/desc），不动包、不动版本 | — |
| 下架 | 从索引移除条目，**文件与历史全部保留** | published → unpublished |
| 重新上架 | 用当前记录的版本文件重建索引条目 | → published |
| 回滚到某一版 | 旧内容 + **新版本号**重发（见下） | → published |

页面还会显示：包内 LAPP 头（与网页展示逐项对照，不一致会告警）、词法扫描结果、
前 60 行代码、版本历史、这条应用的全部操作记录、原始包下载。

### 3.4 为什么回滚要"新版本号"

固件只在 `index.json` 的 `build`（或 `ver`）变大时才提示更新。把版本号直接退回去，
已经装了坏版本的用户**永远收不到修复**。所以回滚 = 用旧内容 + 新版本号 + 新 `build`
重新发布，`versions` 里同时保留原版本（内容在 `pkgs_backup/`）。

---

## 4. 发布事务（`store_publish`）

顺序固定，任何一步失败都回到起点：

1. 参数与文件（ID/文件名/ver/size）
2. `sha256` 与 `crc32` **重算**并与投稿记录比对
3. LAPP 头存在；包内 `name` 与审核记录的应用 ID 一致；图标 512 字节
4. `build` 格式（12 位）与单调性（比包内 build，不是索引 build）
5. 取 `flock`（同一时刻只允许一个发布）
6. 读索引用 **strict** 版：解析不了就**拒写**（不能拿空骨架覆盖整店）
7. 版本必须大于商店当前版本
8. 备份：`data/backup/index-<ts>.json` + 旧包 → `pkgs_backup/<name>.<ts>`
9. 原子写包：`.tmp` → `rename`
10. 原子写索引：`.tmp` → 自检 JSON 与条目数 → `rename`
11. 发布后自检：条目存在、ver/size/crc32 对得上、条目数没减少、文件真实存在
12. 记 `versions` 与 `audit_log`

第 9 步之后任何失败 → 用第 8 步的备份恢复索引与旧包。

---

## 5. 第三方 Lua 权限表

信任级**只由安装位置决定**（`/store/apps/` = OFFICIAL，其余 = THIRD_PARTY），
因为 LAPP 头是用户可控文本，写 `--@app trust=official` 谁都会。

| 能力 | 官方 | 第三方 | 实现位置 |
|---|---|---|---|
| `os.execute` / `os.exit` / `os.tmpname` | ✅ | ❌ 移除 | `lua_sandbox.c` L1 |
| `io.popen` / `io.tmpfile` | ✅ | ❌ 移除 | L1 |
| `loadfile` / `dofile` / `package.loadlib` | ✅ | ❌ 移除 | L1 |
| `require` 加载磁盘模块 | ✅ | ❌ 只有已注册 C 库（searchers 只剩拒绝桩） | L1b |
| `io.open` / `os.remove` / `os.rename` | 全盘 | 只能落 `数据目录`，绝对路径含 `..` 直接拒 | L2 |
| `app.fs.*` / `app.img.load` | 全盘 | 同上（它们直接 fopen，是 `io.open` 的旁路） | L2b |
| `app.hid.*`（USB 键鼠/BadUSB） | ✅ | ❌ `permission denied` | L4 |
| `app.wiki.open`（可传任意目录） | ✅ | ❌ | L4 |
| `app.rec.start`（麦克风） | ✅ | ❌ | L4 |
| `app.net.*` / `app.wifi.*` / `app.espnow` | ✅ | ✅（联网是正当需求） | — |
| `app.db.*` / `app.nvs.*` | `core_db` / `luaapp` | `appdb_<8hex>`（按 appid 哈希，14 字符） | `lua_sandbox_ns()` |
| Lua 内存 | 不限 | 4 MB（只算 Lua 自己的分配） | `lua_quota.c` |
| 单次运行时长 | 不限 | 10 s（count hook + 阻塞 API 夹紧） | L3 |

**数据目录**：`/sdcard/apps/.data/<appid>/`，首次启动自动创建。
第三方应用**相对路径**自动落在这里，这是推荐写法。

**两条硬事实（不是设计选择，是文件系统语义）**：

- FatFs 把**反斜杠也当分隔符**（`ff.c:54 IsSeparator(c) = '/' || '\\'`），
  所以 `..\..\x` 与 `../../x` 等价 —— 路径守卫两者都拒。
- `app.db.get/del` 曾经直连 `core_db`（写走新命名空间、读写还是系统库），
  第三方能**删系统键**。现已三条路径统一走 `lua_sandbox_ns("core_db")`。

---

## 6. 静态扫描（`scan_lua`）

词法级，不是 `strpos`：先切 token（正确跳过 `--`、`--[[ ]]`、`[[ ]]`、`".."`、`'..'`），
再在 token 流上匹配 `标识符(.标识符)*`。因此：

- 注释/字符串里的 `app.fs.remove` 不误报（字符串里出现会单独提示"可能在拼名字"）
- `app.fs . remove`（带空格）照样命中
- `app["fs"]["write"]` 命中"动态下标访问 app"
- `load(变量)` 命中"动态代码，静态分析不了"
- `while true` 单独看关键字对

结论分 `high / med / low / none`，只是**审核辅助**：它能被绕过（运行时沙箱才是边界），
也不承诺识别所有恶意代码。

---

## 7. 故障恢复

| 现象 | 处置 |
|---|---|
| 发布到一半断电 | 索引是 `.tmp` → `rename`，要么旧要么新；包同理。重启后重发一次即可 |
| `index.json` 坏了（JSON 解析失败） | 写路径全部**拒绝**（strict 检查）。从 `data/backup/index-*.json` 挑一份拷回去 |
| 上架错了想退回 | 后台「回滚到某一版」（新版本号重发旧内容） |
| 想彻底消失 | 「下架」（条目移除、文件保留） |
| `versions` 里的 `src` 指向的文件没了 | 回滚会先 `crc32` 校验并报错，不会静默发坏包 |
| 管理员忘了口令 | `php admin_cli.php passwd <用户名>` |
| 站点挂了但商店要正常 | 商店是纯静态目录，投稿站宕机不影响下载 |

**回滚暂存目录为什么必须是 `data/tmp/`**：`data/pkgs/<sha>.lua` 是投稿包的正式存储，
下载/重审都读它；回滚结束时会把暂存文件 `unlink`，若共用目录就会把原始投稿包一起删掉
（实测表现：审核页下载 404）。

**部署后必须让 PHP 重新加载**：如果开了 opcache，改完 `lib.php` 不重启/不 `opcache_reset()`
会继续跑旧代码（本轮就踩到：测试全绿、生产行为旧）。宝塔里点"重载配置"或重启 php-fpm 即可。

---

## 8. 自动化测试

```bash
# 1) 在服务器上建独立副本（不碰生产）
mkdir -p /tmp/bbk_test && cd /tmp/bbk_test
tar xzf upload_new.tgz && mv upload web
mkdir -p data/pkgs data/backup bbk/pkgs bbk/pkgs_backup
# web/config.php 里 store_dir/data_dir 指向 /tmp/bbk_test/...

# 2) 逻辑测试（直接调生产函数，含并发发布、索引损坏、回滚）
php t_cli.php          # 71 PASS

# 3) HTTP 测试（真实请求，含鉴权/CSRF/隐私/发布/下架/回滚/下载）
php -d opcache.enable=0 -d opcache.enable_cli=0 \
    -S 127.0.0.1:8899 -t /tmp/bbk_test/web &
php t_http.php         # 76 PASS
```

固件侧主机测试（不需要设备）：

```bash
cd <repo>
# 路径守卫（生产源码）
clang -O2 -std=c11 -Wall -Wextra -I components/apps/lua_bridge \
      tests/lua_guard/lua_guard_test.c components/apps/lua_bridge/lua_path_guard.c -o /tmp/lg && /tmp/lg
# 配额账本（生产源码）
clang -O2 -std=c11 -Wall -Wextra -I components/apps/lua_bridge \
      tests/lua_quota/quota_test.c components/apps/lua_bridge/lua_quota.c -o /tmp/qt && /tmp/qt
# 沙箱（真实 Lua 引擎 + 桩平台层，编译生产 lua_sandbox.c）
#   见 tests/lua_sandbox/sandbox_test.c 顶部注释里的完整命令行
```

---

## 9. 已知限制

1. **静态扫描不是沙箱**，动态拼的代码只能靠人工看。
2. **`lua_sethook` 只在 Lua 字节码边界打断**，C 里的一次阻塞调用它拦不住 ——
   所以 `app.util.sleep` / `app.net.http_get` / `app.net.port` 的时长会被夹到剩余预算内；
   其它 C API 若有长阻塞（如大文件读）仍可能超出软预算。
3. 第三方应用**读**也只限自己的数据目录：想看用户放在 `/sdcard/` 的文件，目前没有授权机制。
4. 4 MB / 10 s 是**未在真机上实测**的量级值，改动依据见 `lua_sandbox.c` 顶部。
5. 应用 ID 碰撞：哈希型 appid（纯中文名）是 8 位十六进制，理论上可与另一个恰好同名的
   ASCII 应用撞同一个 ID（概率 2^-32 量级，且只影响同一台设备自己的两个应用）。
