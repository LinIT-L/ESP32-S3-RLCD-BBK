# LinTOS 第三方 Lua 应用投稿平台 · 正式化改造方案（阶段 0 交付）

**日期**：2026-10-31　**状态**：只完成核实与设计，**未修改任何服务器/固件文件，未部署**
**前置**：`docs/LinTOS 第三方Lua应用投稿平台技术现状审计报告.md`（下称"审计报告"）

> 证据分级：`【源码】` `【线上】` `【产物】`（读 ELF/二进制）`【推断】` `【待核实】`

---

## 0 与上一轮审计的差异（本轮实测修正）

上一轮结论基本成立，但有 **3 处必须更正/补充**，直接影响隔离设计：

| # | 上一轮说法 | 本轮实测 | 影响 |
|---|---|---|---|
| 1 | "Lua 可能通过 `os.execute` 执行" | `loslib.c:413` 确实注册了 `execute`，但固件里 `system` 只有 **21 字节的桩**（`nm` 实测 `T system 0x15`）⇒ **无 shell，调用无效** | 风险降级，但仍应从第三方沙箱移除（避免依赖 IDF 未文档化行为）|
| 2 | "`app.db` 写自己的库" | **错**。`app.db.set/set_int` 走 `LNS_NS_COREDB` → NVS 命名空间 **`core_db`**，与系统 `os_core_db_set()` **同一个库**（`page_lua.c:344`）| 🔴 **第三方应用可覆盖系统共享 KV**，这是本轮最需要修的越权点 |
| 3 | 未评估官方应用对标准库的依赖 | 官方 31 个 `.lua` 中，`apps/lua/apps/_ui.lua` **依赖 `require/dofile`** 做模块加载 | 🔴 一刀切禁用 `require/dofile` 会**直接破坏官方应用** |

补充确认：`app.nvs.*` 只写 `luaapp` 命名空间（**安全**）；Lua 状态用自定义分配器 `lua_psram_alloc`（**内存配额有落点**）；未发现 `lua_sethook`（**无运行时限**）。

---

## 1 现状核对（阶段 0 必查项全过）

| 项 | 结果 |
|---|---|
| Git | `main @ c55ad92`（tag v2.0），未提交 34 项 |
| 线上部署 | `/www/wwwroot/bbk.linit.cn/upload/` 5 个 PHP + `config.php`，与本地 `server/upload/` **md5 全一致**【线上】 |
| SQLite | 4 表（`users/codes/sessions/subs`），**全 0 行**【线上】 |
| 商店仓库 | `bbk/index.json`（`ver=18`，updated 2026-10-31 09:19）、`pkgs/` **27 包**、`pkgs_backup/` **14 包**【线上】 |
| `bbk/index.json` 字段 | `ver / site / updated / mirrors[] / pkgs[]{name,label,cat,desc,ver,size,crc32,path,build,type,target}` |
| 固件消费方 | `am_store_net.c`（拉索引）、`am_store_json.c`（解析）、`am_store.c`（安装）、`apppack`（LAPP 头）、`net_client`（下载+CRC） |
| 上传校验现状 | 扩展名 `.lua` + 体积（nginx 4m）+ LAPP 头解析 + `scan_risks()` 关键字 + sha256【源码】 |

---

## 2 改造清单（按文件，不重写）

### 2.1 服务器（`server/upload/`，改动集中于此）

