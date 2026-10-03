# LinTOS Lua 单文件程序规则（V1.2 深度整理）

> **单文件 Lua 程序**：把一个 `.lua` 丢进 TF 卡（或从应用商店装）就能跑，**免重编固件**。
> 本文是该形态的**唯一规范**：契约、完整 API、UI 工具层、发布流程、常见坑。
> 原生 C 应用见 [App制作规则.md](App制作规则.md)；分层/构建见 [架构与工程约定.md](架构与工程约定.md)。
>
> ⚠ API 清单以固件注册块为准：`components/apps/pages/page_lua.c`。文档与它冲突时**以代码为准**并回来修本文。

---

## 0. 一句话流程

```
写 .lua → 丢 /sdcard/apps/{engine|app|ops}/  （或放商店源后从商店装）
        → 应用中心对应分类自动出现 → 点开运行 → on_exit 释放硬件
```

**核心原则（最重要）**：**核心能力进固件、只对脚本开放 API，脚本绝不手搓核心动作。**

| 层 | 职责 | 例子 |
|---|---|---|
| 固件 (C) | 硬件 / 网络 / HID / 文件 / 持久化 / 算法 | WiFi 扫描与统计、键鼠、存储、NVS、中文点阵 |
| 脚本 (Lua) | 界面布局、状态机、存信息、少量计算 | 页面与 Tab、按钮、进度条、触控板、游戏逻辑 |

程序只该做三件事：**把数据画出来、把输入接起来、把结果存下来**。
> 边界：**画图不进固件**（`line/circle` 等图元由脚本 UI 层实现），但**图元算法与字形数据进固件**。

---

## 1. 最小可运行模板（复制即用）

```lua
-- myapp.lua — 最小单文件程序
local T = { n = 0 }              -- 私有状态：用局部表，避免污染全局

function on_enter()              -- 进入：读配置 / 准备状态
  T.n = app.nvs.get_int("myapp", "n") or 0
end

function render()                -- 整页渲染：先清屏再全画
  app.clear()
  app.zh(8, 32, "我的程序", false)          -- 中文（固件 16px 字库）
  app.text(8, 64, "count = " .. T.n)        -- ASCII 8x16
  app.text(8, 280, "按 BACK 返回")
end

function action(a)               -- 按键 / 手柄动作
  if a == app.CONFIRM then
    T.n = T.n + 1
    app.nvs.set_int("myapp", "n", T.n)
    app.redraw()
  elseif a == app.BACK then
    app.exit()                   -- 退出本程序（回应用中心）
  end
end

function poll() end              -- 每帧（可选；动画/轮询才用）
function on_exit() end           -- 退出：释放用过的硬件
```

---

## 2. 契约回调（**签名必须与下面完全一致**）

| 回调 | 时机 | 关键约定 |
|---|---|---|
| `function on_enter()` | 进入程序 | 初始化状态、读 NVS；**不要在此长时间阻塞** |
| `function render()` | 需要重绘时 | **先 `app.clear()`**；一帧画完整；画布 400×300、1bpp 白底黑字；**y<24 是状态栏区，避开** |
| `function poll()` | 每帧调用 | 动画/定时/异步取数据；不需要就不定义 |
| `function action(a)` | 有按键/手柄动作 | `a` 见 §3.1。**返回 `true` = 已消费**（内核不再处理）；`false`/`nil` 交内核 |
| `function on_exit()` | 退出程序 | **必须释放硬件**：`app.wifi.release()` / `app.hid.stop()`；关文件 |

**BACK 语义**：程序内若开了子窗口/弹窗，`action(app.BACK)` 里 `return true` = "我关掉了子窗口，别退出程序"；
真要退出用 `app.exit()`（或 `return false` 让内核处理）。

> 只定义了的回调才会被调用；`poll` / `on_exit` 可省略。

---

## 3. 完整 API 全表

### 3.1 常量
| 常量 | 值 | 说明 |
|---|---|---|
| `app.UP` / `app.DOWN` / `app.LEFT` / `app.RIGHT` | 1 / 2 / 3 / 4 | 方向 |
| `app.OK` / `app.CONFIRM` | 5 | 确认（同值） |
| `app.BACK` | 6 | 返回 |
| `app.HOME` | 7 | 回桌面 |

