# LinTOS 投稿平台正式化 · 本轮实施报告（固件隔离部分完成）

**日期**：2026-10-31　**基线**：`main @ c55ad92`（tag v2.0），未提交 34 → **45**
**边界**：未部署、未修改线上、未烧录、未提交

---

## 0 一句话结论

**固件端 Lua 第三方隔离已实现、已编译、已过 23 项主机测试；服务器端 PHP 改造本轮未做**，
原因：本机没有 PHP 运行时，写出来只能靠肉眼，无法满足"不能只语法检查就交付"的标准。
两条都给了可执行的下一步。

---

## 1 已完成（可验证）

### 1.1 新增文件（617 行）

| 文件 | 作用 |
|---|---|
| `components/apps/lua_bridge/lua_path_guard.h/.c` | **纯 C 路径守卫**（无 ESP 依赖）→ 主机可测 |
| `components/apps/lua_bridge/lua_sandbox.h/.c` | 信任级 / 摘库 / io 包装 / 内存配额 / 时限钩子 / NVS 命名空间 |
| `tests/lua_guard/lua_guard_test.c` | 23 个断言，直接编译生产源码 |

### 1.2 修改（3 个文件）

- `components/apps/pages/page_lua.c`
  - 分配器改为**计费版**：`used = used - osize + nsize`，超限返回 NULL（Lua 抛 "not enough memory"，被 pcall 收住）
  - `lua_load_run()` 启动前调 `lua_sandbox_begin(path)`；`openlibs + bridge` 后调 `lua_sandbox_apply(L)`
  - `lua_nvs_h()` 改为 `svc_nvs_open(lua_sandbox_ns("luaapp"))`
- `components/apps/lua_bridge/luat_lib_store.c`
  - `lns_ns_name()` 统一过 `lua_sandbox_ns()` ⇒ **第三方不再写 `core_db`**
- `components/apps/CMakeLists.txt`：SRCS 加两个新文件

### 1.3 隔离到底做了什么

| 层 | 官方（`/store/apps/`）| 第三方（其余路径）|
|---|---|---|
| 信任判定 | 路径前缀 | 路径前缀（应用改不了自己的安装位置）|
| 标准库 | **完全不动** | 摘 `os.execute/os.exit/os.tmpname/io.popen/io.tmpfile/package.loadlib/loadfile/dofile`；`package.path/cpath` 置空 |
| 文件访问 | 原样 | `io.open/os.remove/os.rename` 全部经路径守卫：**只能落在 `/sdcard/apps/.data/<appid>/`**；自身目录只读 |
| NVS | 原样（`core_db` / `luaapp`）| 映射到 `appdb_<8hex>`（14 字符，不超 NVS 15 字节上限）|
| 内存 | 不限 | 4 MB（`LUA_SANDBOX_MEM_KB`，**未经实测，待调**）|
| 运行时间 | 无限 | 10 s 硬超时（`lua_sethook`，每 20 万指令查一次）|

### 1.4 测试与构建结果

```
主机测试（tests/lua_guard，直接编译 lua_path_guard.c）
  23 PASS / 0 FAIL
  覆盖: 相对路径规整、裸 ..、多级越界、绝对路径+穿越、合法回退、
        数据目录可写/自身目录只读、别的应用数据目录、/sdcard/fonts、
        /store 不可写、前缀攻击(/data/ab 冒充 /data/a)、超长不截断、appid 推导

固件构建: exit=0，LinTOS.bin 0x3EE570（改前 0x3EC560，+8,208 B）
          lua_sandbox.c / lua_path_guard.c 零告警
```

### 1.5 过程中发现并修掉的两个**会破坏现有应用**的问题

1. **中文包名**（商店里大量如 `英雄坛说.lua`）原本推不出 appid ⇒ 会被拒绝启动。
   改为：ASCII 名直接用，纯中文名退化为**稳定哈希 id**（`a4c51b77f`），同名恒定。
