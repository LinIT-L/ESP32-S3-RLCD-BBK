# ⚠ 已归档（2026-10-05）

> 桥**模块化重构已完成**，这是过程记录。现行 API 规范看 `Lua桥API参考.md`（自动生成）。
> （归档而非删除：结论与证据留痕。）

---
# Lua 桥：模块化重构 + API 规范

> **状态：✅ 已于 2026-10-02 执行完毕并通过真机验证**（`page_lua.c` 1869 → 558 行；
> 新增 `components/apps/lua_bridge/` 11 个文件；编译 0 error；真机进应用/按键/退出全部正常，panic=0，
> 画面与拆分前逐像素一致）。执行记录与 6 个踩坑清单见技能 `lintos-lua-app-pipeline` §18。
>
> 目标：**完整、可用、不缺东西、模块化、规范化、可靠**。
> 标杆参考：**LuatOS**（合宙，MCU 上的 Lua 运行时，MIT，GitHub `openLuat/LuatOS`）。
> 本文是**可执行的改造方案**（含目录划分、注册契约、文档自动化、迁移步骤）。

---

## 一、先看标杆：LuatOS 怎么做（我们直接抄它的骨架）

| 维度 | LuatOS 的做法 | LinTOS 现状 | 结论 |
|---|---|---|---|
| 分层 | Lua VM(`lua/`) → 核心框架(`luat/`) → 组件(`components/`) → 脚本层(`script/`) | 无明确分层，桥和页面混在一起 | **照抄 4 层** |
| C→Lua 绑定 | **一个模块一个绑定文件**：`components/<模块>/luat_lib_<x>.c`，导出 `luaopen_<x>()` | 全部 ~120 个函数挤在 `page_lua.c`（1900 行） | **按模块拆分** |
| 模块注册 | `lua_module_<x>_register()` 把 `luaopen_<x>` 绑到模块名 | `lua_setup_globals()` 里一大段线性 setfield | **改成注册表驱动** |
| 脚本库分层 | 核心库(C, 内置固件) / 扩展库(Lua, `require` 才加载) | 只有 C 桥；Lua 侧应用是单文件 | 先拆 C 桥（本期），Lua 库分层后续 |
| API 文档 | `tools/sys_pub.py` **扫源码注解自动生成 wiki** | 靠 `docs/Lua单文件程序规则.md` 手写，易过时 | **注解 + 生成 + 闸门** |
| 测试 | `testcase/`，函数名 `test_` 前缀，`assert` 断言 | `tools/lua_check.py`（语法+契约）+ `tools/lua_sim.py`（本机跑图） | 已够用，补"API 覆盖" |
| 编码规范 | 宏体整体加括号、3 处重复就抽函数、跨线程计数用原子操作 | 有踩坑记录但未成文 | 写进本文第六节 |

---

## 二、现状问题诊断（为什么"老是做得不对"）

| # | 问题 | 证据 | 影响 |
|---|---|---|---|
| 1 | **`LUA_32BITS` 是 PRIVATE** → Lua 库按 32 位 `lua_Integer` 编译，消费方按 64 位 | `components/libs/lua/CMakeLists.txt` | ☠️ **C 送出的数字到脚本侧全变垃圾**（`app.key()` 返回 PSRAM 地址、`app.UP` 读成 -2、`action(a)` 入参失效）。**已修：改 PUBLIC** |
| 2 | 桥全部堆在一个文件里（~120 函数 + 状态机） | `page_lua.c` 1900 行 | 改一处怕碰坏别处；无法单独测试 |
| 3 | 没有"哪个函数归哪个模块"的成文契约 | 靠人读代码 | 加新 API 容易放错层、命名不统一 |
| 4 | API 文档与代码不同源 | `docs/` 手写 | 文档必然过时 → 用户照着写就错 |
| 5 | 应用拿不到键（历史遗留的通道混乱） | 见 §五 | 已修（单通道 + ABI 修复） |

---

## 三、目标目录结构（模块划分表）

```
components/apps/lua_bridge/          ← 新建：Lua 桥（把 page_lua.c 里那 1200 行搬出来）
├── lua_bridge.h          · 注册契约 + 共享上下文 (lua_bridge_ctx_t)
├── lua_bridge.c          · 只做调度：依次调用各模块的 register；不含任何具体 API
├── luat_lib_gfx.c        · 绘制+字模   clear px fill rect hline vline bmp bmpr
│                                        text texts textm textsb textc big bigc mix mixc mixw
│                                        zh zh24 zhs zhsm zhsb sbar
├── luat_lib_input.c      · 输入        key key_down keys_mask key_repeat touch touch_start touch_hold
│                                        millis delay redraw exit
├── luat_lib_sys.c        · 系统/提示   sys.battery sys.heap_free sys.time_ms sys.clock
│                                        log tone tone_async melody vibrate
├── luat_lib_fs.c         · 文件        fs.exists read write append remove listdir
├── luat_lib_store.c      · 持久化      nvs.get_str/set_str/get_int/set_int ; db.set/get/del/seti/geti
├── luat_lib_net.c        · 网络        net.is_connected get_ip http_get ping port arp arp_list ipinfo vendor
├── luat_lib_wifi.c       · 无线        wifi.init scan scan_done ap_count ap_info probe_* release ch_stat
│                                        cur_ssid connecting connect clear_saved
├── luat_lib_hid.c        · USB 键鼠    hid.start stop connected running key key_str mouse_* shortcut cdc_*
├── luat_lib_kbd.c        · 软键盘      kbd.use reset draw hit geom height count press char label layer_on get set
└── luat_lib_dlg.c        · 弹窗        dlg.toast
```

