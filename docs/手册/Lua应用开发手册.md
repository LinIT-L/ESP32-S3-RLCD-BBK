# Lua 应用开发

> **解决什么问题**：从零写一个能跑、不卡、能干净退出的 LinTOS Lua 应用。
> **真相源**：宿主 `components/apps/pages/page_lua.c`；API 实现 `components/apps/lua_bridge/luat_lib_*.c`。
> **API 全量**：见 **§10 附录 A**（201 函数 / 30 常量，含参数、返回、阻塞特性与坑）；`API完整参考手册.md` 是自动生成的索引（API 名字与注册位置以它为准，勿手改）。
> 本文所有结论按源码核实；标 **待真机验证** 的条目没有实测数据。

## 0 三条一眼结论

1. **入口是全局函数**，不是 `return` 表。宿主用 `lua_getglobal("render"/"poll"/…)` 逐个取。
2. **`render()` 每帧都会被调用**（RUN 态每帧置 `needs_redraw`，`page_lua.c:677`）。`app.redraw()` 不是"触发重绘"，而是"别跳过这一帧"——要不要画由你自己判断。
3. **所有 Lua 回调都跑在系统主循环里**，没有独立任务。任何阻塞调用（`app.delay`、`app.tone`、同步 HTTP、大文件整读）= 整机 UI 停摆，连返回键都不响应。

---

## 1 快速入门

### 1.1 运行环境

| 项 | 事实 |
|---|---|
| Lua 版本 | 5.5.0（`components/libs/lua`，上游 espressif 组件 v5.5.0） |
| 数值宽度 | `LUA_32BITS=1` ⇒ 整数 **32 位**、小数 **float（约 7 位有效数字）** |
| 标准库 | base / coroutine / string / table / math / os / io / debug / utf8 **全开**；`require` 无自注册模块，多文件靠 `app.fs.read` + `load()` |
| 状态机 | **全局单例**，一次只跑一个应用；退出即 `lua_close` |
| 执行位置 | `app_main` 主循环（帧周期 16ms，脏帧 8ms） |
| 内存 | Lua 分配器只走 PSRAM，**无硬上限**；PSRAM 枯竭 → Lua 抛 "not enough memory" |
| 屏幕 | 400×300，1bpp（黑=0 / 白=1），反射式无背光 |
| 单帧调用顺序 | `os_tick → poll`（含定时器）→ `os_render → render` → 内核统一 `flush` |

### 1.2 应用放哪里

| 位置 | 用途 | 说明 |
|---|---|---|
| `apps/lua/apps/app/` | 源码：工具/应用类 | 同步后进 SPIFFS 分区 `apps`，设备路径 `/store/apps/app/` |
| `apps/lua/apps/ops/` | 源码：运维类 | 设备路径 `/store/apps/ops/` |
| `/sdcard/apps/{app,ops}/` | TF 卡应用 | 商店安装的默认位置 |
| `/sdcard/apps/data/` | 应用私有数据 | 建议存这里或 `/sdcard/Rec` 一类专属目录 |

- 应用管家**一次扫三个目录**（`engine/app/ops` × SPIFFS/TF），**同名以 `/store` 优先**，最多 **48 条**（`am_lua.c:280-350`）。
- 应用 ID = `lua:<文件名>`；文件名即唯一键，改名等于换了个应用。

### 1.3 创建第一个应用（最小可运行）

**① 新建文件** `apps/lua/apps/app/hello.lua`，**② 写 LAPP 头**（注释行，固件静态解析、不执行）：

```lua
--@lapp v1
--@app name=你好 cat=软件 desc=最小可运行示例 version=1
-- ↑ build=YYYYMMDDHHMM 由 tools/pack_app.py 打包时自动写入，手写示例里不要写
```

**③ 写五个契约函数**：

```lua
local S = { t0 = 0 }

function on_enter()                 -- 进页面：初始化（可选）
  S.t0 = app.millis()
end

function render()                   -- 只画，不做 IO（每帧都会被调用）
  app.clear(app.WHITE)
  app.mixc(app.W // 2, 110, "你好 LinTOS")     -- 16px 中英混排、水平居中
  app.textc(150, "按返回键退出")                -- 8x16 ASCII、按屏宽居中
  app.mixc(app.W // 2, 200, "已运行 " .. ((app.millis() - S.t0) // 1000) .. " 秒")
end

function poll()                     -- 每帧尾巴：只做非阻塞的事（可选）
end

function action(a)                  -- 按键：a = app.UP/DOWN/LEFT/RIGHT/OK/BACK
  if a == app.BACK then return false end   -- false = 交回框架（弹"退出程序?"）
  return true
end

function on_exit()                  -- 退出清理（可选，但强烈建议写）
end
```

**④ 跑起来**：应用管家（主菜单 → 应用管理）里点它；或重新打包刷入（见 §7）。

**⑤ 退出**：按返回 → 弹「退出程序?」→ 确定。或脚本内 `app.exit()`。

> 最小可运行只需 `render()` 一个函数；其余四个都是可选的。但**名字必须是全局函数**，写成 `local function render()` 桥侧看不到，表现是"界面全白且无报错"（`tools/lua_check.py` 会拦这一条）。

### 1.4 一个应用从启动到退出的完整链路

```text
应用管家 am_lua.c
  → os_page_lua_set_boot(path)     记下要跑的脚本
  → os_push(OS_PAGE_LUA)
  → lua_enter()                    消费 boot_path
  → lua_load_run()                 lua_newstate(PSRAM) → openlibs → 建 app 表 → 读文件 → pcall
  → on_enter()
  ┌ 每帧 ────────────────────────────────────────┐
  │ os_tick → lua_poll → lua_util_tick → poll()  │
  │ os_render → render() → 内核 flush            │
  └──────────────────────────────────────────────┘
  → 按键 → action(a)（BACK 先问脚本是否消费）
  → app.exit() 或 BACK 未消费 → 弹「退出程序?」→ on_exit() → lua_close() → 退栈
```

细节与异常分支见 §4.4。

---

## 2 API 参考

17 个模块、**201 个注册函数 + 30 个常量**（`consts` 12 + `kbd` 18）。