### 3.2 绘制（顶层，直接 `app.xxx(...)`）
| API | 说明 |
|---|---|
| `app.clear()` | 整屏清白 |
| `app.px(x, y, black)` | 单像素（`black` 真=黑） |
| `app.fill(x0, y0, x1, y1, black)` | 实心矩形 |
| `app.rect(x0, y0, x1, y1, black)` | 矩形边框（别名 `app.store_rect`） |
| `app.hline(x0, x1, y)` / `app.vline(x, y0, y1)` | 水平 / 垂直直线 |
| `app.text(x, y, s)` | ASCII 8×16 |
| `app.textc(cx, y, s)` | ASCII 居中 |
| `app.textw(s)` | 字符串像素宽（排版用） |
| `app.big(x, y, s, scale)` | ASCII 整数倍放大 |
| `app.bigc(cx, y, s, scale)` | 放大 + 居中 |
| **`app.bmp(x, y, w, h, hex)`** | 贴 1bpp 图标：**hex 串**，`1=黑 / 0=透明`，上限 **2048B（约 128×128）** |
| **`app.bmpr(x, y, w, h, raw)`** | 贴大位图：**二进制串**（不是 hex），`1=黑 / 0=透明`，**无大小上限**，长度需 ≥ `ceil(w/8)*h`。整幅重绘用（白板基线快照 336×268 = 11 KB 就是走它） |
| **`app.zh(x, y, s, inv)`** | **中文**（固件 16px 字库，逐字绘制，中英混排安全） |
| **`app.zhs(x, y, s, inv)`** | 中文绘制并**返回结束 x**（排版/换行用） |

> **中英混排分两次调用**（ASCII 用 `app.text`，中文用 `app.zh`）；
> `app.zh` 内部按首字节判长度，**不会中英错位**。

### 3.3 时间 / 输入 / 声音 / 震动 / 退出
| API | 说明 |
|---|---|
| `app.millis()` | 开机毫秒（计时/动画） |
| `app.key(code)` | 查询按键是否按下（`code` 见 3.1） |
| `app.delay(ms)` | 阻塞延时（**慎用**，会卡界面） |
| `app.tone(freq, ms)` | 蜂鸣（Hz / ms） |
| `app.vibrate(ms)` | 震动 |
| `app.exit()` | 退出程序 |
| `app.redraw()` | 请求重绘（改状态后调用） |
| `app.sbar(inv)` | **状态栏整条反白**（暗底页面用，如雷达/黑底工具页）。⚠ 必须自己 `on_exit()` 里还原 `app.sbar(false)`，否则带到下一页 |
| `app.dlg.toast(msg[, ms])` | **走系统弹窗模块的提示**（居中、2px 框、**默认 0.5s 自动关闭**）。⚠ **提示一律用它，不要自绘** —— 自绘的框和系统弹窗长得不一样，改框架还得逐处改。`ms=0` 用默认时长 |
| `app.sys.clock()` | **本地墙上时钟** → `(时, 分, 秒)`（与状态栏同源 `time()/localtime`）。给"按时间命名存档"用；系统时间未同步时是错值 |
| `app.sys.battery()` / `app.sys.heap_free()` / `app.sys.time_ms()` | 电池 mV / 空闲堆 / 开机毫秒 |

### 3.4 触摸
| API | 说明 |
|---|---|
| `app.touch()` | 返回 `x, y`（当前按下）或 `nil`（未按下）；逻辑坐标 0..399 / 0..299 |
| `app.touch_start()` / `app.touch_hold()` | 本次触摸是否"刚按下" / "长按保持" |

```lua
local x, y = app.touch()
if x and y >= 200 then ... end      -- 只响应下半屏
```