2. **官方应用推不出 id 就拒绝启动** 属于不可接受的兼容性回归 ⇒ 官方路径放行（用 `official` 占位）。

### 1.6 明确没做的（不能假装）

- **L4 能力白名单**（`app.hid.*` / `app.usb*` 对第三方禁用）：未做。需要先跑完官方 31 应用的 API 清单，否则可能误伤。
- **`app.fs.*` 的路径约束**：未做，仍是原样调用。它是最可能绕过 `io.open` 的旁路，**必须补**。
- 真机验证：无。

---

## 2 未完成：服务器端（有明确原因）

| 阻塞点 | 事实 |
|---|---|
| 本机无 PHP | `which php` 无结果（macOS 自带已移除）；`php -l` 也跑不了 |
| 线上不许动 | 你明确"本次不授权修改线上生产服务器/数据库/nginx" |
| 唯一替代 | 在服务器 `/tmp` 建隔离沙箱跑测试 —— 仍属"在服务器上写文件"，需你一句话授权 |

**为什么不硬写**：管理员认证、CSRF、发布事务这类代码，写错的表现是"看起来能跑、实际能绕过"；
没有运行时验证的 PHP，比没有更危险。这条我按你的第九条纪律"不能只语法检查就宣布完成"处理。

**可执行的下一步（二选一）**：
- A. 你允许我在服务器 `/tmp/bbk_test/` 复制一份站点 + 独立 SQLite 跑真实请求测试（不碰生产目录），我一次把投稿站改完并给出测试日志；
- B. 本机装 `php`（`brew install php@8.2`），我在本地跑完整测试后再由你决定何时部署。

---

## 3 服务器改造清单（代码未写，方案已冻结在上一份文档）

`docs/LinTOS 投稿平台正式化改造方案.md` §2/§4/§5 已给出逐文件清单：
`lib.php`（audit_log / 发布六项校验 / unpublish / rollback / scan_lua）、
`review.php`（去 `?k=`，session+CSRF，5 个新动作）、`auth.php`（管理员口令）、
`index.php`（规范页 / 驳回原因 / 重投）、`submit.php`（UTF-8 文本校验）、
`audit_log` 表。

---

## 4 回滚方法

```
固件（本轮全部改动可一键回退）:
  git diff -- components/apps/pages/page_lua.c components/apps/lua_bridge/luat_lib_store.c components/apps/CMakeLists.txt
  # 撤销三处修改 + 删 4 个新文件 → 行为回到 2.0（隔离完全不存在）
  # 或在 page_lua.c 把 lua_sandbox_apply() 那行注释掉 = 只关隔离、保留计费
```
不需要动 sdkconfig / 分区表 / 引脚。

---

## 5 需要你拍板才能继续

1. 服务器测试走 **A（服务器 /tmp 沙箱）** 还是 **B（本机装 php）**？
2. 内存配额 4 MB、超时 10 s 是**未实测值**，是否先按此跑一轮真机再定？
3. `app.fs.*` 的路径约束要不要一并做（属于 L4，需先出官方 API 清单）？

---

## 6 诚实的状态表

| 项 | 状态 |
|---|---|
| 路径守卫 | ✅ 已实现 · ✅ 23 项主机测试通过 |
| 信任级判定 | ✅ 已实现 · ⚠ 仅路径前缀（未做"受保护安装元数据"，已在方案里标为后续） |
| 标准库裁剪 | ✅ 已实现 · ❌ 未真机验证 |
| NVS/core_db 隔离 | ✅ 已实现 · ❌ 未真机验证 |
| 内存配额 | ✅ 已实现 · ⚠ 阈值未实测 |
| 运行超时 | ✅ 已实现 · ❌ 未真机验证 |
| 能力白名单 / app.fs 约束 | ❌ 未做 |
| 固件构建 | ✅ exit=0，0x3EE570 |
| 服务器改造 | ❌ 未做（无 PHP 运行时） |
| 真机测试 | ❌ 未做（不烧录） |