`page_lua.c` 只保留**页面与状态机**：`lua_enter/render/action/touch/poll/on_exit`、`lua_load_run`、`lua_stop`、
错误容忍策略、`_key` 通道 —— 目标 **≤450 行**。

---

## 四、注册契约（每个模块长一个样，照抄即可）

```c
/* lua_bridge.h */
#pragma once
#include "lua.h"
typedef struct {
    st7305_handle_t *lcd;     /* 绘制模块要 */
    ui_ctx_t        *ctx;     /* 少数模块要(如 sbar/dlg) */
} lua_bridge_ctx_t;

/* 每个模块实现这一个函数：往栈顶的 app 表里塞自己的字段 */
typedef void (*lua_mod_register_fn)(lua_State *L, const lua_bridge_ctx_t *bc);
extern const lua_mod_register_fn lua_mod_gfx_register;
extern const lua_mod_register_fn lua_mod_input_register;
/* … */
```

```c
/* luat_lib_gfx.c —— 一个模块的样子 */
static int l_gfx_clear(lua_State *L) { ... }
static int l_gfx_fill (lua_State *L) { ... }

void lua_mod_gfx_register(lua_State *L, const lua_bridge_ctx_t *bc) {
    (void)bc;
    /* ⚠ 栈顶必须是 app 表, 一律用 lua_setfield(L, -2, "名字") */
    lua_pushcfunction(L, l_gfx_clear); lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, l_gfx_fill);  lua_setfield(L, -2, "fill");
    /* … 子表也在这里建：lua_newtable → 填 → lua_setfield(L,-2,"名称") */
}
```

```c
/* lua_bridge.c —— 只有一张表, 加模块 = 加一行 */
static const lua_mod_register_fn k_mods[] = {
    lua_mod_gfx_register,   lua_mod_input_register, lua_mod_sys_register,
    lua_mod_fs_register,    lua_mod_store_register, lua_mod_net_register,
    lua_mod_wifi_register,  lua_mod_hid_register,   lua_mod_kbd_register,
    lua_mod_dlg_register,
};
void lua_bridge_setup(lua_State *L, const lua_bridge_ctx_t *bc) {
    lua_newtable(L);                                   /* app */
    /* 常量走一个专门的模块, 见 §六 规则 1 */
    lua_mod_consts_register(L, bc);
    for (unsigned i = 0; i < sizeof(k_mods)/sizeof(k_mods[0]); i++) k_mods[i](L, bc);
    lua_setglobal(L, "app");
}
```

**规矩**：① 名字一律 `l_<模块>_<动作>`（静态）；② 只通过 `lua_setfield(L, -2, …)` 写栈顶的 app 表；
③ 模块之间**不互相调用**，共用逻辑抽到 `lua_bridge_util.c`（如 `pl_now_ms()`、`utf8_len()`）。

---

## 五、可靠性清单（"所有东西都要可靠"）

| 项 | 规则 | 状态 |
|---|---|---|
| **ABI 一致** | `LUA_32BITS` 必须 **PUBLIC**；改 Lua 配置后必须**全量重编**（`touch` 所有 lua 消费方）| ✅ 已修 |
| **数字通道** | 修好 ABI 后 `lua_pushinteger/luaL_checkinteger/lua_tointeger` 才可靠；**加新 API 后必须上机验一次**（本机模拟器测不出 ABI 问题） | ✅ 已验证 |
| **按键通道** | **只走一条路**：桥写 Lua 全局字符串 `_key`（"1".."7"），脚本 `poll()` 里 `tonumber` 后清空。⛔ 不要再用"双通道 + 时间去重"（帧 20~50ms，去重窗口必然失效） | ✅ 已落地 |
| **BACK 语义** | 脚本 `action(a)` 一律 `return true`（应用自己拥有 BACK：弹窗→关；子菜单→退一层；顶层→`app.exit()`） | ✅ 已落地 |
| **暂停页退栈** | 任何页面 `on_exit` 必须能安全释放；`os_pane` 这类"静态 BSS 结构 + early-return 初始化"要加空指针兜底 | ✅ 已修 |
| **字体** | 16px 中文一律 `app.zh`（`font_zh16`，与系统 UI 同款）；⛔ `app.zhsb` 是 24×24 降采样，会糊 | ✅ 已修 |
| **改完必验** | 顺序固定：`lua_check` 闸门 → `lua_sim` 本机跑图 → `sync_apps` → `merge_flash` → 烧 `0x0` → `remcon app` + `key` + `fbdump` 核图 | 已流程化 |