### 3.5 系统 / 文件 / 键值 / 共享库
| 命名空间 | API | 说明 |
|---|---|---|
| `app.sys` | `battery()` | 电池电压 mV |
| | `heap_free()` | 空闲内部 RAM 字节 |
| | `time_ms()` | 同 `app.millis()` |
| `app.fs` | `exists(p)` / `read(p)` / `write(p, d)` / `append(p, d)` / `remove(p)` | TF 卡 FAT，路径从 `/sdcard` 起 |
| | `listdir(dir)` | 返回文件名数组 |
| `app.nvs` | `get_int(sp,k)` / `set_int(sp,k,v)` / `get_str(sp,k)` / `set_str(sp,k,v)` | 掉电保留；`sp` 用程序自己的短名 |
| `app.db` | `set(k,v)` / `get(k)` / `seti(k,v)` / `geti(k)` / `del(k)` | **跨程序共享**（NVS `core_db`），多程序交换少量状态 |

> `app.fs` 依赖 TF 已挂载，用前建议 `app.fs.exists("/sdcard")`。

### 3.6 网络 `app.net`（需系统已联网）
| API | 说明 |
|---|---|
| `is_connected()` | 是否已联网 |
| `get_ip()` | 本机 IP 字符串 |
| `http_get(host, port, path, timeout_ms)` | 简易 HTTP GET，返回 body 或 `nil` |
| `ping(ip, count, timeout_ms)` | ICMP ping，返回 `{ok, rtt_ms, ok, rtt_ms, ...}` 两两成对 |
| **`port(ip, port[, ms])`** | **TCP 端口探测**（V1.2 新增）：返回 `true`=开放 / `false`=关闭或超时。默认 300ms |
| **`arp(ip)`** | **主动 ARP 解析目标 MAC**（V1.2 新增）：返回 `"AA:BB:CC:DD:EE:FF"` 或 `nil`（最多等 ~260ms） |
| **`arp_list()`** | **已学到的局域网主机**（V1.2 新增）：返回 `{{ip="192.168.1.1", mac="AA:BB:.."}, ...}` |
| **`ipinfo()`** | **本机网络信息**：返回 `ip, 掩码, 网关, DNS, 接口名, 本机MAC` 共 **6 值**（未联网返回 0 个值）。与 `page_netdect` 的 `nd_get_local()` 同源 |
| **`vendor(mac)`** | **MAC → OUI 厂商名**（英文如 `"TP-Link"`）；未命中返回 `nil`。要中文显示自行映射（`netdect.lua` 有现成的 `VENDOR_ZH` 表） |

> ⚠ **没有 `app.net.init()`** —— 联网由系统 WiFi 服务管理，程序只读状态 / 发请求。
> ✅ OUI 厂商查询**已开放**（`app.net.vendor`）：原先担心 232KB 字典撑爆 app 分区，2026-09-22 分区调整
> （factory 5MB）后已放得下，`page_lua.c` 经 `os_net_mac_vendor()` 调用它。

### 3.7 无线探测 `app.wifi`（**不需要联网**，网络扫描类工具的主力）
| API | 说明 |
|---|---|
| `init()` / `release()` | 申请 / **释放** WiFi 硬件（`on_exit` 必须 `release()`） |
| `scan()` / `scan_done()` | 发起扫描 / 扫描是否完成 |
| `ap_count()` | 扫到的 AP 数 |
| `ap_info(i)` | 第 i 个 AP：`rssi, ssid, channel, authmode, mac` |
| `ch_stat()` | 14 信道统计 `{{beacon=n, probe=n}, ...}` —— **热力图数据源** |
| `probe_init()` / `probe_stop()` / `probe_count()` / `probe_info(i)` | 探针嗅探：开始 / 停止 / 数量 / 第 i 项（`mac, rssi, ch`） |
| **`cur_ssid()`** | 已连接 WiFi 的 SSID；未连接返回 `nil` |
| **`connecting()`** | 是否正在连接（给"连接中..."提示用） |
| **`connect(ssid[, pass])`** | **保存并连接**该 WiFi（返回 bool）。密码页/自动重连都用它 |
| **`clear_saved()`** | 清空已保存的 SSID/密码（连接失败后"清空重输"用） |