| 文件 | 新增/修改 | 优先级 |
|---|---|---|
| `lib.php` | ① `audit_log()` 写入封装 ② `publish()` 增加**发布前六项校验**（ID/版本/build 单调/CRC/索引合法性/回滚点）③ `unpublish()` ④ `rollback()` ⑤ `scan_lua()` 词法级扫描替换 `scan_risks()` ⑥ `admin_auth()` | P0 |
| `review.php` | 去掉 `?k=`；改 **session + 角色** 鉴权 + CSRF；新增 5 个动作：编辑展示信息 / 下架 / 重新上架 / 历史版本 / 回滚 | P0 |
| `auth.php` | 增加**管理员登录**（独立口令哈希，不用邮箱码）与会话续期 | P0 |
| `index.php` | ① 投稿规范页入口 ② 驳回原因展示 ③ 被驳回后"改完重投" ④ 状态标签补 `已发布/已下架` | P1 |
| `submit.php` | ① 内容校验：必须是 UTF-8 文本、禁止二进制/NUL ② LAPP 必填字段校验 ③ 重复 ID 检查 ④ 限频沿用 | P0 |
| `docs.php`（新增） | 应用开发规范页（从固件真实 API 生成，见 §3.4） | P1 |
| `config.example.php` | 同步新增项（不写真实密钥） | P0 |
| nginx `upload.conf` | 可选：`/upload/tpl/` 静态模板目录、`limit_req` | P2 |

### 2.2 数据库（SQLite，只增不改）

```sql
CREATE TABLE audit_log (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER NOT NULL,            -- 纪元秒
  actor TEXT NOT NULL,            -- 管理员标识（不放口令/token）
  action TEXT NOT NULL,           -- approve/reject/edit/unpublish/publish/rollback/delete
  app TEXT, ver INTEGER,          -- 涉及应用与版本
  before TEXT, after TEXT,        -- 状态快照（JSON，脱敏）
  result TEXT,                    -- ok/fail:原因
  note TEXT
);
CREATE INDEX idx_audit_ts ON audit_log(ts);
-- subs.status 取值扩展（不改表结构，靠应用层约束）：
-- pending / reviewing / approved / rejected / published / delisted
```

> 不新增 `apps` 表：**`index.json` 仍是唯一对外真值**，避免两处状态打架。展示信息覆盖直接写进 `index.json` 的 `label/desc` 字段。

### 2.3 固件（仅在 P3 阶段动，投稿功能本身不需要）

| 文件 | 改动 |
|---|---|
| `components/apps/pages/page_lua.c` | 按信任级别裁剪库；`lua_sethook` 时限；allocator 配额 |
| `components/apps/lua_bridge/luat_lib_store.c` | 第三方禁写 `core_db` |
| `components/apps/lua_bridge/lua_bridge.c` | 能力白名单（HID/USB/NVS）按级别过滤 |
| `components/services/apppack/*` | 可选新增 `perm=` 声明字段（**新增字段旧固件忽略**）|

---

## 3 Lua 权限隔离设计

### 3.1 核心原则：**信任级别由"包从哪来"决定，不由文件里写什么决定**

```
/store/apps/{cat}/*.lua         → OFFICIAL   （内置/随固件打包，可信）
/sdcard/apps/{cat}/*.lua        → THIRD_PARTY（商店下载/用户拷贝，不可信）
```
理由：LAPP 头是**用户可控文本**，`--@app trust=official` 谁都能写；
安装位置由安装流程决定，第三方改不了。`page_lua.c` 已有 `path` 参数，取前缀即可。

### 3.2 分层防护（四层，逐层都不足以单独成立）

| 层 | 做法 | 落点 | 能挡什么 |
|---|---|---|---|
| **L1 库裁剪** | OFFICIAL 保持 `luaL_openlibs` 原样；THIRD_PARTY 在 openlibs 后**主动摘除**：`io.popen/io.tmpfile`、`os.execute/os.exit/os.remove/os.rename`、`package.loadlib` | `page_lua.c` `lua_setup_globals()` 之后 | 低成本去掉明显危险入口；**不构成沙箱** |
| **L2 路径沙箱** | 用 C 包装替换 `io.open` / `os.remove` / `os.rename`：只放行 `<appdata>/<appid>/` 下的读写，其余路径返回错误；`dofile/loadfile` 只允许白名单内置模块 | 新增 `luat_lib_sandbox.c` | 越权读写、删别的应用 |
| **L3 资源配额** | ① `lua_psram_alloc` 内累计字节，超 `2 MB`（第三方）/`8 MB`（官方）返回 NULL ② `lua_sethook` 每 10 万条指令检查截止时间，软 5s / 硬 15s 触发 `luaL_error` | `page_lua.c` | 死循环、内存打爆 |
| **L4 能力白名单** | `app.hid.*` / `app.usb*` / `app.nvs.*` / `app.db.*(core_db)` 仅 OFFICIAL 可用；第三方调用返回 `nil, "permission denied"` | `lua_bridge.c` 注册时按级别过滤 | USB BadUSB、改系统 KV |