---

## 六、编码规范（照 LuatOS + 本项目踩坑）

1. **常量与函数同源**：`app.BLACK/WHITE/UP…` 这类常量必须由**同一个 `luat_lib_consts.c`** 推送，
   且与 `os.h` 的 `os_action_t` 用 `_Static_assert` 钉住（防两边改歪）。
2. **宏体整体加括号**；`#define` 里的低优先级运算符必须包起来（LuatOS 明文规矩）。
3. **同一模式出现 ≥3 次就抽函数**（如 `fill_rect`、`strip_ext`）。
4. **边界一律显式**：`snprintf` 用 `%.*s` 让编译器看得见上限（本项目已被 `-Wformat-truncation` 咬过）。
5. **不用时间去重**：跨帧的时间窗口会随帧率失效 —— 要去的是**通道**，不是去重。
6. **任何"退出路径"都要问一句**：它依赖的字段，在最短进入路径上被赋值了吗？（`os_pane_reset(NULL)` 事故）
7. **Lua 侧**：只 `--` 注释（无 `/* */`）；`local` 必须在声明之后才可见；`lua_pcall` 要求"函数在参数下面"。

---

## 七、API 文档自动化（✅ 已落地：`tools/lua_api_doc.py`）

> 对照 LuatOS 的 `tools/sys_pub.py`（扫源码注解生成 wiki）。本项目更省事：**不用写注解**，
> 文档直接从源码提取三样东西 —— ① 注册行（有哪些 API）② `lua_newtable`/`lua_setfield` 块（挂在哪层）
> ③ **函数上方的注释块（说明文字）**。改代码 = 改文档。

```bash
python3 tools/lua_api_doc.py           # 生成 docs/Lua桥API参考.md
python3 tools/lua_api_doc.py --check   # 与源码对账; 不一致 exit 1
```

产出：`docs/Lua桥API参考.md` —— **110 个 API / 10 个模块**，每条含
`app.<子表>.<名>` + 说明 + `luat_lib_<模块>.c:行号`。

**已挂进构建期**（根 `CMakeLists.txt`）：新增 `lin_lua_api_guard` 自定义目标，与既有的
`lin_dep_guard`（分层依赖守卫）并列，`add_dependencies(${CMAKE_PROJECT_NAME}.elf lin_lua_api_guard)`。
⇒ **改了 API 却没重新生成文档，构建直接失败**，从根上杜绝"文档与代码不同步"。

⚠ 写这个工具的两个坑（复现时注意）：
1. **子表名在块的末尾才出现**（`lua_newtable` 在前、收尾的 `lua_setfield(L,-2,"tab")` 在后），
   边扫边定名一定会把 `app.fs.exists` 写成 `app.exists` → 必须**两遍扫描**。
2. 说明文字要"取最后一段"并限长：函数上方常压着几十行 banner 注释，全拼进去没法读。


---

## 八、迁移步骤（每步都可独立验证，不一次性大爆改）

| 步 | 动作 | 验收 |
|---|---|---|
| 1 | ✅ 修 `LUA_32BITS` 为 PUBLIC + 全量重编 + 上机验数字 | `app.W/H` 读出 400/300 |
| 2 | 建 `lua_bridge/` 骨架（`lua_bridge.h/.c` + `luat_lib_consts.c`），`page_lua.c` 改调 `lua_bridge_setup()` | 编译过 + 上机功能不变 |
| 3 | 迁 `gfx`（最大、最独立，~400 行） | `fbdump` 与迁移前**逐像素一致** |
| 4 | 迁 `input` + `sys`（含 `_key` 通道与 `log/tone`） | 按键、发声、日志正常 |
| 5 | 迁 `fs`/`store`/`dlg` | nvs 读写、弹窗正常 |
| 6 | 迁 `net`/`wifi`/`hid`/`kbd` | 各应用分别上机跑一遍 |
| 7 | 写 `tools/lua_api_doc.py` + 挂 `--check` 闸门 | 生成的 API 文档与代码零差异 |
| 8 | 拆 Lua 侧：`script/libs/*.lua`（可复用 Lua 库，`require` 加载） | 两三个应用共用同一份 UI 工具库 |

**每步的验收方式固定**：`lua_sim` 本机图 + `fbdump` 真机图 + 无 panic。
⛔ **第 2~6 步都是纯搬代码，不得顺手改逻辑** —— 搬迁与改行为分开提交，出问题才好二分。