### 3.8 电脑键鼠 `app.hid`（USB-HID，直控 PC）
| API | 说明 |
|---|---|
| `start()` / `stop()` | 启动 / **停止** HID 模式（`on_exit` 必须 `stop()`） |
| `connected()` | PC 是否已枚举连接 |
| `key(mod, hidcode)` | 敲一键（`mod`：LCTRL=1 LSHIFT=2 LALT=4 LGUI=8） |
| `key_str(s)` | 依次敲 ASCII 字符串（自动处理 Shift / 大写） |
| `mouse_move(dx, dy)` | 相对移动（-127..127） |
| `mouse_click(btn)` | 点击一次（左=1 右=2） |
| `shortcut(name)` | 合成快键：`CTRL+C/V/A/X/Z/Y/S/F/P/TAB`、`ALT+TAB`、`WIN+R/E/L` 等 14 项 |
| **`cdc_read([maxlen])`** | **读 PC 经 USB CDC 发来的数据**（V1.2 新增）：返回 `string` 或 `nil`(当前无数据)。默认 256B，上限 1024B |

> 新增 `cdc_read` 后，**终端 / 日志诊断类应用可以完全用 Lua 写**（不需要固件侧先做）。

> `key()` / `mouse_click()` **内部已含按下+释放** —— **没有** `key_release()` / `mouse_release()`。
> 发命令示例：`app.hid.shortcut("WIN+R")` 然后 `app.hid.key_str("cmd")`。

---

## 4. Lua UI 工具层（纯 Lua，复制即用）

> 固件只给**图元与字形**（`px/fill/rect/hline/vline/text*/big*/zh*`）与**数据**；
> `line/circle` 这类由脚本自备。以下片段拷到 `.lua` 开头即可复用。

```lua
local U = {}
function U.line(x0,y0,x1,y1)                 -- 直线 (Bresenham)
  local dx,dy = math.abs(x1-x0), math.abs(y1-y0)
  local sx,sy = (x0<x1) and 1 or -1, (y0<y1) and 1 or -1
  local err = dx-dy
  while true do
    app.px(x0,y0,true)
    if x0==x1 and y0==y1 then break end
    local e2 = err*2
    if e2 > -dy then err = err-dy; x0 = x0+sx end
    if e2 <  dx then err = err+dx; y0 = y0+sy end
  end
end
function U.circle(cx,cy,r,fill)              -- 圆 / 实心圆
  local x,y,d = r,0,1-r
  while x >= y do
    if fill then
      app.hline(cx-x,cx+x,cy+y); app.hline(cx-x,cx+x,cy-y)
      app.hline(cx-y,cx+y,cy+x); app.hline(cx-y,cx+y,cy-x)
    else
      app.px(cx+x,cy+y,true); app.px(cx-x,cy+y,true)
      app.px(cx+x,cy-y,true); app.px(cx-x,cy-y,true)
      app.px(cx+y,cy+x,true); app.px(cx-y,cy+x,true)
      app.px(cx+y,cy-x,true); app.px(cx-y,cy-x,true)
    end
    y = y+1
    if d < 0 then d = d+2*y+1 else x = x-1; d = d+2*(y-x)+1 end
  end
end
function U.bar(x,y,w,h,ratio)                -- 进度条 (0..1)
  app.rect(x,y,x+w,y+h,true)
  local n = math.floor((w-2)*math.max(0,math.min(1,ratio)))
  if n > 0 then app.fill(x+1,y+1,x+n,y+h-1,true) end
end
function U.signal(x,y,bars,n)                -- 信号条 (亮 n 根)
  for i=1,bars do
    local bh = i*3
    if i <= n then app.fill(x+(i-1)*4, y-bh, x+(i-1)*4+2, y, true) end
  end
end
function U.tabrow(x0,y,tabs,sel)             -- 顶部 Tab 行
  local x = x0
  for i,t in ipairs(tabs) do
    local w = app.textw(t) + 10
    if i == sel then app.fill(x,y,x+w,y+18,true); app.textc(x+w/2,y+2,t)
    else app.rect(x,y,x+w,y+18,true); app.textc(x+w/2,y+2,t) end
    x = x + w + 4
  end
end
return U
```

