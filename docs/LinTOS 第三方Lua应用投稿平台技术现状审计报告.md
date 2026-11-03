# LinTOS 第三方 Lua 应用投稿平台 · 技术现状审计报告

**日期**：2026-10-31　**性质**：只读审计（未修改任何代码 / 配置 / 数据库 / 服务；未重启、未部署、未提交）
**审计范围**：固件源码（本地 `c55ad92`）+ 线上服务器（120.24.110.41:2546，只读 SSH）

> 证据分级：`【源码】` `【线上】`（服务器实测）`【文档】` `【推断】` `【未确认】`

---

## 0 最重要的一条先说

**投稿平台不是"待建"，而是"已经建成并部署在跑"** —— `server/upload/` 里 5 个 PHP + 1 个配置模板，
线上 `https://bbk.linit.cn/upload/` 返回 **200**，5 个文件与本地副本 **md5 完全一致**【线上】。
但线上数据库 `users / codes / subs` **全部 0 行**【线上】⇒ 已就绪、尚无真实投稿。

| 结论项 | 判定 |
|---|---|
| 服务器能否直接承载 | ✅ **能，已经在承载** |
| 可否复用现有商店后端 | ✅ 本来就是同一个后端（发布即写 `bbk/index.json` + `bbk/pkgs/`）|
| 是否需新增表 | ❌ 核心不需要；建议补 1 张审计日志表 |
| 是否需改固件 | ❌ 上线投稿**不需要**；要加强安全才需要 |
| 审核通过能否自动上架 | ✅ **已实现**（复制包→备份→写 index→ver+1）|
| 最大缺口 | 管理后台功能不全（改信息/下架/历史版本/操作日志）+ Lua 无沙箱 |

---

## A 固件现状

### A1 Lua 应用体系

| 项 | 事实 | 证据 |
|---|---|---|
| 应用形态 | **单个 `.lua` 文件**（LAPP 包），代码 + 图标 + 元信息同文件 | `components/services/apppack/include/apppack.h` |
| 元信息 | 头部注释 `--@app key=value`，**静态可解析**（不执行 Lua） | 同上，`apppack_parse()` |
| 字段 | `name` / `cat` / `desc` / `version` / `author` / `date` / `build` / `need=wifi` + `icon` | 同上（`apppack_meta_t`）|
| 图标 | 内嵌 **512 B = 64×64 1bpp**，MSB first，1=黑 | `APPPACK_ICON_BYTES 512` |
| 版本判定 | `build` = `YYYYMMDDHHMM` 定长数字，设备端 **strcmp 比较**（字典序即时间序）；缺省回退 `version` | `apppack.h` 注释 2026-10-30 规则 |
| 能力声明 | 只有一条 `need=wifi`（启动前门禁：未联网则提示+复用系统连 Wi-Fi） | `apppack.h` / `am_lua.c` |
| 第三方开发支持 | ✅ 有 `tools/pack_app.py`、`tools/new_app.py` 模板 | `tools/` |
| 沙箱 | ❌ **无**：`luaL_openlibs(s_L)` 打开全部标准库 | `components/apps/pages/page_lua.c:383` |

### A2 应用商店

| 项 | 事实 | 证据 |
|---|---|---|
| 实现位置 | `components/apps/pages/am_store.c`（列表/安装/卸载）、`am_store_net.c`（网络）、`am_store_json.c`（解析）、`components/services/net_client`（HTTP） | 源码目录 |
| 列表来源 | 拉 `https://bbk.linit.cn/bbk/index.json`，失败按序回退备源 | `components/apps/os_sources.c` 的 `S[]` |
| 镜像回退 | 编译期 3 个源（bbk.linit.cn → jsDelivr → GitHub raw），运行期用 `index.json.mirrors[]` 覆盖/扩展 | 同上；`am_store_net.c:121-160` |
| 索引缓存 | 落 `/store/index.json.tmp` → rename（防半截文件） | `am_store_net.c:187` |
| 下载 | 流式写 `<目标>.part`，校验通过才 `.part → .bak → 正式` 替换 | `components/services/net_client/netc_fs_replace.c` |
| 完整性校验 | **CRC32 + 体积**双闸门（`am_store_json` 里 `crc==0` 判非法；设备侧替换前硬闸门） | `am_store_net.c` / `net_client.c` |
| **签名校验** | ❌ 无。只有 CRC32，且 CRC 由服务器下发 ⇒ 服务器被攻破即可推任意包 | 全链路无公钥 |
| 兼容性判断 | 无显式字段；靠"索引里不出现不兼容包"隐式保证 | `am_store_json.c` |
| 安装位置 | 内置 `/store/apps/{cat}` 或 TF `/sdcard/apps/{cat}`，`.new`→`.old`→rename 带回滚 | `am_store.c:687-713` |
| 上下架/回滚 | 服务器侧靠 `index.json` 删条目 + `pkgs_backup/` 留档；固件只认索引 | `server/upload/lib.php` |