### 3.3 关键越权点修复（本轮新发现）

| 越权 | 证据 | 修法 |
|---|---|---|
| 第三方写系统 `core_db` | `luat_lib_store.c:188` → `LNS_NS_COREDB` → `os_core_db_set()`（`page_lua.c:344`）| THIRD_PARTY 强制改写到 `appdb_<appid>` 命名空间；`app.db.get` 同样隔离 |
| 官方 `_ui.lua` 依赖 `require/dofile` | `apps/lua/apps/_ui.lua` | **OFFICIAL 不动**；THIRD_PARTY 的 `require` 只解析内置模块名，不读任意路径 |

### 3.4 兼容性分析（必须先做完才能动固件）

| 官方资产 | 依赖 | 隔离后是否受影响 |
|---|---|---|
| 31 个 `.lua`（含 `_ui.lua`）| `require/dofile` | ❌ 不受影响（OFFICIAL 走原样路径）|
| 地图类（`map.lua` 等）| `app.net.*` | ❌ 不受影响（网络属中风险，不封）|
| 阅读器/白板等 C 应用 | 不经过 Lua | ❌ 无关 |
| 商店 27 个包 | `index.json` 字段 | ❌ 协议不动 |
| 现有第三方包（若有）| 未知 | ⚠ **需先扫描**：`/sdcard/apps` 下若已有用 `io.*` 的包，收紧后会坏 → 先出清单再决定"白名单放行"还是"通知作者改" |

### 3.5 明确边界（写在文档里，避免以后误判）

- **静态扫描不是沙箱**，它只能减少到达审核台的可疑代码量；真正的边界在 L2/L3/L4。
- 不承诺能识别所有恶意 Lua；**人工审核仍是最后一道**。
- `system()` 是桩不代表以后永远是桩（IDF 升级可能变），所以 L1 仍要摘 `os.execute`。

---

## 4 服务器安全加固清单

| # | 项 | 现状 | 目标 | 风险 |
|---|---|---|---|---|
| S1 | 审核鉴权 | `review.php?k=<key>`（会进 access log/Referer）| 改 **POST + session + CSRF**；`k` 仅用于首次绑定管理员 | 中（改鉴权逻辑）|
| S2 | 管理员口令 | 无 | `password_hash` 存 `users.role='admin'`，独立登录入口 | 低 |
| S3 | CSRF | 已有 `csrf()`（用户侧）| 管理员所有写操作强制校验 | 低 |
| S4 | 上传内容 | 只看扩展名 + 关键字 | 校验 UTF-8 文本、无 NUL、LAPP 必填项；大小沿用 512 KB | 低 |
| S5 | 路径安全 | 落盘名由服务端生成哈希 | 保持；文件名一律不使用用户输入 | 低 |
| S6 | 并发写 | SQLite 默认 | `PRAGMA journal_mode=WAL` + `busy_timeout`；发布用文件锁 | 中 |
| S7 | 发布原子性 | `.tmp → rename` 已有 | 增加"发布前备份 index.json 到 `backup/`，失败自动还原" | 低 |
| S8 | 错误泄露 | 需复查 | 生产关 `display_errors`，错误只进日志 | 低 |
| S9 | 下架 | 无 | `unpublish()` 从 index 移除条目 + 文件移入 `pkgs_offline/` | 低 |
| S10 | 回滚 | 无界面 | 从 `pkgs_backup/` 选版本 → 重新发布（**不覆盖备份**）| 中 |

---

## 5 发布系统兼容约束（冻结协议）

以下**一律不改**，只在其外层加校验：