> 想跨程序共享：存成 `/sdcard/apps/_ui.lua`，程序里 `local U = dofile("/sdcard/apps/_ui.lua")`（需 TF 已挂载）。
> ⚠ `_ui.lua` **不要**放进 `{cat}` 分类目录，否则会被当成一个"应用"列出来。

---

## 5. 发布与安装（两条路，都不需要重编固件）

### 5.1 路线 A：TF 卡直接丢（最快，开发时用）
```
/sdcard/apps/app/我的工具.lua      ← 应用中心「应用」分类
/sdcard/apps/ops/xxx.lua           ← 「运维」分类
/sdcard/apps/engine/xxx.lua        ← 「引擎」分类
```
放入后进应用中心对应分类即出现。

### 5.2 路线 B：走商店（模拟下载安装）
1. `.lua` 放到**商店源**：`/store`（SPIFFS 根，随固件打包）或 `/sdcard/store_pkg`
2. 应用中心 → **商店**：列出该包 → 点击 → 确认 → 复制到 `/store/apps/app/`（或 `/sdcard/apps/app/`）
3. 复制完成即"已安装"，同时出现在对应分类里，可直接运行；长按可卸载（删副本）

> 商店是**本地源 + 复制安装**：不联网、无 WASM 运行时；`wasm:` 只是预留 id 前缀命名。

### 5.3 显示名规则（**没有映射表**）
- 显示名 = **文件名去掉 `.lua`**；TF 来源会加书名号区分，如 `「我的工具」`。
- **要中文名就给文件起中文名**（FAT 支持 UTF-8）。
- 旧文档写的 `s_pkg_meta[]` 映射表**已于 2026-09-19 移除**。

---

## 6. 常见坑

| 坑 | 现象 | 正确做法 |
|---|---|---|
| `render` 不打底 | 上次画面残留 / 花屏 | 开头先 `app.clear()` |
| 画到状态栏 | 与时间/电量重叠 | y 从 **24** 起画 |
| 中英混排错位 | 中文后接英文错行 | 中文 `app.zh`、英文 `app.text`，分两次调用 |
| 退出不释放硬件 | WiFi/HID 被占，别的程序用不了 | `on_exit()` 里 `wifi.release()` / `hid.stop()` |
| 滥用 `app.delay` | 界面卡死、按键无响应 | 用 `poll()` + `app.millis()` 非阻塞计时 |
| 回调签名写错 | 回调根本不触发 | 必须是全局函数：无参（`action` 除外的单个 `a`） |
| 忘 `app.redraw()` | 改了状态画面不变 | 状态变更后调用 |
| `_ui.lua` 放错位置 | 应用列表多出一个"程序" | 公共库放 `/sdcard/apps/` 根，不放 `{cat}` 子目录 |
| **`app.mix` 当 `app.texts` 用** | 选中行/反白文字**整行看不见** | `app.mix`/`app.mixc` 是**透明字**（只翻前景色，不铺底）→ `inv=true` 在白底上就是白字画白底。反白必须先 `app.fill(x, y, x2, y2, app.BLACK)` 铺底再画（`app.texts`/`app.textsb`/`app.zhs` 不透明，会自动铺格底，可直接 `inv`） |
| `app.mixc` 当 `app.textc` 用 | 文字跑到屏幕中间 / 坐标错位 | `app.mixc(cx, y, s[, inv])` 按**给定 cx** 居中；`app.textc(y, s[, inv])` 没有 cx，按**屏宽**居中 |
| **`app.fs.listdir` 用 `pairs()` 迭代** | 目录列成 `1 2 3 …`，或"一个存档都找不到" | 它返回的是**数组**（`1..n` = 条目名），不是 `{名=值}` 表。`pairs()` 拿到的是**下标**。必须 `for i = 1, #list do local name = list[i]`（闸门已加自动检查） |
| `s:find("%.bmp", 1, true)` | 永远匹配不到 | `plain=true` 时 `"%.bmp"` 是含 `%` 的**字面量**。判后缀用 `s:sub(-4):lower() == ".bmp"` |
| **`local function f()` 放在调用点之后** | 调用时报 `attempt to call a nil value` / `index a nil value` | Lua 的 `local` 只对**声明之后**的代码可见。函数引用它**后面**才声明的 local（含 `local G = {}`、工具函数）会变成**全局查找** → 运行时 nil。规则：被引用的 local 一律放在使用点之前 |
| 自定义 1bpp 文件格式 | 自己的读写在 Lua 内自洽，但和固件/C 版互不通用 | 要跟既有格式互操作就**逐字节照抄**：块偏移、调色板、位序（画布 `1=墨` ↔ BMP `bit1=白`，是**反的**）、行对齐填充、甚至头部里没用的字段（C 版 `xPelsPerMeter`/`clrImportant` 都留 0）。判据：**载入→保存 逐字节 md5 相同** |