### A3 运行时与安全边界

- Lua 版本：随固件内置 `components/libs/lua`（Lua 5.x 源码内置，非外挂）。
- **所有标准库可用**（`io` / `os` / `loadfile` / `dofile`）⇒ 恶意投稿脚本可读写 `/sdcard`、`/store` 下文件，也能删别的应用。**这是当前最大的安全事实**。
- 文件系统无 per-app 隔离；应用能访问整机可见路径。
- 崩溃隔离：Lua 侧 pcall 有限，任务级无独立看门狗（未逐条确认，【未确认】）。
- 内存限制：无 per-app 配额（未发现限制代码，【推断】无）。

---

## B 服务器现状【线上】

| 项 | 实测值 |
|---|---|
| OS / 内核 | Alibaba Cloud Linux 4 / `6.6.102-8.alnx4.x86_64` |
| CPU / 内存 | 2 vCPU / **1.67 GB（已用 622 MB，可用 1.05 GB）** |
| 磁盘 | 40 GB，已用 13 GB（**35%**，余 25 GB） |
| Web | **nginx 1.30.6**（宝塔 vhost，extension 分片） |
| PHP | **8.2.34 (cli/ftp)**，扩展含 `pdo_sqlite` `sqlite3` `gd` `mbstring` `curl` `openssl` |
| 站点 | `bbk.linit.cn`（商店+投稿）、`linit.cn`（WordPress）、`bbk.linit.cn/bbk`、`tag.linit.cn`、`default` |
| 投稿站目录 | `/www/wwwroot/bbk.linit.cn/upload/`（www:www） |
| 数据 | `upload/data/app.db`（SQLite 45 KB）、`upload/data/pkgs/`（空） |
| 商店仓库 | `/www/wwwroot/bbk.linit.cn/bbk/`：`index.json`（ver=18，updated **2026-10-31 09:19**）、`pkgs/` **27 个包**、`pkgs_backup/` **14 个** |
| 访问实测 | `/upload/` **200**、`/upload/review.php` **403**（无 k）、`/upload/data/app.db` **404**、`/upload/config.php` **0 字节 text/html**（被 PHP 执行、**未泄露源码**）|

### B1 数据库结构（SQLite，4 表）

```
users   (id, email UNIQUE, created, verified, last_login)
codes   (id, email, code, expires, used)            idx_codes_email
sessions(token PK, uid, expires)                    -- 预留
subs    (id, uid, name, label, cat, descr, ver, fname, size, crc, sha,
         risks, status, created, reviewed, note, ip, pub_ver)   idx_subs_uid/status
```
行数：`users=0`、`codes=0`、`subs=0`、`sessions=0`【线上】。
`subs` 已含 `sha`(sha256)、`crc`、`risks`(JSON)、`status(pending/approved/rejected)`、`note`(驳回原因)、`pub_ver`(发布版本)。

### B2 已有接口（全部在 `upload/*.php`，无 REST 前缀）

| 文件 | 作用 | 鉴权 |
|---|---|---|
| `index.php` | 邮箱登录 + 上传表单（选文件后自动读 LAPP 头、画图标预览）+ 我的提交 | PHP session |
| `auth.php` | 发验证码 / 校验 / 退出 | 邮箱验证码（10 分钟、一次性） |
| `submit.php` | 收包：扩展名 → 体积 → 解析 LAPP → **危险 API 扫描** → sha256 → 落盘 → 入库 → 邮件 | session |
| `review.php` | 审核面板：列表 / 详情 / 代码预览 / 通过 / 驳回（填意见） | **URL 里的 `review_key`**（`hash_equals` 比较）|
| `lib.php` | SQLite、LAPP 解析、校验、风险扫描、SMTP、商店读写、限频 | — |

**发布实现**【源码 `lib.php:339-398】：`copy` 包到 `pkgs/` → 旧包 `copy` 到 `pkgs_backup/<name>.<时间戳>` → 改 `index.json` 的 `pkgs[].ver` 与整体 `ver` → 写 `.tmp` → `rename` 原子替换。

### B3 上传安全现状【源码】

- 只接受扩展名 `.lua`（`submit.php:19`），有体积上限（nginx `client_max_body_size 4m`）。
- 内容校验：解析 LAPP 头（纯字符串，**不执行 Lua**）+ `scan_risks()` 字符串命中 + sha256。
- `scan_risks` 规则：15 条桥 API 关键字 + 硬编码 `IP:端口` 正则 + **标准库逃逸** `io.open|io.popen|os.execute|loadfile|dofile`（标 high）。
- 未发现任何 Lua 解释器调用 ⇒ **服务器不会执行投稿代码**，符合你的硬要求。

---

## C 固件 ↔ 服务器 数据流

```
[设备] am_store_net.c
   │  ① GET  https://bbk.linit.cn/bbk/index.json      （失败依次回退 3 个镜像）
   ▼