```
index.json:  ver / site / updated / mirrors[] / pkgs[]{name,label,cat,desc,ver,size,crc32,path,build,type,target}
LAPP 头:     --@app name/cat/desc/version/author/date/build/need
文件路径:    /bbk/pkgs/<name>.lua（相对 path 不变）
校验:        CRC32 + size（设备侧硬闸门）
版本:        build=YYYYMMDDHHMM 字符串比较，单调递增
```
新增字段（如 `perm`）只能**加在 LAPP 头**——旧固件按注释忽略，天然兼容；**不得往 index.json 加必需字段**（`am_store_json` 对未知 key 的处理需先验证，标【待核实】）。

**发布前六项校验**（`lib.php publish()` 内，缺一不发布）：
① 文件 sha256/CRC 与提交记录一致 ② LAPP 头必填字段齐全 ③ `name` 未与已有包冲突（除自身升级）④ `ver` 递增 ⑤ `build` 严格大于上一版 ⑥ 生成的 index.json 能被 `json_decode` 且 `pkgs` 数量未异常减少。

---

## 6 测试计划（不靠"语法检查"结案）

| 组 | 用例 | 方式 |
|---|---|---|
| 投稿 | 正常 / 缺元数据 / 非 Lua 二进制 / 超大 / 重复 ID / 恶意文件名（`../../`）/ 重复提交 | PHP CLI + curl 打本地副本 |
| 审核 | 通过 / 驳回 / 改展示信息 / 未授权 / 无 CSRF token | curl + session |
| 发布 | 上架 / 更新 / 下架 / 回滚 / index 损坏恢复 / 发布中断 | 临时目录复制一份 `bbk/` 演练，**不碰生产** |
| 固件 | 官方 31 应用回归 / 第三方包 / 旧包 / 非法包 / 隔离生效 / 超时生效 | esp32sim + 主机侧；真机另行申请 |

> 所有服务器测试在 `/tmp/bbk_test/` 副本上做，**不写生产目录、不动生产 DB**。

---

## 7 分阶段实施与回滚

| 阶段 | 内容 | 回滚 |
|---|---|---|
| **P1** | 服务器：`audit_log` 表 + 管理端 5 个动作 + 鉴权改 session/CSRF（本地改，先不部署）| 备份 `app.db` + `index.json`；回滚=换回旧 5 个 PHP |
| **P2** | 扫描升级 `scan_lua()`（词法级）+ 投稿页规范页 + 驳回重投 | 同上，`scan_lua` 保留旧 `scan_risks` 开关 |
| **P3** | 固件 Lua 隔离（L1/L3 先行，L2/L4 随后）| `page_lua.c` 保留 `LUA_SANDBOX=0` 编译开关，一键回原行为 |
| **P4** | 真机验证 + 上线 | 固件回刷上一版 |

**P3 的硬前提**：先跑一遍"官方 31 应用 + 商店 27 包"的 API 使用清单，确认没有用到要封的接口。**这个清单没出来之前不动固件。**

---

## 8 本轮"未做"的（避免误会）

- ❌ 没有改任何 PHP、没有建表、没有动 `index.json`/`pkgs`、没有部署、没有重启服务。
- ❌ 没有改固件源码，没有烧录。
- ⏳ 下面这些要你确认后才做：① 是否按 P1 先做服务器（本地改、不部署）② 管理员是否用独立口令（而非邮箱码）③ 第三方应用数据目录定在哪（建议 `/sdcard/apps/.data/<appid>/`）。

---

## 9 待核实（我拿不到或需你拍板）

1. `am_store_json.c` 对 **index.json 未知字段**的容错程度（决定能不能加字段）——需读解析器逐行确认。
2. 现网 `/sdcard/apps`、`/store/apps` 下是否已有第三方包用了 `io.*`（决定收紧后要不要放行）。
3. 官方 27 个商店包里对 `app.db/core_db` 的实际使用面（grep 只能覆盖仓库内的 31 个源文件，线上包需下载比对）。
4. 商家是否要求"投稿即公开可见进度"，还是只给作者本人看（影响列表查询范围）。
5. 管理员账号如何初始化（首个 admin 由 CLI 建，还是 CLI 生成一次性口令）。