| 模块 | 数量 | 做什么 | 参考 |
|---|---|---|---|
| `consts` | 12 | `W/H/BLACK/WHITE/UP…HOME` | [A.1](#consts) |
| `gfx` | 26 | 图元、8 套字模、位图、折线 | [A.2](#gfx) |
| `input` | 10 | 时间、按键（事件/电平/连发）、触摸 | [A.3](#input) |
| `sys` | 19 | 日志、蜂鸣/旋律、震动、音量、蓝牙状态、弹窗、退出 | [A.4](#sys) |
| `fs` | 10 | TF 卡文件读写/改名/建目录/列目录 | [A.5](#fs) |
| `store` | 9 | NVS 键值 + 跨应用共享 `core_db` | [A.6](#store) |
| `net` | 13 | 连通性、镜像源、HTTP（同步/异步）、ping、端口、ARP | [A.7](#net) |
| `json` | 4 | 只物化需要的字段，不建中间树 | [A.8](#json) |
| `wifi` | 17 | 连接、扫描、被动探测（promiscuous） | [A.9](#wifi) |
| `hid` | 11 | USB 键鼠 / CDC 串口 | [A.10](#hid) |
| `kbd` | 13 | 软键盘（4 种样式，布局+命中+取字符） | [A.11](#kbd) |
| `img` | 5 | 1bpp BMP 图集加载/贴图（10 槽资源池） | [A.12](#img) |
| `gps` | 6 | GPS 启停与定位快照（整数定标） | [A.13](#gps) |
| `espnow` | 12 | 无路由器点对点/广播（≤250B） | [A.14](#espnow) |
| `wiki` | 10 | 离线维基（索引/搜索/LZ4 解压/取行） | [A.15](#wiki) |
| `util` | 17 | 定时器、随机、CRC32、base64、序列化、游戏数学 | [A.16](#util) |
| `rec` | 14 | WAV 录音/播放/电平/频谱 | [A.17](#rec) |

### 2.1 最常用的一屏速查

```lua
app.clear(app.WHITE)                  -- 清屏（默认就是白）
app.mix(x, y, "血 3/6")               -- 16px 混排，返回结束 x
app.mixc(cx, y, "标题")               -- 混排居中
app.zhsm(x, y, "纯汉字")              -- 12px 汉字
app.fill(x0,y0,x1,y1, app.BLACK)      -- 填充矩形
app.rect(x0,y0,x1,y1, app.BLACK)      -- 描边矩形
app.px(x, y, app.BLACK)
app.hline(x0, x1, y, app.BLACK)

local now = app.millis()                        -- 开机毫秒
local x, y = app.touch()                        -- 当前按住点，没按返回 0 个值
local held = app.touch_hold()                   -- 按住时长 ms
if app.key_down(app.LEFT) then ... end          -- 电平（支持手柄/虚拟键）

app.dlg.toast("已保存", 1200)                    -- 非阻塞提示
app.nvs.set_str("k", "v"); local v = app.nvs.get_str("k")
local txt = app.fs.read("/sdcard/a.txt")         -- ≤512KB
app.util.after(500, function() app.log("tick") end)
app.tone_async(880, 60, 40)                      -- ⛔ 别用 app.tone（阻塞）
app.exit()                                       -- 请求退出（默认弹确认）
```

完整参数、返回值、阻塞特性、注意事项见 **§10 附录 A**（按模块跳转见上表）；只想知道"有哪些 API、注册在哪个文件"用 `API完整参考手册.md`。

---

## 3 界面与交互

### 3.1 屏幕与状态栏

- **400×300，1bpp**，无背光。`app.W / app.H` 就是这两个数。
- Lua 页是**全屏页**（`page_lua.c` 的 `fullscreen=true`）⇒ **系统状态栏不绘制**，顶部 24px 归应用自己用。
  现有应用的惯例：`local SB_H = 24`，所有绘制从 `y >= 24` 开始（`recorder.lua:63,593`）。
- `app.sbar(v)` 只影响系统状态栏的反白开关；在全屏 Lua 页里**没有可见效果**（静态推断，**待真机验证**）。

### 3.2 绘图 API 分组

| 分组 | API | 备注 |
|---|---|---|
| 清屏/像素 | `clear` `px` `fill` `rect`(=`store_rect`) `hline` `vline` | 坐标容忍小数（四舍五入）；**颜色必须整数**，传 `true/false` 直接报错 |
| 位图 | `bmp`(hex，≤2048B) `bmpr`(二进制串，无上限) | 大图用 `bmpr` 或 `img`，`bmp` 超限是**静默不画** |
| 折线 | `poly(raw[,w,ct,cb])` | 每点 8 字节(i4 x,i4 y)，**一次调用画整条线**，地图类必用 |
| 文字宽度 | `textw` `mixw` | 排版前先量宽 |

### 3.3 字体（只用固件字模，别自绘）

| API | 字体 | 单字宽 | ASCII 表现 |
|---|---|---|---|
| `app.texts` | 8×12 ASCII | 8 | 正常 |
| `app.textm` | 12×18 ASCII | 12 | 正常 |
| `app.textsb` | 8×16 ASCII | 8 | 正常（状态栏同款） |
| `app.text` | 16×24 ASCII | 16 | 正常 |
| `app.big` / `app.bigc` | 8×12 点阵整数倍放大 | 8×scale | 正常 |
| `app.zh` | **16×16** 汉字 | 16 | **空白格** |
| `app.zh24` | 24×24 汉字 | 24 | **空白格** |
| `app.zhs` | 12×12 汉字 | 12 | **空白格** |
| `app.zhsm` | 12×12 汉字 | 12 | **空白格** |
| `app.zhsb` | 16×16 汉字 | 16 | **空白格** |
| `app.mix` / `app.mixc` | 16px 混排 | 汉字 16 / ASCII 8 | 正常 |

- 带 `*m/*sb` 的返回"下一字符起点 x"，可接着排版（`x = app.mix(8, 40, "A"); app.mix(x, 40, "B")`）。
- **中英混排一律 `app.mix`**，不要用 `app.zh` + `app.texts` 拼两次（间距与基线都会错）。

### 3.4 图片

```lua
local id = app.img.load("/sdcard/qqtang/tile.bmp")   -- 1bpp BMP；0 位=透明
app.img.blit(id, sx, sy, dx, dy, 20, 20)             -- 从源图取块贴屏
local w, h = app.img.size(id)
app.img.free(id)          -- 单张释放
app.img.freeall()         -- 退出必调
```

- 只支持 **1bpp BMP**，文件 ≤4MB、宽 ≤2048、`blit` 单次宽 ≤512（超出自动裁剪）。
- **池容量 10**，满了**顶掉第 1 个槽（最早加载的）**且不报错（`luat_lib_img.c:33`）⇒ **先加载核心图集，后加载临时图**。风险见 §8-R6。
- 素材放 `/store/<应用名>/` 或 TF 目录；`img.load` 是**阻塞**的（整文件读 + 逐位解包），只在 `on_enter` 里做。

### 3.5 触摸

-** 框架不转发触摸**：RUN 态 `lua_touch()` 恒返回 false，必须自己在 `poll()` 里轮询：

```lua
local x, y = app.touch()          -- 当前按住点；没按住时返回 0 个值（不是 nil!）
if x then ... end                 -- ⚠ 判 type(x)=="number"，别判 x ~= nil
local sx, sy = app.touch_start()  -- 本次按下的起点（拖拽/滑动方向）
local ms = app.touch_hold()       -- 按住时长（长按判定用它，别自己攒时间）
```

判断"点击 vs 拖动 vs 长按"最小写法（recorder 的实战逻辑，见 §6.1）：
按下沿建状态 → 位移 >12px 记 `moved` → `touch_hold()>=480` 记 `long` → 抬起时 `moved`? 拖动 : `(now-t0)<900`? 点击 : 无。

**弹窗打开时 `poll()` 不执行**（当前页变成弹窗页）⇒ 触摸状态要能容忍"跨弹窗丢失一次抬起"。

### 3.6 按键

| 通道 | API | 用途 |
|---|---|---|
| 事件 | `action(a)` 回调 | 菜单、确认/返回等一次性动作 |
| 队列 | `app.key()` | 同上，轮询式（24 槽 FIFO，满则丢最旧） |
| 电平 | `app.key_down(a)` / `app.keys_mask()` | 连续移动、组合键（**覆盖物理键/蓝牙手柄/网页手柄/虚拟键**） |
| 连发 | `app.key_repeat(a[,first=400,rate=90])` | 列表滚动、连续发炮 |

- 动作码：`UP1 DOWN2 LEFT3 RIGHT4 OK/CONFIRM5 BACK6 HOME7`（`app.UP` 等常量直接用）。
- **HOME 应用内观测不到**：被内核直接消费退回桌面，`app.key_down(app.HOME)` 恒 false。
- **BACK 三重语义**：`action` 返回真值 = 我消费了（不退出）；返回 `false`/`nil` = 交框架弹「退出程序?」；**顶层 `return true` 会吞掉确认框**。
- 想关掉退出确认：`app.exit_confirm(false)`（顶层函数，**不是** `app.sys.exit_confirm`）。

### 3.7 软键盘

```lua
app.kbd.use("rows", 4)                 -- 样式只认首字母：std / edit / rows / dialog
local i = app.kbd.hit(x, y)            -- 命中键索引，-1 = 没中
if i >= 0 then
  local act = app.kbd.press(i)         -- ⚠ 有副作用：会改 caps/shift/fn
  local ch  = app.kbd.char(i)          -- 仅普通字符键有值
  if act == app.kbd.CHAR and ch then buf = buf .. ch
  elseif act == app.kbd.BKSP then buf = buf:sub(1, -2)
  elseif act == app.kbd.ENTER then commit()
  elseif act == app.kbd.ESC then cancel() end
end
```

- 四种样式：`"std"`(5 排全高 145px) / `"edit"`(排高 19px 压下 1/3) / `"rows"`(只画前 N 排) / `"dialog"`(需 x,y,w,row_h)。
- ⛔ **没有 `app.kbd.STYLE_*` 常量**（源码未注册），必须传字符串。
- 键盘只管布局/命中/字符，**文本缓冲、光标、校验全由应用自己实现**（`recorder.lua:791-806`）。

### 3.8 弹窗（一律用系统弹窗，别自绘）

```lua
app.dlg.toast("已保存", 1200)          -- 非阻塞，超时自动关
app.dlg.confirm("删除这条录音?")        -- 非阻塞：发出即返回
-- ... 下一帧在 poll 里取结果 ...
local r = app.dlg.result()             -- nil=还没选 / -1=取消 / 0=确定 / >=1=列表下标
app.dlg.list("选择地图包", names, 1)    -- 返回条数；结果同样走 result()
```

- **轮询式契约，没有回调**：发请求 → 立即返回 → `poll` 里取结果。弹窗期间 `poll` 不执行，所以"发请求后同一帧就取结果"永远取到 nil。
- 弹窗尺寸固定 350×250（框架统一），**不要自绘弹窗**。
- ⛔ 没有 `app.dlg.wheel`。⛔ 弹窗回调里不要再开弹窗、也不要退出页面（框架层会互相顶掉，表现为"弹窗一闪而过"）。

### 3.9 刷新机制（最容易出错的一节）

```text
每帧：os_tick → poll() → [置 needs_redraw=true] → os_render → render() → flush
```

- **`render()` 每帧必被调用**，与"有没有变化"无关。
- `app.redraw()` 的语义是"这一帧别跳过"，**不是**"触发一次重绘"。qqtang 全文件 0 次调用 `app.redraw()` 也照常跑。
- ⇒ **控制频率的责任在脚本**：自己记 `dirty` + 时间闸门（见 §5.2）。

---

## 4 开发规范

### 4.1 常量与布局

- 屏幕/颜色/动作码直接引 `app.*` 常量，**不要写字面量 400/300/0/1**。
- 布局常量集中在文件头，函数体里不出现魔法数（三个参考应用都这么做）：

```lua
local W, H, SB_H = app.W, app.H, 24
local HDR_H, ROW_H, ROWS = SB_H + 46, 44, 3
```

- 按键常量做一层降级保护（老固件可能没有）：

```lua
local A_BACK = (type(app.BACK) == "number") and app.BACK or 6
```

### 4.2 状态管理

- **一个状态表 + 一个字符串"页面枚举"**，例如 `G.mode = "list" | "rec" | "playing"`。
- 手势/按键的"按下中"状态单独一张表（recorder 的 `P`），**不要混进业务状态**。
- 大应用按职责拆表（map：视图 `M` / 下载 `D` / 收藏 `F`），错误态单独 `err_kind + err` 走整页提示。
- 所有状态用 `local`，只在本文件内可见；全局只留五个契约函数与 `msg` 这类必须前向声明的家伙。

### 4.3 函数组织（顺序有讲究）

推荐顺序（与三个参考应用一致）：

```text
常量/布局 → 状态表 → 小工具 → 绘图原语 → 数据层(解析/读文件) → 动作层
→ 命中/分发 → 绘制函数 → render() → 手势 → poll() → action() → on_enter/on_exit
```

- ⛔ **被前面函数引用、但定义在后面的 `local` 函数，必须先声明**：`local msg` / `local dl_next_file`。
  否则前面那些函数会把名字解析成**全局** ⇒ 运行时 `attempt to call a nil value`，整应用挂掉。两个参考应用都为此留了注释（`qqtang.lua:90-93`、`map.lua:1357-1361`）。

### 4.4 生命周期分工

| 函数 | 何时调用 | 该做什么 | 不该做什么 |
|---|---|---|---|
| `on_enter` | 脚本主体 `pcall` 成功**之后**一次 | 建目录、读配置、加载素材、开 GPS | 长阻塞循环、等网络 |
| `poll` | 每帧一次（弹窗/其它页压栈时不执行） | 轮询触摸/按键电平、推进状态机、驱动动画 | 文件整读、同步 HTTP |
| `render` | 每帧一次 | 只画 | 文件 IO / 网络 / NVS / 改业务状态 |
| `action` | 按键时（BACK 特殊） | 改状态、返回真值表示"已消费" | 做耗时操作 |
| `on_exit` | 退出 RUN 态时（`lua_stop`） | 停录音/播放、`img.freeall`、`espnow.stop`、`wiki.close`、写存档 | 依赖网络、长等待 |

补充事实：

- **`on_exit` 一定会执行**的三条路径：`app.exit()`、BACK 未消费、页面退栈（HOME 也走这里）。
- **唯一不会执行的情况**：脚本卡死在某个 C 调用里（如死循环 + 阻塞 API），此时只能整机复位。
- 框架在 `lua_stop` 里还会**兜底关 GPS**（停任务 + 送 46µA 待机 + 卸 UART），但脚本仍应显式 `app.gps.stop()`。
- `app.exit()` 在 `on_enter` 里调用时**不弹确认**（还没"真正跑起来"）；进入 `poll` 后才算跑起来。

### 4.5 异步任务：一律"轮询式"

本项目**没有 Lua 回调**（除 `util.after/every` 的定时器函数）。所有异步能力都是"发请求 → 每帧问结果"：

| 能力 | 发起 | 取结果 | 备注 |
|---|---|---|---|
| 弹窗 | `app.dlg.confirm/list` | `app.dlg.result()` | 弹窗期间 `poll` 不执行 |
| 网络下载 | `app.net.fetch_start(url,…)` | `app.net.fetch_poll()` → `"wait"/"ok"/"err"/"idle"` | 单请求槽；执行在后台任务 |
| 录音/播放 | `app.rec.start/play` | `active()/playing()/ms()` | 底层独立任务 |
| GPS | `app.gps.start()` | `app.gps.fix()` | 后台解析任务 |

⚠ **异步 + 弹窗必须成对轮询**：发起后忘了轮询 = 结果永远没人取，UI 卡在"正在…"。见 §8-R9。

### 4.6 定时器

```lua
local id = app.util.after(500, function() app.log("once") end)   -- 一次性
local pid = app.util.every(1000, tick)                            -- 周期
app.util.cancel(pid)
```

- **16 个槽**，满了 `after/every` 返回 **nil**（不是静默丢弃）——要判空。
- 回调在 `poll` 的同一拍执行（`lua_util_tick`），**没有独立任务**；回调里死循环=整机卡死。
- 换应用/重新加载时槽位自动清空（按 `lua_State` 变化检测）。
- 简单节流用 `app.millis()` 自己算更省（现有 8 个应用 **0 处**使用 `after/every`）。

### 4.7 文件读写

```lua
if not app.fs.exists(DIR) then app.fs.mkdir(DIR) end   -- mkdir 等价 -p，已存在返回 true
local s = app.fs.read(path)                            -- ≤512KB，超限返回 0 个值
local blk = app.fs.read_at(bin, off, size)             -- ≤1MB/次，大文件分块唯一手段
app.fs.write(path, data)                               -- 返回 bool，必须检查
app.fs.rename(a, b); app.fs.remove(p); app.fs.size(p)
local names = app.fs.listdir(dir)                      -- 只返回名字，跳过 . 开头
```

- **路径要带 `/sdcard` 或 `/store` 前缀**，没有"当前目录"概念。
- 写文件**没有 fsync**，掉电可能丢最后一段；重要数据"写完再读回校验"（recorder 的 `verify_saved` 就是这个思路）。
- 目录本身也算"存在"：`app.fs.exists("/sdcard")` 靠 `opendir` 兜底为 true。

### 4.8 网络请求

```lua
-- 小请求 / 只想阻塞拿结果（⛔ 不要在 poll/render 里用）
local body = app.net.http_get("example.com", 80, "/api", 4000)   -- 仅 http，≤4KB，阻塞

-- 正式场景：异步 + 可选流式落盘
if not app.net.is_connected() then app.dlg.toast("未联网") ; return end
app.net.fetch_start(url, 8000)               -- 结果留在内存（≤128KB）
app.net.fetch_start(url, 900000, dst_path)   -- 边收边写 TF（大包，无内存上限）
-- 下一帧起：
local a, body, code, n = app.net.fetch_poll()
```

- `fetch_start` 在**未联网时直接返回 false**（这是故意的：没联过网就发请求会让 lwIP assert 重启）。
- **同一时刻只能一个请求**；忙时 `fetch_start` 返回 false。
- 返回值形状随模式变化，判断时**先看状态字 `a`，再看 body**（流式成功时 body 是 nil）。
- 未校验数据一律当不可信：首字节、字节数、（可选）校验和都要过。

### 4.9 错误处理

框架侧（`page_lua.c:201-248`）：

- 首次报错：串口打日志 + 屏底两行**错误条（带行号）持续 6s** —— 可以直接截图。
- 错误计数**每 500ms 最多 +1**，累计 **90 次（约 45 秒）** 才退回列表。
- 任何一次成功调用即清零；`action()` 抛错则**立刻退出**（防 BACK 被吞导致出不去）。

写应用时：

- 所有"外部世界"的返回都要判：`fs.*` 的 bool、`nvs.set_*` 的 bool、`fetch_poll` 的状态字、`rec.start` 的返回码。
- 提示用 `app.dlg.toast` 或页内一行文字，**不要靠"没反应"当错误处理**。
- 存档类写失败要保留 dirty 以便重试（见 §8-R5 的反例）。

### 4.10 资源释放清单

| 你开了什么 | 必须在 `on_exit` 做什么 |
|---|---|
| `app.img.load` | `app.img.free(id)` 或 `app.img.freeall()` |
| `app.rec.start` / `app.rec.play` | `app.rec.stop()` / `app.rec.stop_play()` |
| `app.gps.start` | `app.gps.stop()`（框架兜底，但仍应显式） |
| `app.espnow.start` | `app.espnow.stop()` |
| `app.wifi.probe_init` | `app.wifi.probe_stop()` / `app.wifi.release()` |
| `app.hid.start` | `app.hid.stop()` |
| `app.wiki.open` | `app.wiki.close()` |
| `app.net.fetch_start` | 至少调一次 `fetch_poll()` 把结果取走（否则槽被占） |
| `app.util.after/every` | 槽位会自动清，但长周期任务建议 `cancel` |
| `app.kbd.use` | 无句柄，无需释放（共享全局状态） |

---

## 5 性能与稳定性

### 5.1 render 每帧调用（第一性能事实）

`page_lua.c:677` 在 RUN 态每帧 `ctx->needs_redraw = true` ⇒ `render()` 每帧执行。
`app.redraw()` 只影响"这一帧要不要画"，不影响"render 会不会被调用"。

**后果**：如果 `render` 无脑全屏重画，就是每秒 62 次全屏重画。
**做法**：`render` 第一行判 dirty，第二行判时间闸门（两道**分开**写）：

```lua
if not G.dirty then return end                       -- 没变化就不画
if (now - G.last_draw) < 16 then return end          -- 画了也别太频繁
G.last_draw = now; G.dirty = false
```

⛔ 反例（qqtang 源码里记录的真实事故）：`if not dirty and (now-last) < GAP then return end`
—— 只有"既不脏、又刚画完"才跳过，等于"过了 16ms 就无条件重绘"，帧率掉到个位数。

### 5.2 dirty 与局部刷新

| 手段 | 适用 | 做法 |
|---|---|---|
| 全量 dirty 闸门 | 一切有静态画面的应用 | 见 §5.1 |
| 分区 dirty | 页面有稳定分区（顶栏/列表/底栏） | `G.part = "header"` 之类，`render` 里只画对应区 |
| 脏格擦除 | 瓦片游戏 | 记录上一帧被动态物体覆盖的格子，先擦回静态层再画新的（`qqtang.lua:1276-1295`） |
| 整块重绘（静态层） | 换关/换图 | 只在状态切换那一帧做，注释里标好成本（qq：约 29ms） |

⚠ **"局部刷新"本身不省算力**，除非 `render` 真的提前 return。recorder 的 `G.part` 只在被请求的那一帧生效，其余帧仍整屏重画 → 实际只省约 13%（见 §8-R4）。

### 5.3 主循环阻塞风险表

| 调用 | 阻塞量级 | 处置 |
|---|---|---|
| `app.delay(ms)` / `app.util.sleep(ms)` | =ms | 只在调试用，正式代码删掉 |
| `app.tone(f,ms)` | **=ms**（内部 vTaskDelay） | 一律换 `app.tone_async` |
| `app.net.http_get` | 1~3s（超时+2s 上限） | 改 `fetch_*` |
| `app.net.ping` / `arp` / `port` | 每个 0.1~0.5s × 次数 | 拆成多次、每帧一次 |
| `app.fs.read` / `read_at` / `listdir` | 与文件数/大小成正比 | 分块、限条数、不在 render 里 |
| `app.img.load` | 整文件读 + 解包（4MB 上限） | 只在 `on_enter` |
| `app.wiki.load/open` | 随机读 + 解压；`open` 内含 1.2s 等待 | 进页面时做，配 loading 提示 |
| `app.gps.start` | 同步 init/wakeup/起任务 | 一次即可，别在 poll 里重复调 |

### 5.4 大文件：一律分块

```lua
-- 整文件读有 512KB 硬上限，18MB 的 .bin 只能这样读
local blk = app.fs.read_at(path, off, size)   -- size ≤ 1MB
```

索引里存每块的 `(off, size)`，只读当前视口需要的几块（`map.lua:430-445`）。
缓存要**双阈值**（条数 + 字节数），淘汰按"离视口中心距离"而不是纯 LRU（刚离开屏幕的块马上要回来）。

### 5.5 图片资源池

- 容量 **10**，满则顶掉 slot0（最早加载的那张），**不报错**。
- 加载顺序 = 淘汰顺序：**核心图集先加载，临时图（背景、预览）后加载**。
- 用完立刻 `free`，不要指望 GC。

### 5.6 Lua 内存与 GC

- 分配器只走 PSRAM，**没有上限也没有 GC 调优**（全项目无 `lua_gc` 调用）。
- PSRAM 不够 ⇒ 分配失败 ⇒ Lua 抛 "not enough memory"（走 pcall，按运行时错误处理）。
- 所以：**大字符串/大表要克制**。`read_at` 读出来的块用完置 nil；`json.pick` 只取需要的字段（`pick` 本身就是为此设计的，不会建中间树）。

### 5.7 32 位整数与 float（最高频的静默 bug 源）

`LUA_32BITS=1` ⇒ 整数 32 位、**小数是 float，约 7 位有效数字**。

| 场景 | 错误写法 | 正确写法 |
|---|---|---|
| 经纬度 | 直接存 `119.119000`（9 位被截断） | 整数微度 `119119000`，显示时自己拼小数点 |
| 时间戳 | `1700000000` 当数字 | `app.util.epoch()` 返回**字符串** |
| CRC32 | 当整数比较 / `%08x` | `app.util.crc32` 返回**8 位十六进制字符串**；或改用 Fletcher-16（map 的做法） |
| 大常数 | `x % 4294967296`（超 int32 变浮点，后续位运算报错） | 只用 `< 2^31` 字面量，**避开位运算**（qqtang 的 `h32`） |
| 坐标运算 | 先乘后减（大数放大误差） | 先减后乘 |

### 5.8 网络任务与音频任务

- 网络：`fetch_*` 的执行在**固件后台任务**里，Lua 只轮询；不存在"跨任务进 Lua VM"的风险（也正因如此，它没有回调）。
- 音频/录音：`app.rec.*` 是独立任务（CPU1），`app.rec.start/stop` **快速返回**；进度靠 `ms()/bytes()` 轮询。
- ⛔ 不要在 `render` 里喂音频或等播放结束。

### 5.9 异常退出与存档保护

| 场景 | 会发生什么 | 你要做的 |
|---|---|---|
| 脚本报错 | 错误条 6s；连续 90 次后退回列表 | 修 typo；用 `app.log` 打状态 |
| `action` 抛错 | **立刻退出应用**（防卡死） | 把 `action` 写得不可能抛错 |
| 用户按 HOME | 页面退栈 → `on_exit` 执行 | 在 `on_exit` 写存档/停任务 |
| 录音中退出 | 底层任务会把 WAV 头补完 | 退出前主动 `stop()` 更稳 |
| 断电 | NVS 有 1s 写节流、存档有节流间隔 | 关键进度"变更即写"，别攒 |

---

## 6 三种实战参考

> 只提炼可复用的骨架与判断，不贴整文件。行号对应当前源码。

### 6.1 工具类：`recorder.lua`（1135 行）—— 触摸状态机 + 硬件任务 + 文件管理

**值得借鉴**

1. **两表状态机**：业务 `G`（含 `G.mode` 六个页面态，`:105-121`）+ 手势 `P`（`:124-125`），互不污染。
2. **`hit()` 返回字符串语义 id**（`"primary"/"row3"/"kbd"`，`:547-590`），`tap()/long_press()` 用 `id:match("^row(%d)$")` 分发（`:808-867`）——比一堆坐标闭区间好读。
3. **硬件任务全走"启动→轮询→收尾校验"**：`start_rec` 四道前置（`:396-421`）→ `tick_rec` 每帧推进 → `verify_saved` 四道校验（`:438-446`）。
4. **三类自愈兜底**（都在 `tick_rec`）：保存超时 5s、写盘停滞 3s、低电量连续 2 次采样 → 自动停录并提示（`:885-939`）。
5. **"录完再退"**：`exit_after_save` 标志（`:819-821, 878-883`），避免录音被退出打断。
6. **WAV 头自己解**（`:283-296`），并对"头没写完"的文件标 `bad` 拒绝播放（`:317-325`）。

**关键片段**

```lua
-- 触摸手势（简化自 :990-1024）
if tx then
  if not P.on then P.on,P.x,P.y,P.id = true,tx,ty,hit(tx,ty); app.redraw()
  else
    if math.abs(tx-P.x) > 12 or math.abs(ty-P.y) > 12 then P.moved = true end
    local hold = app.touch_hold()
    if not P.long and not P.moved and hold >= 480 then P.long = true; long_press(P.id) end
  end
elseif P.on then
  P.on = false
  if not P.long then
    if P.moved then ...翻页...
    elseif (now - P.t0) < 900 then tap(P.id, P.x, P.y) end
  end
end
```

**不要照抄**：`scan()` 每次全量遍历且无条数上限（`:305-335`，见 §8-R3）；`render` 里"局部重绘"只在被请求的那一帧生效（见 §8-R4）。

### 6.2 网络 + 大数据：`map.lua`（2353 行）—— 下载状态机 + 分块 + 折线批绘

**值得借鉴**

1. **状态按职责拆表**：视图 `M`（`:155-173`）、下载 `D`（`:182`）、收藏 `F`（`:180`）；错误单独 `err_kind/err`，走**整页错误态 + 引导文案**（`:1746-1761`）。
2. **下载是一个显式状态机**：`idle → index → dl → idle`，每步失败都"换源重试当前步"，源耗尽才认输（`dl_retry_or_fail :1364-1373`）。
3. **双路径分派**：≥900KB 流式落盘（900s 超时），否则收内存（30s）并在落地前**三重校验**：首字节 + 字节数 + Fletcher-16（`:1396-1415, 1510-1525`）。
4. **"目录即真相源"**：不维护"已安装清单"，扫目录找 `*.json`（`:347-384`）。
5. **分块缓存带淘汰**：24 块 / 3MB 双阈值 + 距离淘汰 + 视口外留 1 圈预取（`:143-146, 449-501`）。
6. **折线批绘**：`app.poly` 一次画整条线，代替逐段 `hline`（`:636-655`，注释里有"6221 次调用 vs 1 次"的对比）。
7. **重活延迟到"所有 local 都可见"的那一帧**：下载完成后不在回调里 `map_reload`，而是在 `poll` 里做（`:2251-2259`）。

**关键片段**

```lua
-- 下载轮询（简化自 :1468-1488）
local a, body, code, n = app.net.fetch_poll()
if a == "wait" then
  if D.stream then D.got = app.net.fetch_got() end
  return
end
local step_err = (a ~= "ok")
if not step_err and D.st ~= "index" and not D.stream then
  step_err = (not body) or (#body == 0)     -- 流式成功时 body 是 nil，别先看 body
end
if step_err then
  if dl_retry_or_fail("下载") then dl_start_file() end
  return
end
```

**不要照抄**：`files[]` 只处理前 30 条、目录只扫前 24 个条目（§8-R1）；`on_exit` 不删车半包（§8-R2）；`render` 每帧全屏重绘 + 同步块读（§8-R7）。

### 6.3 游戏：`qqtang.lua`（1957 行）—— dirty 闸门 + 三层绘制 + 定长步进

**值得借鉴**

1. **dirty 双闸门**（`:1714-1717`）+ **状态切换整屏重建**（`:1719-1724`），是本项目最省算力的范式。
2. **三层绘制**：静态层（背景+瓦片，仅换关/开砖/相机跨格时重建，`:1744-1750`）→ 动态格（擦旧画新，`:1283-1295`）→ HUD（key 缓存，`:1686-1693`，key 里塞 `millis//500` 让时间也参与刷新）。
3. **相机整格吸附**（`:1235-1247`）：只在跨格时返回 true，静态层才重建 —— 大地图能跑起来的关键。
4. **固定步长 + 累加器**（`:1527-1548`）：`step_t += dt; while step_t >= sms do … end`，低帧率下速度不变；`poll` 里 `dt>120` 归一为 16（`:1948`）。
5. **输入用 `key_down` 电平**（`:1549-1555`）做连续移动，`action` 只管菜单。
6. **存档"标记+节流+退出必写"**（`:788-825, 1813`）。
7. **整数 LCG 随机**（`:142-154`）：只用 `<2^31` 字面量、不用位运算 —— 32 位 + float 下的正确写法。

**关键片段**

```lua
-- GAME.render 开头（:1714-1717）
if not GAME.dirty then return end
if (now - GAME.last_draw) < CFG.DRAW_GAP_MS then return end
GAME.last_draw = now; GAME.dirty = false

-- poll 里的 dt（:1944-1951）
local dt = now - last_t; last_t = now
if dt <= 0 or dt > 120 then dt = 16 end
if GAME.state == "play" then GAME.step(dt); GAME.dirty = true end
```

**不要照抄**：`SAVE.write` 丢弃 `nvs.set_str` 返回值还清 dirty（§8-R5）；图片池加载 7~8 张后换背景有顶掉核心图集的风险（§8-R6）；静态层重建单帧约 29ms（注释数字，待实测）。

### 6.4 三者共性（可以直接当模板骨架）

1. 常量/布局在文件头，函数体无魔法数。
2. 单一状态表 + 字符串页面枚举；`render` 只读、`poll/action` 只写。
3. 五个契约函数齐全；`on_exit` 一定归还自己开的资源。
4. 时间基准统一 `app.millis()`；不用 `os.time()`。
5. 前向引用的 `local` 函数先声明。
6. 外部返回值一律判（这一条三个应用做得都不完整，见 §8）。

---

## 7 测试与发布

### 7.1 静态检查（写的时候就跑）

```bash
python3 tools/lua_check.py              # 校验 apps/lua/ 下全部 .lua
python3 tools/lua_check.py a.lua b.lua  # 只查指定文件
python3 tools/lua_check.py -v           # 额外打印函数定义与回调
```

三道闸门（`tools/lua_check.py` 文件头）：① Lua 语法真解析（luaparser）② **五个契约函数必须是全局** ③ 绘制函数末位颜色参数必须是整数（传 `true/false` 在设备上直接报错）。

### 7.2 API 文档对账（改了桥才需要）

```bash
python3 tools/lua_api_doc.py --check    # 只对账，不写文件；不一致 exit 1
python3 tools/lua_api_doc.py            # 重新生成 docs/Lua桥API参考.md
```

新增 `app.*` API 时**必须**跑，否则构建会失败（`docs/规范/文档规范.md §4`）。

保持**§10 附录 A** 不陈旧的规则：① 任何 `app.*` 名字都应能在 `API完整参考手册.md` 里查到（`--check` 通过即代表 201 条齐全）；② 改了桥先重生成参考，再回来补附录里的参数与坑；③ 附录与 `luat_lib_*.c` 冲突时**以实现为准**，并据此修附录。

### 7.3 本机模拟（不用烧录就能看界面）

```bash
python3 tools/lua_sim.py apps/lua/apps/app/recorder.lua
python3 tools/lua_sim.py app.lua --steps "touch:200,70;key:5;render"
```

- 用 lupa（Lua 5.5）+ 固件字库渲染到 400×300 帧缓冲并导出 PNG，字模与设备一致。
- 适合验界面/字体/交互流程；**不覆盖**真机专有问题（32 位整数、PSRAM、真实 SD 速度、音频）。

### 7.4 真机验证

1. 应用管家启动 → 看串口 tag `LUAAPP`（启动路径、错误、img 加载）。
2. 出错时屏底错误条有**函数名 + 行号**，直接截图即可定位。
3. 关掉诊断日志就只剩错误，别靠"画面看起来对"当验证。
4. 性能数据必须实测（`remcon` 的帧率/耗时查询见 [构建测试与发布手册](构建测试与发布手册.md) §9），**不要把注释里的历史数字当承诺**。

### 7.5 安装与更新

#### 7.5.1 LAPP 头（`--@app`）

文件开头是若干行 `--@app key=value` 注释，**固件静态解析、不执行代码**；
`--@lapp v1` 是格式标记（存在即视为已打包）。一行写多个键、或每行一个键都可以。

| 键 | 必需 | 说明 |
|---|---|---|
| `name` | 是 | 列表显示名，可中文 |
| `cat` | 是 | `游戏` / `软件` / `标准` / `运维` / `学习`（写 `工具` 会归一成 `运维`）|
| `desc` | 是 | 商店列表一句话 |
| `version` | 建议 | 只给人看。`ver=` 也认 —— 固件两个键都收（`apppack.c:98`）|
| `build` | 打包器自动写 | 12 位 `YYYYMMDDHHMM` **纯数字**，商店判「有没有新版」只看它 |
| `icon` | 否 | 64×64 1bpp，512 字节（base64 约 684 字符）；尺寸不对会被忽略 |
| `need` | 否 | 目前只认 `wifi`：启动前做联网门禁，120 秒超时放弃 |
| `author` / `date` | 否 | 展示用 |

⚠ **键名中间不能有空格**，值里也不能出现 ` 别的键=` 这种形态 —— 解析器会把空格后
`xxx=` 当作下一个键，导致值被截断（`apppack.c:74-88`）。这也是 `build` 必须写成
12 位连续数字的原因之一。

#### 7.5.2 安装与更新

- **安装 = 复制**，永不自动启动。下载先落到 `/sdcard/store_pkg/<名>.lua`（无 TF 卡则 `/store/<名>.lua`），
  再复制进 **`/store/apps/<目录>/`**（游戏→`app`，运维/工具→`ops`，其余→`app`）。
  覆盖已有版本时走 `.new` → 校验 → 旧版改 `.old` → rename，任一步失败回滚旧版。
- 卸载 = 删已装副本 + 商店源副本，并清主菜单/隐藏标记；资源包则递归删 `target` 目录。
  细节见[应用中心与安装](系统整体设计手册.md)。
- **"有没有新版"按 `build`（构建时间）判断**，不是按 `version`：

| 字段 | 作用 | 谁写 |
|---|---|---|
| `version` | 只给人看（`1` / `1.1` / `1.1.1` 都行） | 手工 |
| `build` | 商店判更新的唯一依据，`YYYYMMDDHHMM` 12 位纯数字 | `tools/pack_app.py` 自动写当前时间 |

- 设备端解析器 **`ver=` 与 `version=` 都接受**（`apppack.c`），但打包脚本与网页上传页按 `version` 走 ⇒ **统一写 `version=`**。
- `need=wifi` 声明后，启动前会做联网门禁（未联网 → 弹窗 → 走系统网络模块 → 连上才启动，120s 超时放弃）。

### 7.6 发布前检查清单

```bash
python3 tools/lua_check.py                     # 语法 + 契约 + 颜色参数
python3 tools/lua_sim.py apps/lua/apps/app/xxx.lua   # 界面自检
python3 tools/pack_app.py pack …               # 生成 LAPP（自动写 build）
python3 tools/sync_apps.py --check-only        # 闸门 + 打包校验，不出镜像
```

- [ ] LAPP 头有 `name` / `cat` / `desc` / `version`，`build` 由打包器写入
- [ ] `cat` 用真实取值：`游戏` / `软件` / `标准` / `运维` / `学习`（写 `工具` 会被归一成 `运维`）
- [ ] 五个契约函数是全局函数，`render` 有 dirty 闸门
- [ ] `on_exit` 覆盖了所有外部资源
- [ ] 没有 `app.tone` / `app.delay` / 同步 HTTP 留在正式路径
- [ ] 自己写了"待真机验证"清单并逐条过

### 7.7 上架与本地发版

- 网页提交：<https://bbk.linit.cn/upload/>（邮箱验证码登录 → 自动读头 → 待审 → 审核通过写 `index.json`）。
  ⚠ 上传页目前**仍按 `version` 校验递增**，走网页提交时 `version` 还要比线上大。
- 本地发版：`python3 tools/sync_apps.py` → 生成 `build/apps_l.bin` → 刷到 apps 分区（改 Lua 不必重编固件）。
- 手工改服务器上的 `bbk/index.json` 后必须 `nginx -s reload`（open_file_cache 会拿旧句柄）。

---

## 8 已知问题与风险（**不是推荐做法**）

> 以下均为源码可验证的现存缺陷或边界，**未修复**。写新应用时请绕开。

| # | 位置 | 问题 | 后果 | 证据 |
|---|---|---|---|---|
| **R1** | `map.lua` | 索引 `files[]` 只处理前 **30** 条（`pick` 取 40）；`scan_pkgs` 只扫目录前 **24** 个条目 | 包/文件多了会**静默漏掉** | `:1439-1441`、`:355-357` |
| **R2** | `map.lua` | 下载中退出只取走在途请求，**不删半包** | 半个 `.bin` 留在 TF；下次覆盖前若被扫到可能读到损坏包 | `:2344-2353` |
| **R3** | `recorder.lua` | `scan()` 无条数上限，逐文件 `size` + `read_at(44B)`，每次进页面/存完都跑 | 录音多了进页面会卡（**待真机验证**具体秒数） | `:305-335` |
| **R4** | `recorder.lua` | "局部重绘"只在 `G.part` 被请求的那一帧生效，其余帧仍整屏重画 | 只省约 1/8 的重绘，不等于"只画变化区" | `:766-786` + `page_lua.c:677` |
| **R5** | `qqtang.lua` | `SAVE.write` 丢弃 `app.nvs.set_str` 返回值，同时清 `dirty` | 写失败**不再重试**，静默丢存档 | `:790-800` |
| **R6** | `img` 池 | 容量 10，满则顶掉 **slot0（最早加载）**，不报错 | 加载顺序不当会让**核心图集**先被淘汰，表现为"某图忽然画不出来" | `luat_lib_img.c:33` |
| **R7** | `map.lua` | `render` 每帧全屏重绘，且 `tile_sync→block_get` 在绘制路径里**同步读块** | 平移出视口时单帧多出一次 SD 读，会掉帧 | `:1744-1804`、`:430-445` |
| **R8** | Lua 数值 | 32 位整数 + float（7 位） | 经纬度/时间戳/CRC 直接当数字会精度丢失或抛错 | `CMakeLists.txt` `LUA_32BITS=1` |
| **R9** | 异步弹窗 | 发起后必须每帧 `dlg.result()`；弹窗期间 `poll` 不执行 | 忘了轮询 = 结果永远收不到，UI 卡在"正在…" | `os_core.c:474`、`:958-969` |
| **R10** | 全项目 | 注释里的性能数字（29ms/6ms/6221 次）是**历史实测**，不是当前承诺 | 不能当性能预算 | 各文件注释 |

另外两条非缺陷但要知道：

- **`app.kbd.STYLE_*` 常量不存在**（源码未注册，注释与旧文档误导）——必须传字符串。
- **`app.dlg.wheel` 不存在**；`app.util.crc32` **存在**（返回 hex 字符串）。

---

## 9 坑清单（速查）

- `local` 只对它**后面**的代码可见；被前向引用的函数必须先 `local f` 声明。
- `local a = 1, b = 2` 语法错；`{...}[k]` 不能直接索引。
- 五个契约函数必须是**全局**，`local function render()` 会静默白屏。
- 颜色参数必须整数：`app.fill(..., app.BLACK)`，`true/false` 会报错。
- `app.touch()` 没按时返回**0 个值**，判 `type(x) == "number"`，不要判 `== nil`。
- `app.zh / zh24 / zhs / zhsm / zhsb` 画 ASCII 是**空白格**；混排用 `app.mix`。
- `app.bmp` 超 2048B **静默不画**；大图用 `bmpr` 或 `img`。
- `app.fs.read` ≤512KB；大文件 `read_at`（≤1MB/次）。
- `app.net.http_get` 只支持 http、≤4KB、阻塞。
- `app.net.fetch_start` 未联网直接返回 false（不是 bug）。
- `app.tone` 阻塞 ms 级，按键音用 `app.tone_async`。
- `app.img` 满 10 张顶最早的；`app.util.after/every` 满 16 个返回 nil。
- `app.db` 读缓冲只有 128B，长字符串用 `app.nvs` 或文件。

---

## 10 附录 A：API 速查（17 模块 / 201 函数 / 30 常量）

> **真相源**：`components/apps/lua_bridge/luat_lib_*.c` 的注册行与实现——参数/返回按实现逐条核对。
> 只查"有哪些 API、注册在哪个文件"时用 `API完整参考手册.md`（`tools/lua_api_doc.py` 自动生成，勿手改）。
> 记号：`必选` 无括号；`[可选=默认值]`；**阻塞**＝会卡住系统主循环（见 §5.3）。

<a id="consts"></a>
### A.1 consts（12 个常量）

| 常量 | 值 | 说明 |
|---|---|---|
| `app.W` / `app.H` | 400 / 300 | 屏幕宽高，与 `ST7305_WIDTH/HEIGHT` 一致 |
| `app.BLACK` / `app.WHITE` | 0 / 1 | 1bpp：0=黑（墨），1=白 |
| `app.UP` / `DOWN` / `LEFT` / `RIGHT` | 1 / 2 / 3 / 4 | 动作码，与内核 `os_action_t` 用 `_Static_assert` 钉死 |
| `app.OK` / `app.CONFIRM` | 5 / 5 | 同义（确认） |
| `app.BACK` / `app.HOME` | 6 / 7 | HOME 被内核直接消费，应用内观测不到 |

```lua
local A_BACK = (type(app.BACK) == "number") and app.BACK or 6   -- 老固件降级保护
```

---

<a id="gfx"></a>
### A.2 gfx（27）— 绘制与字模

`luat_lib_gfx.c`。坐标/尺寸参数**容忍小数**（四舍五入）；**颜色参数必须整数**。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.clear([c=WHITE])` | → 无 | 否 | 清屏；不传=白 |
| `app.px(x,y,c)` | → 无 | 否 | 单像素 |
| `app.fill(x0,y0,x1,y1,c)` | → 无 | 否 | 填充矩形（含端点） |
| `app.rect(x0,y0,x1,y1,c)` | → 无 | 否 | 描边矩形 |
| `app.store_rect(x0,y0,x1,y1,c)` | → 无 | 否 | 与 `rect` 同一实现的旧别名 |
| `app.hline(x0,x1,y,c)` | → 无 | 否 | 横线；**颜色是必选参数** |
| `app.vline(x,y0,y1,c)` | → 无 | 否 | 竖线 |
| `app.text(x,y,s[,inv=false])` | → 无 | 否 | 16×24 ASCII |
| `app.textc(y,s[,inv])` | → 无 | 否 | ASCII 按屏宽居中；**只有 y，没有 cx** |
| `app.textw(s)` | → 宽(px) | 否 | 量文字宽 |
| `app.big(x,y,s[,scale=1,inv])` | → 无 | 否 | 8×12 点阵 ×scale 放大 |
| `app.bigc(cx,y,s[,scale=1,inv])` | → 无 | 否 | 同上，按 cx 居中 |
| `app.zh(x,y,str[,inv])` | → 无 | 否 | **16×16 汉字专用**，ASCII 画空白格 |
| `app.zh24(x,y,str[,inv])` | → 无 | 否 | 24×24 汉字专用 |
| `app.zhs(x,y,str[,inv])` | → 无 | 否 | 12×12 汉字（24 点阵降采样） |
| `app.zhsm(x,y,str[,inv])` | → 结束 x | 否 | 12×12 汉字（同上，带返回） |
| `app.zhsb(x,y,str[,inv])` | → 结束 x | 否 | 16×16 汉字 |
| `app.texts(x,y,s[,inv])` | → 结束 x | 否 | 8×12 ASCII |
| `app.textm(x,y,s[,inv])` | → 结束 x | 否 | 12×18 ASCII |
| `app.textsb(x,y,s[,inv])` | → 结束 x | 否 | 8×16 ASCII（状态栏同款） |
| `app.mix(x,y,s[,inv])` | → 结束 x | 否 | **16px 中英混排**（汉字16 / ASCII 8） |
| `app.mixc(cx,y,s[,inv])` | → 左端 x | 否 | 混排按 cx 居中 |
| `app.mixw(s)` | → 宽(px) | 否 | 混排文字宽 |
| `app.bmp(x,y,w,h,hexstr)` | → 无 | 否 | 1bpp hex 位图；**≤2048B，超限静默不画** |
| `app.bmpr(x,y,w,h,binstr)` | → 无 | 否 | 1bpp 二进制串位图，**无尺寸上限**（地图基线快照用它） |
| `app.poly(raw[,w=1,ct=0,cb=299])` | → 无 | 否 | 一次画整条折线：每点 8 字节(i4 x,i4 y)；w=2 加粗；ct/cb 上下裁剪 |

```lua
app.mixc(app.W // 2, 110, "血 3/6")           -- 混排居中
local x = app.mix(8, 40, "A")                  -- 返回下一字符 x，可接续排版
app.poly(packed, 1, MAP_T, MAP_B)              -- packed = i4 点串；打包写法见 map.lua:657-680
```

注意：汉字族 API 遇 ASCII **不报错、画空白格**；正文字号混排一律 `app.mix`，不要用两次调用拼。

---

<a id="input"></a>
| `app.statusbar()` | → 无 | 否 | 游戏内状态栏：读电池百分比并绘制（`svc_statusbar_draw_game`）|

### A.3 input（10）— 时间、按键、触摸

`luat_lib_input.c`。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.millis()` | → ms | 否 | 开机以来毫秒（时间基准统一用它） |
| `app.delay(ms)` | → 无 | **是** | `vTaskDelay`；只用于调试 |
| `app.key()` | → 动作码 / 无返回 | 否 | 从 24 槽 FIFO 取一条 |
| `app.key_down(a)` | → bool | 否 | 该键是否**按住**；覆盖物理键/手柄/虚拟键；HOME 恒 false |
| `app.keys_mask()` | → 0..63 | 否 | bit0=上 … bit5=返回；多键同按 |
| `app.key_repeat(a[,first=400,rate=90])` | → bool | 否 | 按下沿+自动重复；`rate<20` 夹到 20 |
| `app.touch()` | → x,y / **0 个值** | 否 | 当前按住点；没按返回 0 个值（不是 nil） |
| `app.touch_start()` | → x,y / 0 个值 | 否 | 本次按下的起点 |
| `app.touch_hold()` | → ms | 否 | 按住时长；长按判定用它 |
| `app.redraw()` | → 无 | 否 | "这一帧别跳过"；**不是触发重绘**（render 本来就每帧调） |

```lua
local x, y = app.touch()
if type(x) == "number" then                      -- 别写 x ~= nil
  if app.key_down(app.LEFT) then ... end         -- 连续移动
  local held = app.touch_hold()
end
```

---

<a id="sys"></a>
### A.4 sys（20）— 系统能力

`luat_lib_sys.c`。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.log(...)` | → 无 | 否 | 任意类型拼成一行，串口 `[APP]`；调试主力 |
| `app.tone(f,ms)` | → 无 | **是** | **内部 vTaskDelay(ms)**，⛔ 正式代码别用 |
| `app.tone_async(f,ms[,amp=0])` | → 无 | 否 | 非阻塞单音，amp 1..6000；按键音用它 |
| `app.melody({{f,ms},...})` | → 无 | 否 | ≤48 音；**上一段没播完直接丢弃本次** |
| `app.vibrate(ms[,str=100])` | → 无 | 否 | 震动；马达未就绪静默 |
| `app.volume([0-100])` | → 当前音量 | 否 | 不传=查询；与系统音量同源 |
| `app.mute([bool])` | → bool | 否 | 不传=查询 |
| `app.bt()` | → 0/1/2 | 否 | 未启用 / 射频就绪 / 已连接 |
| `app.exit()` | → 无 | 否 | 请求退出（默认弹「退出程序?」） |
| `app.exit_confirm([on])` | → bool | 否 | 开关退出确认；**顶层函数，不在 `app.sys` 下** |
| `app.sbar(v)` | → 无 | 否 | 状态栏反白；全屏 Lua 页看不到效果（**待真机验证**） |
| `app.sys.battery()` | → mV / 无返回 | 否 | 读缓存值；0 表示未检测到电池 |
| `app.sys.heap_free()` | → 内部 RAM 空闲字节 | 否 | 排查内存问题用 |
| `app.sys.time_ms()` | → ms | 否 | 同 `millis` |
| `app.sys.clock()` | → h,m,s | 否 | 未对时时是开机时长 |
| `app.sys.date()` | → 月,日,星期(1–7) | 否 | `tm_wday+1`，1 = 周日；未对时时取开机时长换算的日期 |
| `app.dlg.toast(msg[,ms=0])` | → 无 | 否 | 0 → 默认 500ms |
| `app.dlg.confirm(msg)` | → 无 | 否 | 结果在 `poll` 里用 `dlg.result()` 取 |
| `app.dlg.list(title,items[,sel=0])` | → 条数 | 否 | 项 ≤60 条、每条 ≤29 字节 |
| `app.dlg.result()` | → nil / -1 / ≥0 | 否 | nil=未选；0=确定；≥1=列表下标 |

```lua
app.tone_async(880, 60, 40)                 -- 按键音
app.dlg.confirm("删除这条录音?")             -- 发出即返回
-- 下一帧（poll）：
local r = app.dlg.result()                  -- nil 还没选 / -1 取消 / 0 确定
```

---

<a id="fs"></a>
### A.5 fs（10）— TF 卡文件

`luat_lib_fs.c`。路径必须带 `/sdcard` 或 `/store` 前缀。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.fs.exists(p)` | → bool | 是 | 目录也算存在（`opendir` 兜底） |
| `app.fs.read(p)` | → string / 无返回 | **是** | **≤512KB**，超限/空文件返回 0 个值 |
| `app.fs.read_at(p,off,len)` | → string / 无返回 | **是** | **len ≤ 1MB**；大文件唯一手段 |
| `app.fs.write(p,data)` | → bool | **是** | 覆盖写；返回值必须判 |
| `app.fs.append(p,data)` | → bool | **是** | 追加 |
| `app.fs.remove(p)` | → bool | 是 | 删文件 |
| `app.fs.rename(a,b)` | → bool | 是 | 同分区改名/移动 |
| `app.fs.size(p)` | → 字节 / 无返回 | 是 | 非普通文件返回 0 个值 |
| `app.fs.mkdir(p)` | → bool | 是 | 等价 `mkdir -p`；已存在返回 true |
| `app.fs.listdir(p)` | → array / 无返回 | **是** | 只返回名字（不含路径），跳过 `.` 开头 |

```lua
if not app.fs.exists(DIR) then app.fs.mkdir(DIR) end
local hdr = app.fs.read_at(path, 0, 44)              -- 只读文件头
local names = app.fs.listdir("/sdcard/Rec")          -- {"a.wav","b.wav"}
```

---

<a id="store"></a>
### A.6 store（9）— NVS 与共享数据库

`luat_lib_store.c`。写 NVS 有 **1 秒/键 节流**（8 槽，淘汰前先落盘）；唯一风险窗口是"写后 1s 内断电"。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.nvs.get_int(k)` | → int / 无返回 | 否 | key ≤15 字节 |
| `app.nvs.set_int(k,v)` | → bool | 否 | 节流写 |
| `app.nvs.get_str(k)` | → string / 无返回 | 否 | 读前先刷该键待落盘值 |
| `app.nvs.set_str(k,v)` | → bool | 否 | 节流写；**返回值必须判** |
| `app.db.set(k,v)` | → bool | 否 | 共享库 `core_db`（跨应用可见） |
| `app.db.get(k)` | → string / nil | 否 | **缓冲 128 字节，值 ≥127B 返回 nil** |
| `app.db.del(k)` | → bool | 否 | 立即落盘 |
| `app.db.seti(k,v)` | → bool | 否 | 存成十进制字符串 |
| `app.db.geti(k[,def=0])` | → int | 否 | 解析失败返回 def |

```lua
app.nvs.set_str("qtadv", "1,0,6,1,1,3")     -- qqtang 的紧凑存档
local lv = app.nvs.get_int("lv") or 1
```

---

<a id="net"></a>
### A.7 net（13）— 网络

`luat_lib_net.c`。异步能力**没有回调**，一律轮询。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.net.is_connected()` | → bool | 否 | |
| `app.net.mirrors()` | → {"url",…} | 否 | 固件内置镜像源清单 |
| `app.net.get_ip()` | → "a.b.c.d" / 无返回 | 否 | |
| `app.net.http_get(host[,port=80,path="/",timeout=5000])` | → body / 无返回 | **是** | 仅 **http**；body **≤4096B**；等 timeout+2s |
| `app.net.fetch_start(url[,timeout=8000,path])` | → bool | 否 | **未联网返回 false**；传 path=流式落盘；**单请求槽** |
| `app.net.fetch_poll()` | 见下 | 否 | 每帧问一次 |
| `app.net.fetch_got()` | → 已收字节 | 否 | 流式进度条用 |
| `app.net.ping(ip[,count=4,timeout=500])` | → table | **是** | 表内 [2i-1]=bool [2i]=rtt(ms) |
| `app.net.port(ip,port[,ms=300])` | → bool | **是** | TCP 端口探测 |
| `app.net.arp(ip)` | → "AA:BB:…" / 无返回 | **是** | 主动 ARP，最长约 260ms |
| `app.net.arp_list()` | → {{ip=,mac=},…} | 否 | 只列已学到的，最多 32 条 |
| `app.net.ipinfo()` | → ip,mask,gw,dns,desc,mac（6 值） | 否 | 无 netif 时返回 0 值 |
| `app.net.vendor(mac)` | → 厂商名 / 无返回 | 否 | OUI 字典 |

`fetch_poll()` 的四种返回：

```lua
"wait"                                   -- 还没回来
"ok", body, status_code                  -- 非流式成功
"ok", nil,  status_code, nbytes          -- 流式成功（body 在文件里）
"err", "",  errcode, errname             -- 失败
"idle"                                   -- 没有请求
```

```lua
if not app.net.is_connected() then app.dlg.toast("未联网"); return end
app.net.fetch_start("https://bbk.linit.cn/bbk/maps.json", 12000)
-- 下一帧起：
local a, body = app.net.fetch_poll()
if a == "ok" and body then local rows = app.json.pick(body, "files", "path,size", 40) end
```

---

<a id="json"></a>
### A.8 json（4）— 只取需要的字段

`luat_lib_json.c`。不建中间树，直接吐 Lua 表。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.json.pick(json,array_key,fields[,max=200])` | → array | 否 | 取顶层数组的每个对象，只保留 fields（逗号分隔，**≤24 个键**，键名 ≤39B）；`max` 仅支持 ≤2048（越界回退 200） |
| `app.json.num(json,key[,def=0])` | → number | 否 | 顶层数字；字符串数字也能转 |
| `app.json.str(json,key)` | → string / 无返回 | 否 | 顶层字符串 |
| `app.json.bool(json,key)` | → bool | 否 | 只判首字符 `t` |

```lua
local rows = app.json.pick(raw, "tiles", "x,y,o,s,po,ps", 1600)   -- map.lua 的做法
local name = app.json.str(raw, "name")
```

限制：只认**顶层键**（没有 `a.b` 路径）；`pick` 只能解"对象数组"，嵌套结构不物化。

---

<a id="wifi"></a>
### A.9 wifi（17）— 连接与无线探测

`luat_lib_wifi.c`。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.wifi.setup()` | → 无 | 否 | **打开系统网络模块的连接界面**（推荐入口） |
| `app.wifi.connect(ssid[,pass=""])` | → bool | 否 | 保存并异步连接 |
| `app.wifi.connected()` | → bool | 否 | |
| `app.wifi.connecting()` | → bool | 否 | |
| `app.wifi.cur_ssid()` | → string / 无返回 | 否 | 已连接的 SSID |
| `app.wifi.clear_saved()` | → 无 | 否 | 清凭据 |
| `app.wifi.scan()` | → bool | 否 | 启动扫描后**立即返回** |
| `app.wifi.scan_done()` | → bool | 否 | 轮询扫描完成 |
| `app.wifi.ap_count()` | → int | 否 | |
| `app.wifi.ap_info(i)` | → rssi,ssid,ch,auth,mac（5 值） | 否 | 越界返回 0 值 |
| `app.wifi.init()` | → bool | 否 | 被动监听前置 |
| `app.wifi.probe_init()` | → bool | 否 | promiscuous + 跳频任务 |
| `app.wifi.probe_stop()` | → 无 | 否 | |
| `app.wifi.probe_count()` | → int | 否 | |
| `app.wifi.probe_info(i)` | → mac,rssi,ch,ssid,count（5 值） | 否 | |
| `app.wifi.ch_stat()` | → 14 项 {beacon=,probe=} | 否 | 2.4G 信道 1..14 统计 |
| `app.wifi.release()` | → 无 | 否 | 关被动监听，**退出必调** |

```lua
app.wifi.scan()
-- 每帧：if app.wifi.scan_done() then for i=1,app.wifi.ap_count() do ... end end
app.wifi.setup()      -- 需要用户连网时，交给系统流程
```

---

<a id="hid"></a>
### A.10 hid（11）— USB 键鼠 / CDC

`luat_lib_hid.c`。先 `start()`，退出 `stop()`。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.hid.start()` | → bool | 否 | |
| `app.hid.stop()` | → 无 | 否 | 内部先释放所有按键 |
| `app.hid.connected()` | → bool | 否 | 对端已枚举 |
| `app.hid.running()` | → bool | 否 | 栈已启动 |
| `app.hid.key([mod=0],kc)` | → 无 | 否 | **mod 在前、kc 在后且必选**（易错） |
| `app.hid.key_str(s)` | → 无 | 否 | 仅支持字母/数字/空格 `. - : /`，其余静默跳过 |
| `app.hid.mouse_move([dx=0,dy=0])` | → 无 | 否 | int8，±127 |
| `app.hid.mouse_click([btn=1])` | → 无 | 否 | 1=左 2=右 4=中 |
| `app.hid.shortcut(name)` | → 1 / 无返回 | 否 | 仅 14 个：CTRL+C/V/A/X/Z/Y/S/F/P、CTRL+TAB、ALT+TAB、WIN+R/E/L |
| `app.hid.cdc_read([max=256])` | → string / 无返回 | 否 | max 夹到 1..1024；无数据返回 0 值 |
| `app.hid.cdc_connected()` | → bool | 否 | |

---

<a id="kbd"></a>
### A.11 kbd（13 + 18 常量）— 软键盘

`luat_lib_kbd.c` + `keyboard.h`。**只管布局/命中/字符，文本缓冲自己维护**。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.kbd.use(style[,…])` | → bool | 否 | style 只认首字母：`std`/`edit`/`rows`/`dialog`；dialog 需 (x,y,w,row_h)，rows 需 (rows[,y]) |
| `app.kbd.reset()` | → 无 | 否 | 复位布局表 |
| `app.kbd.draw([press=-1])` | → 无 | 否 | 用当前几何画；press=按下键索引 |
| `app.kbd.hit(tx,ty)` | → idx / -1 | 否 | 屏幕坐标 → 键索引 |
| `app.kbd.geom()` | → {style,x,y,w,row_h,rows} | 否 | |
| `app.kbd.height()` | → px | 否 | std=145，edit=19×5 |
| `app.kbd.count()` | → 键数 | 否 | |
| `app.kbd.press(i)` | → 动作码 | 否 | **有副作用**：改 caps/shift/fn |
| `app.kbd.char(i)` | → "c" / nil | 否 | 只有普通字符键有值 |
| `app.kbd.label(i)` | → string | 否 | 随 Fn/Caps/Shift 联动 |
| `app.kbd.layer_on(i)` | → bool | 否 | 是否处于激活层 |
| `app.kbd.get([k="caps"])` | → bool / int | 否 | 按首字母：`s`=shift、`f`=fn_mode、其余=caps |
| `app.kbd.set(k,v)` | → 无 | 否 | 同上 |

常量（18）：`CHAR0 ENTER1 BKSP2 SPACE3 ESC4 TAB5 UP6 DOWN7 LEFT8 RIGHT9 CAPS10 SHIFT11 FN12 CTRL13 ALT14`、`ROW=29`、`EDIT_ROW=19`、`ROWS=5`。

⛔ **没有 `app.kbd.STYLE_STD/EDIT/ROWS/DIALOG`**（源码未注册），样式一律传字符串。

```lua
app.kbd.use("rows", 4)
local i = app.kbd.hit(tx, ty)
if i >= 0 then
  local act = app.kbd.press(i)
  local ch  = app.kbd.char(i)
  if act == app.kbd.CHAR and ch then buf = buf .. ch
  elseif act == app.kbd.BKSP then buf = buf:sub(1, -2)
  elseif act == app.kbd.ENTER then commit()
  elseif act == app.kbd.ESC then cancel() end
end
```

---

<a id="img"></a>
### A.12 img（5）— 1bpp 图片资源池

`luat_lib_img.c`。池容量 **10**，满则顶掉 slot0（最早加载）**且不报错**。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.img.load(path[,opaque=false])` | → id(1..10) / nil | **是** | 仅 1bpp BMP；≤4MB；宽 ≤2048；0 位=透明；opaque=true 时 0 位也写白 |
| `app.img.blit(id,sx,sy,dx,dy,w,h[,mode=0])` | → 无 | 否 | 从源图取块贴屏；w ≤512；越界自动裁剪；mode=1 位=1 写白（mask 用） |
| `app.img.size(id)` | → w,h / nil | 否 | |
| `app.img.free(id)` | → 无 | 否 | |
| `app.img.freeall()` | → 无 | 否 | **退出必调** |

```lua
local tile = app.img.load("/store/qqtang/tile.bmp")
app.img.blit(tile, 20, 0, dx, dy, 20, 20)
```

---

<a id="gps"></a>
### A.13 gps（6）— 定位

`luat_lib_gps.c`。数值**全部整数定标**（32 位 + float 的必然选择）。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.gps.start()` | → bool | 同步（init→唤醒→起任务） | 幂等；别在 `poll` 里重复调 |
| `app.gps.stop()` | → bool(true) | 同步 | 停任务+送待机+卸 UART；框架退出时也兜底 |
| `app.gps.fix()` | → table / nil | 否 | 见下 |
| `app.gps.sats()` | → {{sys,prn,el,az,snr,used},…} | 否 | sys：0 GPS / 1 GLO / 2 GAL / 3 BDS |
| `app.gps.demo([on])` | → bool | 否 | 不传=查询；演示数据不读 UART |
| `app.gps.reset()` | → 无 | 否 | 清状态与统计 |

`fix()` 字段：`valid demo has_time has_date`、`lat_e6 lon_e6`(微度)、`kmh10 mps100 crs10`、`alt_m alt_dm hdop100`、`sats quality dim`、`hh mm ss yy mo dd`、`age_ms sentences badcs rx ubx age_rx baud link proto`。

```lua
local f = app.gps.fix()
if f and f.valid then
  local lat, lon = f.lat_e6 / 1e6, f.lon_e6 / 1e6      -- 只在显示时转浮点
end
```

---

<a id="espnow"></a>
### A.14 espnow（12）— 无路由器点对点

`luat_lib_espnow.c`。单包 **≤250 字节**；两端需同信道。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.espnow.start()` | → bool | 否 | 会拉起射频 |
| `app.espnow.stop()` | → 无 | 否 | 只关 ESP-NOW，不动用户 WiFi |
| `app.espnow.ready()` | → bool | 否 | |
| `app.espnow.mac()` | → "aa:bb:…" / nil | 否 | 本机 MAC = STA MAC |
| `app.espnow.channel([n])` | → 当前信道 | 否 | 传整数则切换（1..13）；联网时切会掉线 |
| `app.espnow.add(mac[,key])` | → bool | 否 | key 必须**正好 16 字节**，否则按不加密 |
| `app.espnow.del(mac)` | → bool | 否 | |
| `app.espnow.send(mac,data)` | → bool | 否 | 1..250 字节 |
| `app.espnow.bcast(data)` | → bool | 否 | 1..250 字节 |
| `app.espnow.recv()` | → mac,data / nil | 否 | 非阻塞取一条 |
| `app.espnow.pending()` | → int | 否 | 队列深度 |
| `app.espnow.stats()` | → {rx,drop,tx,fail,rssi} | 否 | 排障 |

---

<a id="wiki"></a>
### A.15 wiki（10）— 离线维基

`luat_lib_wiki.c`。库在 `/sdcard/wiki/<库名>/`。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.wiki.open([dir])` | → 篇数 / nil,"原因" | **是**（内含 1.2s 挂载等待） | 省略 dir 自动找第一个库 |
| `app.wiki.info()` | → {count,name,lines} / nil | 否 | |
| `app.wiki.search(q[,max=20])` | → {{id,title},…} | 是 | 标题**前缀**匹配、大小写敏感；max ≤50 |
| `app.wiki.search_py(q[,initials=false,max=20])` | → {{id,title},…} | 是 | 首字母 / 全拼 |
| `app.wiki.title(id)` | → string / nil | 否 | |
| `app.wiki.titles(from[,n=16])` | → {{id,title},…} | 是 | n ≤32 |
| `app.wiki.load(id)` | → 行数 / nil,"原因" | 是 | 错误原因已翻中文 |
| `app.wiki.line(i)` | → string | 否 | **0 基**、非 NUL 结尾（带长度） |
| `app.wiki.lines()` | → int | 否 | |
| `app.wiki.close()` | → 无 | 否 | **退出必调** |

---

<a id="util"></a>
### A.16 util（17）— 定时器 / 随机 / 编码 / 数学

`luat_lib_util.c`。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.util.after(ms,fn)` | → id / nil | 否 | **16 槽**，满了返 nil |
| `app.util.every(ms,fn)` | → id / nil | 否 | 同上，周期 |
| `app.util.cancel(id)` | → 无 | 否 | 越界安全 |
| `app.util.rand([min,]max)` | → int | 否 | 无参=1..100；单参=1..n；双参可反序 |
| `app.util.randf()` | → [0,1) float | 否 | 24 位精度 |
| `app.util.crc32(s)` | → "xxxxxxxx" | 否 | **8 位十六进制字符串**（不是整数） |
| `app.util.b64(s)` | → string / nil | 否 | base64 编码 |
| `app.util.unb64(s)` | → string / nil | 否 | 解码，格式错返回 nil |
| `app.util.serialize(t)` | → string | 否 | nil/bool/number/string/table；不支持环；深度 16 |
| `app.util.unserialize(s)` | → 值 / nil | 否 | 失败整串返回 nil（不抛错） |
| `app.util.epoch()` | → "1700000000" | 否 | **字符串**（int32 装不下 10 位） |
| `app.util.sleep(ms)` | → 无 | **是** | ms≥60000 不睡 |
| `app.util.clamp(v,lo,hi)` | → number | 否 | |
| `app.util.lerp(a,b,t)` | → number | 否 | |
| `app.util.dist(x1,y1,x2,y2)` | → number | 否 | |
| `app.util.round(v)` | → int | 否 | 四舍五入 |
| `app.util.sign(v)` | → -1/0/1 | 否 | |

```lua
local id = app.util.after(500, function() app.dlg.toast("好了") end)
if not id then app.log("定时器槽满了") end
local s = app.util.serialize({lv = 3, items = {"a","b"}})
```

---

<a id="rec"></a>
### A.17 rec（17）— 录音、WAV 与音频文件播放

`luat_lib_rec.c`。底层是独立任务（CPU1），调用**快速返回**，进度靠轮询。

| API | 参数 → 返回 | 阻塞 | 说明 |
|---|---|---|---|
| `app.rec.start(path[,rate=16000])` | → 0 成功 / <0 失败 | 否 | -1 已在录 / -2 麦失败 / -3 建不了文件 / -4 建不了任务 |
| `app.rec.stop()` | → 已录 ms | 否 | 底层异步收尾写 WAV 头 |
| `app.rec.active()` | → bool | 否 | |
| `app.rec.ms()` | → int | 否 | |
| `app.rec.bytes()` | → int | 否 | 已写字节（判"卡住"用） |
| `app.rec.peak()` | → int | 否 | 峰值电平 |
| `app.rec.mic_available()` | → bool | 否 | 进页面先问，无麦给提示 |
| `app.rec.spec(bin)` | → 0..100 | 否 | **bin 从 1 起** |
| `app.rec.spec_bins()` | → int | 否 | |
| `app.rec.play(path)` | → 0 成功 | 否 | 只播 **WAV**；-1 上一段没停 / -4 已静音 |
| `app.rec.stop_play()` | → 无 | 否 | |
| `app.rec.playing()` | → bool | 否 | |
| `app.rec.play_ms()` | → int | 否 | 按墙钟推进 |
| `app.rec.play_total_ms()` | → int | 否 | 0 = 无可播数据 |
| `app.rec.mplay(path)` | → int | 否 | 播放音频文件（`audio_player_play`，不限 WAV）|
| `app.rec.mplaying()` | → bool | 否 | `audio_player_is_running` |
| `app.rec.mstop()` | → 无 | 否 | `audio_player_stop` |

```lua
if app.rec.mic_available() and app.rec.start("/sdcard/Rec/a.wav", 16000) == 0 then
  -- 每帧轮询 app.rec.ms() / app.rec.bytes()
end
```

---

## 本文的证据强度

- 性能数字除注明外来自**源码注释或静态推断**，标"待真机验证"的条目没有实测。
- `app.sbar` 在全屏 Lua 页的可见效果**未验证**。
- 缺陷表 §8 的 R1–R10 是源码可验证的现存问题，行号会随源码漂移，**以函数名为准**。