[服务器] /www/wwwroot/bbk.linit.cn/bbk/index.json     ←── lib.php: publish()
   │  ② 解析 pkgs[] {name,label,cat,desc,ver,size,crc32,build,path,…}
   ▼
[设备] am_store_json.c  → 列表/详情（本地元数据 + 索引）
   │  ③ GET  mirrors[i] + "/" + path      （如 /pkgs/英雄坛说.lua）
   ▼
[服务器] /www/wwwroot/bbk.linit.cn/bbk/pkgs/*.lua     ←── publish 时 copy，旧版进 pkgs_backup/
   │  ④ 流式落盘 /sdcard/store_pkg/<name>.lua.part
   │  ⑤ CRC32+size 闸门 → .part→.bak→正式（netc_fs_replace）
   ▼
[设备] am_store.c: am_store_auto_install() → /store/apps/{cat} 或 /sdcard/apps/{cat}（.new→.old→rename）
   │  ⑥ 应用列表重扫 → 主菜单出现

[投稿侧·独立链路，与设备无关]
用户浏览器 → /upload/index.php（登录）→ submit.php → data/pkgs/<hash>.lua + SQLite(subs)
                                                    ↓
                                管理员 review.php（review_key）→ 通过 → lib.php publish()
                                                    ↓
                                    bbk/pkgs/*.lua + bbk/index.json(ver+1)
```
**关键点**：投稿站与设备**没有直接耦合**——审核通过写进同一个商店目录，设备下次拉 `index.json` 就看到。**不需要改固件。**

---

## D 投稿平台可行性（逐条对照你的需求）

### 用户端

| 需求 | 现状 |
|---|---|
| 查看投稿规范 | ⚠ 未确认页面上是否有规范正文（`index.php` 未逐行读，【未确认】）|
| 上传 Lua 应用 | ✅ `submit.php` |
| 填写名称/介绍/作者/版本 | ✅ 从 LAPP 头读 + 表单补 |
| 上传图标/素材 | ⚠ **图标在 LAPP 内嵌**（64×64 1bpp），无独立素材上传 |
| 上传完整包 | ✅ 单文件 `.lua`（不是 ZIP）|
| 我的投稿 / 审核状态 | ✅ `index.php` 列表 + `subs.status` |
| 未通过原因 | ✅ `subs.note` 有字段（页面是否显示【未确认】）|
| 上架结果 | ⚠ `subs.pub_ver` 有记录，展示待确认 |

### 管理端

| 需求 | 现状 |
|---|---|
| 待审列表 / 详情 / 代码预览 | ✅ `review.php` |
| 下载人工测试 | ✅ 可从 `data/pkgs/` 取 |
| 通过 / 驳回 / 审核意见 | ✅ |
| **在线改展示信息** | ❌ 缺（要改 `index.json` 或 LAPP 头）|
| **下架** | ❌ 缺（只能手工删 `index.json` 条目）|
| **历史版本浏览** | ❌ 文件有备份（`pkgs_backup/`），**无 UI** |
| **管理员操作日志** | ❌ 无表、无记录 |

---

## E 推荐架构（不实施，供确认）

**结论：不要另起一套，把现有 `server/upload` 补全即可。**

1. **身份**：沿用邮箱验证码（已有），不引入密码/第三方 OAuth。
2. **投稿**：沿用单文件 LAPP；新增可选"图标/截图"上传仅用于网页展示（不入包）。
3. **审核**：`review.php` 补 4 个动作 —— 改展示信息、下架、历史版本列表、操作日志。
4. **发布**：沿用 `lib.php publish()`（已原子写 index.json）。**保持"人工点通过"为唯一发布触发**，不做自动直发。
5. **审计**：新增 `audit_log(id, ts, actor, action, target, detail)` 一张表即可。
6. **固件**：投稿上线阶段**零改动**。要补的安全性按 F 节单独立项。

---

## F 预计涉及的文件

**服务器（改动集中在这里）**
```
server/upload/lib.php        + 审计日志、下架、历史版本、展示信息编辑
server/upload/review.php     + 上述 4 个界面/动作
server/upload/index.php      + 规范正文、驳回原因展示、投稿记录分页
server/upload/submit.php     + （可选）素材上传、更严的内容校验
server/upload/config.example.php  模板同步
线上 data/app.db             + audit_log 表（ALTER/CREATE，需授权）
nginx upload.conf            （可选）+ 静态素材目录、限速
```
**固件（仅在"要安全加固"时才动，非投稿必需）**
```
components/apps/pages/page_lua.c   luaL_openlibs → 白名单库；应用级内存/CPU 配额
components/apps/lua_bridge/*       能力声明扩展（目前只有 need=wifi）
components/services/apppack/*      LAPP 头增加 permissions= 字段
components/apps/pages/am_store_net.c / net_client  若引入签名校验
```

---

## G 安全风险（按严重度）

| # | 风险 | 证据 | 建议 |
|---|---|---|---|
| G1 | **Lua 无沙箱**：投稿应用能 `io.open`/`os.execute`（若编译开）/删文件 | `page_lua.c:383` `luaL_openlibs` | 固件侧改白名单库；这是长期最大项 |
| G2 | **静态扫描可绕过**：15 条关键字是 `strpos` 精确匹配，`app.fs.rem".."ove` 之类拼接即失效；且风险等级只提示不拦截 | `lib.php scan_risks()` | 改为"高风险必须人工确认"的硬门禁 + AST/正则加强 |
| G3 | **审核令牌在 URL**：`review.php?k=xxx` 会进 nginx access log / 浏览器历史 / Referer | `review.php:11-13` | 改 POST + 一次性短时 token + 登录会话 |
| G4 | **无签名**：CRC32 由服务器给，服务器被攻破即可推任意包；设备无从验证来源 | 全链路 | 中期加 Ed25519 签名（固件需改） |
| G5 | **单机资源紧张**：2 vCPU / 1.67 GB 同时跑 WP + 商店 + 投稿站，可用内存 1.05 GB | 【线上】free | 投稿量上来前先看负载；上传接口限频已有 |
| G6 | 上传体积 4 MB vs 单包上限 512 KB：中间余量给了表单，但**大头文件仍会被 PHP 读进内存** | nginx conf | 保持 4m 够用，勿放大 |
| G7 | 已确认**未泄露**的两处：`config.php` 被 PHP 执行（0 字节）、`data/` 404 | 【线上】 | 保持现状；**不要**给 `/upload/` 加 `^~` location |

---

## H 实施阶段（建议）

| 阶段 | 目标 | 范围 | 测试 | 回滚 |
|---|---|---|---|---|
| P0 已具备 | 现有投稿站可用 | 无改动 | 人工投一包走完流程 | — |
| P1 补管理闭环 | 改信息/下架/历史版本/审计日志 | `lib.php` `review.php` + `audit_log` 表 | 审核 3 个假包；验证 index.json ver 递增、旧包在 backup | 备份 `app.db` + `index.json`；回滚=恢复两文件 |
| P2 加固 | 令牌改 POST、扫描硬门禁、限频细化 | `review.php` `lib.php` nginx | 无 k 访问 403；高频提交被限 | 同上 |
| P3 固件安全（可选） | Lua 白名单库 + 能力声明 | `page_lua.c` `lua_bridge` `apppack` | 现网应用回归（重点：地图/阅读器/白板） | 保留旧 `luaL_openlibs` 分支一键切回 |

---

## I 尚未确认

1. `index.php` 页面上是否已有"投稿规范"正文、是否展示驳回原因（未逐行读，只看了文件清单与容量）。
2. 线上 `data/pkgs/` 为空 ⇒ 无法验证"待审包命名/权限"在真实文件上的表现。
3. 是否有定时任务在重建 `index.json`（今天 09:19 有更新，来源未确认——可能是别的构建脚本）。
4. 固件 Lua 是否禁用了 `os.execute`（需看 `components/libs/lua` 的编译宏，本轮未查）。
5. 服务器是否已有备份策略（宝塔计划任务未查）。

---

## 结论（对应你的 7 问）

1. **能承载吗** → ✅ 能，且**已经部署在跑**（PHP 8.2 + SQLite，内存余 1 GB，磁盘余 25 GB），只是还没有真实投稿。
2. **能复用现有商店后端吗** → ✅ 就是同一个后端：发布 = 写 `bbk/pkgs/` + `bbk/index.json`，设备主源就是它。
3. **需要新增表吗** → 核心流程**不需要**（4 张表够用）；建议仅加 `audit_log` 一张。
4. **需要改固件吗** → 投稿功能**完全不需要**。要提升安全性才需要（Lua 沙箱、能力声明、签名）。
5. **审核通过能自动上架吗** → ✅ **已经实现**（复制→备份→index.json tmp+rename→ver+1）。
6. **推荐架构** → 沿用现站补全管理端 + 审计表；固件零改动；安全加固单独立项。
7. **涉及模块** → 服务器 `lib.php`/`review.php`/`index.php`（+1 表）；固件仅在 P3 才动 `page_lua.c`/`lua_bridge`/`apppack`。

**一句话**：这条路已经铺好了 80%，缺的是管理端的四个动作和一层安全加固，不需要新建系统。