---

## 7. 三种应用形态（什么时候用 Lua）

| 形态 | 载体 | 编译期 | 适合 |
|---|---|---|---|
| **C 内置应用** | `components/apps/pages/page_xxx.c` | 需重编固件 | 高频 / 重逻辑 / 需新增固件能力（见《App制作规则》） |
| **Lua 单文件**（本文） | `/sdcard/apps/{cat}/x.lua` 或商店源 | **免重编** | 界面为主、能力桥里已有、想快速迭代与分发 |
| 下载型（未实现） | 预留 `wasm:` 命名 | — | 需先定运行时与沙箱 |

**判断标准**：需要的能力**桥里已有** → Lua；需要**新固件能力** → 先加桥（`page_lua.c`）再用 Lua，或直接做 C 应用。

---

## 8. 参考

| 位置 | 内容 |
|---|---|
| `components/apps/pages/page_lua.c` | Lua 桥 C 实现（**API 权威来源**）。加新 API 就改这里 |
| `backup_20260919_power/lua_apps_removed/` | 早期 Lua 样例（calc / pomodoro / morse / netdect / diag / term / wb / …），可作排版与状态机写法参考 |
| [App制作规则.md](App制作规则.md) | 原生 C 应用规范（要新增固件能力时看） |
| [架构与工程约定.md](架构与工程约定.md) | 分层 / 构建 / 日志纪律（改桥代码前必读） |

**外部可借鉴的思想**（只借鉴，不整库移植 —— 本平台是 1bpp + 自绘，通用 GUI 库搬不进来）：
- **u8g2**（单色屏图形 / 点阵字体）：字体紧凑存储、图元裁剪思路
- **LVGL**（对象树 / 事件分发 / 重绘区域）：界面结构分层思想
- **LÖVE (love2d)**（Lua 2D 框架）：PC 上先跑通 `update/draw` 再搬成单文件，做原型很快

---

## 软键盘：一律走 kbd_lib 统一入口（2026-09-22）

### 为什么不再在脚本里自己画键盘
项目里"很多键盘 UI"的真相是：**内核只有 `kbd_lib` 一份**（`keyboard.c` = 布局表/档位状态/几何命中；
`kbd_ui.c` = 渲染器），7 个页面共用，但**外观有 4 种形态**。
历史上 `draw` 与 `hit` 是**两组独立函数**，调用方要自己配对 —— 一旦"用 `kbd_draw_edit` 画、
却调 `kbd_hit` 判"，几何就不一致，**命中全错且不报错**。

现已在 `kbd_lib` 加**统一样式入口**（`kbd_style_t` + `kbd_geom_t` + `kbd_draw_style`/`kbd_hit_style`），
draw 与 hit 共用同一个几何结构；Lua 侧同样只暴露这一条路。

### Lua API（`page_lua.c` 的 `app.kbd`）

| API | 说明 |
|---|---|
| `app.kbd.use("std"[, y])` | 5 排全高 145px，布局 `kbd_std`。y 默认贴屏幕底 |
| `app.kbd.use("edit"[, y])` | 排高 19px（压低 1/3），布局 `kbd_edit` |
| `app.kbd.use("rows", n[, y])` | 只画/只判前 n 排，布局 `kbd_std` |
| `app.kbd.use("dialog", x, y, w, row_h)` | 把满宽布局等比缩放平移到 `[x, x+w)`，布局 `kbd_edit` |
| `app.kbd.geom()` | → `{style=,x=,y=,w=,row_h=,rows=}` |
| `app.kbd.height()` | 当前样式占用像素高（用于给它腾地方） |
| `app.kbd.draw([press])` | 用当前几何绘制；`press` = 按下键索引(-1 无) |
| `app.kbd.hit(tx, ty)` | 用**同一几何**命中 → 键索引 / -1。**先 x 后 y** |
| `app.kbd.press(i)` | 动作码，**含档位副作用**（CAPS/SHIFT/FN 在内部翻转） |
| `app.kbd.char(i)` | 当前档位下该键的字符；无则 `nil` |
| `app.kbd.label(i)` / `layer_on(i)` | 当前显示标签 / 是否处于激活档层 |
| `app.kbd.get("caps"/"shift"/"fn")` / `set(k,v)` / `reset()` / `count()` | 档位读写与复位 |

动作码常量与 `KBD_ACT_*` 同值：`app.kbd.CHAR/ENTER/BKSP/SPACE/ESC/TAB/UP/DOWN/LEFT/RIGHT/CAPS/SHIFT/FN/CTRL/ALT`
样式常量：`app.kbd.STYLE_STD/STYLE_EDIT/STYLE_ROWS/STYLE_DIALOG`；几何：`app.kbd.H/ROW/ROW_EDIT/ROWS`

### 推荐写法（一次 `use` 定样式，之后 draw/hit 同源）
```lua
function on_enter()
  app.kbd.use("std")          -- 或 "edit" / "rows",3 / "dialog",x,y,w,16
  app.kbd.reset()
end

function render()
  app.kbd.draw(press_index)   -- 画
end

function poll()
  local tx, ty = app.touch()
  if tx and ty and not was then
    local i = app.kbd.hit(tx, ty)        -- 判命中：与 draw 同一几何
    if i >= 0 then
      local act = app.kbd.press(i)
      local c   = app.kbd.char(i)
      if act == app.kbd.ENTER then ... elseif act == app.kbd.CHAR and c then ... end
    end
  end
end
```

> ⚠ 模拟器（`tools/lua_sim.py` + `tools/fw_kbd.py`）**直接从 `keyboard.c` 解析布局**并实现同一套
> 内核语义，所以本机截图与设备一致，不会分叉。

---

## 应用一致性口径（2026-09-22 linit 定案，放宽视觉）

> **Lua 应用不必与 C 版像素级一模一样，"差不多"即可；但这三条必须完全一致：**
> 1. **菜单选项与规则分类一致** —— 条目、层级、分类归属、取值范围/步进照抄 C 版
> 2. **处理逻辑一致** —— 同样的输入得到同样的结果与提示（分发/分支/边界/超时）
> 3. **点击逻辑一致** —— 点哪里做什么、拖动/长按的判定与阈值、命中区域

理由：视觉细节（自绘小字、圆角、图标像素）属表现层，允许用固件字模近似；
而**菜单结构 + 逻辑 + 交互**是用户的操作契约，改了就是另一个软件。


## 弹窗一律用 `app.dlg.*`（禁止自绘）

规范（linit 2026-09-22 / 2026-10-03 补全）：**弹窗的框架与位置只有一处实现**（`os_dialog.c`），
调用方只给文字。Lua 侧对应：

| API | 说明 | 取结果 |
|---|---|---|
| `app.dlg.toast(msg[, ms])` | 提示，默认 0.5s 自动关 | 无需取 |
| `app.dlg.confirm(msg)` | 确认框（左"确定"右"取消"） | `app.dlg.result()` → `0`=确定 / `-1`=取消 |
| `app.dlg.list(title, {项…}[, sel])` | 列表选择，返回项数 | `app.dlg.result()` → 选中下标(1起) / `-1`=取消 |

**轮询式取值**（贴合 on_enter/render/poll/action 契约，不需要额外回调）：

```lua
local awaiting = false
-- 打开
app.dlg.confirm("删除这条记录?") ; awaiting = true
-- poll 里取
function poll()
  if awaiting then
    local r = app.dlg.result()
    if r ~= nil then
      awaiting = false
      if r == 0 then ...确定... end
    end
  end
end
```
