# Lua 应用开发

## Lua 桥 API 参考

# Lua 桥 API 参考（自动生成，勿手改）

> 由 `tools/lua_api_doc.py` 从 `components/apps/lua_bridge/luat_lib_*.c` **自动提取**。
> 「说明」一列取的是源码里函数上方的注释 —— **改代码就是改文档**，不需要手改本文件。
> 共 **138** 个 API / 13 个模块。加 API 见技能 `lintos-lua-app-pipeline` §18。「定义」列是源码位置。

## `fs` — 文件（TF 卡，`fs` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.fs.exists` | app.fs.* : TF 卡文件存取 | `luat_lib_fs.c:155` |
| `app.fs.read` | （源码未写注释） | `luat_lib_fs.c:156` |
| `app.fs.read_at` | （源码未写注释） | `luat_lib_fs.c:157` |
| `app.fs.write` | （源码未写注释） | `luat_lib_fs.c:158` |
| `app.fs.append` | （源码未写注释） | `luat_lib_fs.c:159` |
| `app.fs.remove` | （源码未写注释） | `luat_lib_fs.c:160` |
| `app.fs.size` | app.fs.size(path) -> 字节数 \| nil * 读之前先问大小: 分块地图的最小/最大块判据、下载落地自检都要用它 | `luat_lib_fs.c:161` |
| `app.fs.mkdir` | 已存在也返回 true (幂等), 免得调用方每次都要先 exists() 判一遍 | `luat_lib_fs.c:162` |
| `app.fs.listdir` | （源码未写注释） | `luat_lib_fs.c:163` |

## `gfx` — 绘制与字模（图元 / 8 套字模 / 位图 / 字体宽度）

| API | 说明 | 定义 |
|---|---|---|
| `app.clear` | （源码未写注释） | `luat_lib_gfx.c:421` |
| `app.px` | （源码未写注释） | `luat_lib_gfx.c:422` |
| `app.fill` | （源码未写注释） | `luat_lib_gfx.c:423` |
| `app.store_rect` | （源码未写注释） | `luat_lib_gfx.c:424` |
| `app.rect` | （源码未写注释） | `luat_lib_gfx.c:425` |
| `app.hline` | （源码未写注释） | `luat_lib_gfx.c:426` |
| `app.vline` | （源码未写注释） | `luat_lib_gfx.c:427` |
| `app.text` | （源码未写注释） | `luat_lib_gfx.c:428` |
| `app.textc` | （源码未写注释） | `luat_lib_gfx.c:429` |
| `app.textw` | （源码未写注释） | `luat_lib_gfx.c:430` |
| `app.big` | （源码未写注释） | `luat_lib_gfx.c:431` |
| `app.bigc` | （源码未写注释） | `luat_lib_gfx.c:432` |
| `app.zh` | （源码未写注释） | `luat_lib_gfx.c:433` |
| `app.zh24` | （源码未写注释） | `luat_lib_gfx.c:434` |
| `app.bmp` | （源码未写注释） | `luat_lib_gfx.c:435` |
| `app.bmpr` | 数据走 luaL_checklstring (长度感知, 二进制安全, 不做 UTF-8 转换) | `luat_lib_gfx.c:436` |
| `app.zhs` | app.zhs(x,y,str[,inv]): 12×12 中文点阵 (23×23 字库 2×2 降采样, 照 page_diag dzh) | `luat_lib_gfx.c:437` |
| `app.texts` | 下面这些 API 一一对应 ui_common.c 里的固件字体函数 (各 C 版应用页面用的就是它们), * 于是 Lua 版与 C 版 UI 同源同形: * app.texts 8×12 ASCII (draw_ascii_small)… | `luat_lib_gfx.c:438` |
| `app.textm` | （源码未写注释） | `luat_lib_gfx.c:439` |
| `app.textsb` | （源码未写注释） | `luat_lib_gfx.c:440` |
| `app.zhsm` | （源码未写注释） | `luat_lib_gfx.c:441` |
| `app.zhsb` | （源码未写注释） | `luat_lib_gfx.c:442` |
| `app.mix` | （源码未写注释） | `luat_lib_gfx.c:443` |
| `app.mixc` | （源码未写注释） | `luat_lib_gfx.c:444` |
| `app.mixw` | （源码未写注释） | `luat_lib_gfx.c:445` |
| `app.poly` | （源码未写注释） | `luat_lib_gfx.c:446` |

## `gps` — 

| API | 说明 | 定义 |
|---|---|---|
| `app.gps.start` | 启动顺序必须是 唤醒 → 再起服务 | `luat_lib_gps.c:102` |
| `app.gps.stop` | app.gps.stop() → bool: 停后台解析任务 + **把模块送回待机** (2026-10-09) * 应用退出时务必调用: 否则解析任务常驻, 且 GPS 会保持 ~10mA 连续跟踪 * (待机只需 46µA) —— 直… | `luat_lib_gps.c:103` |
| `app.gps.fix` | app.gps.fix() → table \| nil * 全是整数定标值 (见文件头), 括号里是还原公式 | `luat_lib_gps.c:104` |
| `app.gps.demo` | app.gps.demo([on]) → bool: 开/关演示数据 (不读 UART) | `luat_lib_gps.c:105` |
| `app.gps.reset` | app.gps.reset(): 清定位状态与统计 (不关演示开关) | `luat_lib_gps.c:106` |

## `hid` — USB 键鼠（`hid` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.hid.start` | （源码未写注释） | `luat_lib_hid.c:101` |
| `app.hid.stop` | （源码未写注释） | `luat_lib_hid.c:102` |
| `app.hid.connected` | （源码未写注释） | `luat_lib_hid.c:103` |
| `app.hid.running` | app.hid.running(): HID 栈是否已启动 (区别于 connected: 对端是否枚举上来) | `luat_lib_hid.c:104` |
| `app.hid.key` | （源码未写注释） | `luat_lib_hid.c:105` |
| `app.hid.key_str` | （源码未写注释） | `luat_lib_hid.c:106` |
| `app.hid.mouse_move` | （源码未写注释） | `luat_lib_hid.c:107` |
| `app.hid.mouse_click` | （源码未写注释） | `luat_lib_hid.c:108` |
| `app.hid.shortcut` | （源码未写注释） | `luat_lib_hid.c:109` |
| `app.hid.cdc_read` | } [modsplit] 共享 | `luat_lib_hid.c:110` |
| `app.hid.cdc_connected` | app.hid.cdc_connected(): 目标电脑的 CDC 串口是否已连接 (日志诊断页显示 USB 状态用) | `luat_lib_hid.c:111` |

## `img` — 大图与精灵（`img` 子表：1bpp BMP 载入 PSRAM + 窗口位块传输，地图/角色用）

| API | 说明 | 定义 |
|---|---|---|
| `app.img.load` | app.img.load(path[, opaque]) -> id \| nil | `luat_lib_img.c:248` |
| `app.img.blit` | #define IMG_ROW_MAX 64 上限 512 像素宽 (我们的屏 400) | `luat_lib_img.c:249` |
| `app.img.size` | app.img.size(id) -> w, h | `luat_lib_img.c:250` |
| `app.img.free` | app.img.free(id) / app.img.freeall() | `luat_lib_img.c:251` |
| `app.img.freeall` | （源码未写注释） | `luat_lib_img.c:252` |

## `input` — 时间与输入（按键 / 按住状态 / 自动重复 / 触摸 / 重绘）

| API | 说明 | 定义 |
|---|---|---|
| `app.millis` | Lua: 时间/输入/声音/震动/退出 | `luat_lib_input.c:162` |
| `app.key` | （源码未写注释） | `luat_lib_input.c:163` |
| `app.delay` | （源码未写注释） | `luat_lib_input.c:164` |
| `app.key_down` | 做"按住持续移动/连发"全靠它 | `luat_lib_input.c:165` |
| `app.keys_mask` | 支持多键同时: `if m & 1 ~= 0 and m & 4 ~= 0` = 左上一起按(斜向) | `luat_lib_input.c:166` |
| `app.key_repeat` | （源码未写注释） | `luat_lib_input.c:167` |
| `app.touch` | app.touch(): 返回 x,y 或 nil | `luat_lib_input.c:168` |
| `app.touch_start` | 无手势返回 nil * 与 app.touch() 同帧使用: 起点固定、当前点变化, 两者相减即位移 | `luat_lib_input.c:169` |
| `app.touch_hold` | 脚本据此实现"长按" | `luat_lib_input.c:170` |
| `app.redraw` | app.redraw(): 标记本帧需重绘刷屏. Lua 应用 poll() 里逐帧刷新用(雷达/触屏绘制). | `luat_lib_input.c:171` |

## `json` — 

| API | 说明 | 定义 |
|---|---|---|
| `app.json.pick` | app.json.pick(json, array_key, fields[, max]) -> 表数组 | `luat_lib_json.c:325` |
| `app.json.num` | app.json.num / str / bool | `luat_lib_json.c:326` |
| `app.json.str` | （源码未写注释） | `luat_lib_json.c:327` |
| `app.json.bool` | （源码未写注释） | `luat_lib_json.c:328` |

## `kbd` — 软键盘（`kbd` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.kbd.use` | app.kbd.use(style[, ...]) — 一次选定 样式 + 几何 + 布局表 | `luat_lib_kbd.c:131` |
| `app.kbd.reset` | （源码未写注释） | `luat_lib_kbd.c:132` |
| `app.kbd.draw` | （源码未写注释） | `luat_lib_kbd.c:133` |
| `app.kbd.hit` | （源码未写注释） | `luat_lib_kbd.c:134` |
| `app.kbd.geom` | （源码未写注释） | `luat_lib_kbd.c:135` |
| `app.kbd.height` | （源码未写注释） | `luat_lib_kbd.c:136` |
| `app.kbd.count` | （源码未写注释） | `luat_lib_kbd.c:137` |
| `app.kbd.press` | （源码未写注释） | `luat_lib_kbd.c:138` |
| `app.kbd.char` | （源码未写注释） | `luat_lib_kbd.c:139` |
| `app.kbd.label` | （源码未写注释） | `luat_lib_kbd.c:140` |
| `app.kbd.layer_on` | （源码未写注释） | `luat_lib_kbd.c:141` |
| `app.kbd.get` | （源码未写注释） | `luat_lib_kbd.c:142` |
| `app.kbd.set` | （源码未写注释） | `luat_lib_kbd.c:143` |

## `net` — 网络（`net` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.net.is_connected` | （源码未写注释） | `luat_lib_net.c:378` |
| `app.net.mirrors` | app.net.* : 网络状态 / HTTP / ping app.net.mirrors() → { "https: bbk.linit.cn/bbk", ... } * * 分发包的**镜像源清单**, 取自固件里的 os_sourc… | `luat_lib_net.c:379` |
| `app.net.get_ip` | （源码未写注释） | `luat_lib_net.c:380` |
| `app.net.http_get` | （源码未写注释） | `luat_lib_net.c:381` |
| `app.net.fetch_start` | （源码未写注释） | `luat_lib_net.c:382` |
| `app.net.fetch_poll` | （源码未写注释） | `luat_lib_net.c:383` |
| `app.net.fetch_got` | 非流式也能看"收了多少") | `luat_lib_net.c:384` |
| `app.net.ping` | （源码未写注释） | `luat_lib_net.c:385` |
| `app.net.port` | V1.2 新增: 局域网扫描 (netscan 服务) —— 解锁 netdect 类应用 | `luat_lib_net.c:386` |
| `app.net.arp` | （源码未写注释） | `luat_lib_net.c:387` |
| `app.net.arp_list` | （源码未写注释） | `luat_lib_net.c:388` |
| `app.net.ipinfo` | 返回 ip, 掩码, 网关, DNS, 接口名 (未联网 / 无 netif 时返回 0 个值) | `luat_lib_net.c:389` |
| `app.net.vendor` | 未命中返回 nil (OUI 字典常驻固件, 与 netdect 的厂商显示同源) | `luat_lib_net.c:390` |

## `store` — 持久化（`nvs` 掉电保留、`db` 核心互通库）

| API | 说明 | 定义 |
|---|---|---|
| `app.nvs.get_int` | （源码未写注释） | `luat_lib_store.c:90` |
| `app.nvs.set_int` | （源码未写注释） | `luat_lib_store.c:91` |
| `app.nvs.get_str` | （源码未写注释） | `luat_lib_store.c:92` |
| `app.nvs.set_str` | （源码未写注释） | `luat_lib_store.c:93` |
| `app.db.set` | （源码未写注释） | `luat_lib_store.c:96` |
| `app.db.get` | （源码未写注释） | `luat_lib_store.c:97` |
| `app.db.del` | （源码未写注释） | `luat_lib_store.c:98` |
| `app.db.seti` | （源码未写注释） | `luat_lib_store.c:99` |
| `app.db.geti` | （源码未写注释） | `luat_lib_store.c:100` |

## `sys` — 系统与提示（日志 / 音效 / 震动 / 退出 / 状态栏，以及 `sys`、`dlg` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.sys.log` | 用定长 C 缓冲更稳 | `luat_lib_sys.c:225` |
| `app.sys.tone` | （源码未写注释） | `luat_lib_sys.c:226` |
| `app.sys.tone_async` | app.tone_async(freq, ms[, amp]) → 非阻塞单音 (不卡帧). 连打 = 天然排队成小旋律 | `luat_lib_sys.c:227` |
| `app.sys.melody` | （源码未写注释） | `luat_lib_sys.c:228` |
| `app.sys.vibrate` | （源码未写注释） | `luat_lib_sys.c:229` |
| `app.sys.exit` | （源码未写注释） | `luat_lib_sys.c:230` |
| `app.sys.exit_confirm` | 只在脚本没消费 BACK 时生效: 脚本把 BACK 当返回键用 (关子窗/收菜单) 依旧优先 | `luat_lib_sys.c:231` |
| `app.sys.sbar` | 注意: 脚本自己负责在 on_exit 里还原成 false, 否则会带到下一页 | `luat_lib_sys.c:232` |
| `app.sys.battery` | app.sys.battery(): 返回电压 mV 或 nil | `luat_lib_sys.c:234` |
| `app.sys.heap_free` | （源码未写注释） | `luat_lib_sys.c:235` |
| `app.sys.time_ms` | （源码未写注释） | `luat_lib_sys.c:236` |
| `app.sys.clock` | 系统时间未同步 (没联网/没设时间) 时返回的就是开机以来的错值, 脚本自行容忍 | `luat_lib_sys.c:237` |
| `app.dlg.toast` | 这样完全贴合已有契约 (on_enter/render/poll/action), 不需要动桥的调用约定 | `luat_lib_sys.c:240` |
| `app.dlg.confirm` | app.dlg.confirm(msg): 通用确认框 (确定=0 / 取消=-1) | `luat_lib_sys.c:241` |
| `app.dlg.list` | app.dlg.list(title, {项...}[, 初始选中]): 列表选择框 (返回选中下标 1-based) | `luat_lib_sys.c:242` |
| `app.dlg.result` | >=0 确定(0/1)/列表下标(1..n) | `luat_lib_sys.c:243` |

## `wifi` — 无线（`wifi` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.wifi.init` | app.wifi.* : 无线探测 | `luat_lib_wifi.c:130` |
| `app.wifi.scan` | （源码未写注释） | `luat_lib_wifi.c:131` |
| `app.wifi.scan_done` | （源码未写注释） | `luat_lib_wifi.c:132` |
| `app.wifi.ap_count` | （源码未写注释） | `luat_lib_wifi.c:133` |
| `app.wifi.ap_info` | （源码未写注释） | `luat_lib_wifi.c:134` |
| `app.wifi.probe_init` | （源码未写注释） | `luat_lib_wifi.c:135` |
| `app.wifi.probe_stop` | （源码未写注释） | `luat_lib_wifi.c:136` |
| `app.wifi.probe_count` | （源码未写注释） | `luat_lib_wifi.c:137` |
| `app.wifi.probe_info` | （源码未写注释） | `luat_lib_wifi.c:138` |
| `app.wifi.release` | （源码未写注释） | `luat_lib_wifi.c:139` |
| `app.wifi.ch_stat` | （源码未写注释） | `luat_lib_wifi.c:140` |
| `app.wifi.cur_ssid` | 联网链 (解锁 netdect 的「选网 → 输密码 → 连接」) | `luat_lib_wifi.c:141` |
| `app.wifi.connecting` | （源码未写注释） | `luat_lib_wifi.c:142` |
| `app.wifi.connect` | （源码未写注释） | `luat_lib_wifi.c:143` |
| `app.wifi.setup` | 另有 app.wifi.connected(): 是否已联网 (只读查询, 门禁/离线提示用) | `luat_lib_wifi.c:144` |
| `app.wifi.connected` | （源码未写注释） | `luat_lib_wifi.c:145` |
| `app.wifi.clear_saved` | （源码未写注释） | `luat_lib_wifi.c:146` |

---

## 约定（所有 API 通用）

- 动作码：`1=UP 2=DOWN 3=LEFT 4=RIGHT 5=CONFIRM(OK) 6=BACK 7=HOME`；
  `app.UP..app.HOME` 与此一一对应，`consts` 模块用 `_Static_assert` 与 `os_action_t` 钉死。
- 颜色：仅黑白两色，`app.BLACK=0` / `app.WHITE=1`（反射式 1bpp 屏，白纸黑字）。
- 屏幕：`400×300`；16px 中文请用 **`app.zh`**（`font_zh16` 真 16×16，与系统 UI 同款），
  ⛔ 不要用 `app.zhsb`（那是 24×24 降采样，会糊）。ASCII 用 `app.textsb`（8×16）。
- 回调契约：`on_enter / render / poll / action(a) / on_exit`；按键**只走 `action(a)`** 一条路。

---

## Lua 应用 UI 规范

# LinTOS Lua 应用 UI 规范（LVGL 风格 · 1bpp 版 v1.0）

> **一句话**：**借 LVGL 的设计语言，不用 LVGL 的代码。**
> 目标：让 Lua 应用**写起来像 LVGL**（组件化、声明式布局、状态样式），**跑起来像原生 C 页面**（同字体、同栅格、同性能）。
> 适用：`/sdcard/apps/{app,ops}/*.lua` 单文件应用 + `/sdcard/apps/_ui.lua` 公共组件库。

---

## 一、⛔ 为什么不直接上 LVGL（三条硬理由，都有依据）

| # | 理由 | 依据 |
|---|---|---|
| **1** | **放卡即玩会断** | 你的 Lua 应用是"**纯脚本 + 现成 `app.*` 桥**"，放卡就能跑。引入 LVGL 需在**固件里塞 C 库 + 定时器 + flush + 输入设备 + 一整套 Lua 绑定** ⇒ 以后改 UI 都得刷固件 |
| **2** | **会更慢** | 这块屏的帧缓冲在 **PSRAM**，`st7305_draw_pixel` 每次落笔都是**随机读改写**（缓存未命中约 100 周期）；项目实测结论是「**唯一的杠杆是少画**」。LVGL 每次 flush 都要把 I1 缓冲**转成 ST7305 的竖向分块布局**再写进 PSRAM ⇒ 比现在的 `app.hline/vline/poly` 直接落笔**更碎更慢** |
| **3** | **1bpp 格式对不上（但能救）** | LVGL 的 `LV_COLOR_FORMAT_I1` 是**横向 MSB-first**（1 字节 = 相邻 8 像素）；ST7305 是 **2 列 × 4 行 / 字节**。LVGL 有 `lv_draw_sw_i1_convert_to_vtiled()` 可转，但**是否与 ST7305 排列完全一致还需实测** ⇒ 不值得为它押注 |

✅ **所以：LVGL 当"规范来源"，`_ui.lua` 当"实现"。** 组件名、布局语义、状态命名**照 LVGL 抄**，实现全部走你现有的桥。

## 二、四、LVGL 概念 → 本规范 对照表

| LVGL | 本规范 | 说明 |
|---|---|---|
| `lv_obj` 对象树 / parent | **函数式绘制 + 显式坐标** | Lua 里建对象树反而慢；用 `UI.xxx(...)` 直接画 |
| `lv_obj_set_size(50%)` | **百分比参数**（`0.5` 或 `UI.PCT(50)`） | 支持；`UI.SIZE_CONTENT` = 自动按内容 |
| flex / grid 布局 | **`UI.row/UI.col/UI.grid`** 三个简化版 | 覆盖 95% 场景，不引入完整 flex 引擎 |
| style / state（default/pressed/focused/disabled） | **`ST` 四态常量 + `UI.button(...,state)`** | 1bpp 表现见第六节 |
| 事件回调 `LV_EVENT_CLICKED` | **`action(a)` + `UI.hit()` 命中测试** | 沿用你现有的按键/触摸单通道模型 |
| `lv_label` | **`UI.label / UI.labels` / `UI.labelc`** | 三档字体，中英**必须分开调用** |
| `lv_btn` | **`UI.button`** | 含圆角框 + 反相/实心两态 |
| `lv_list` / `lv_roller` | **`UI.list`** | 带滚动 + 焦点选中 |
| `lv_bar` | **`UI.bar`** | 进度/电量/强度 |
| `lv_switch` / `lv_checkbox` | **`UI.switch` / `UI.check`** | 反相圆角描边，不用颜色区分 |
| `lv_msgbox` | **`app.dlg.confirm/list/toast`** | ⛔ 一律走系统弹窗，**不要自绘弹窗** |
| `lv_table` / `lv_chart` | **`UI.table` / `UI.chart`**（简易版） | 只做 1bpp 可读性优先的极简版 |
| `LV_IMG` | **`app.img.load/blit`** | ⚠ 位图资源走图集；`load` 有 **2048px 宽硬上限** ⇒ 排**网格**不排长带 |

---

## 三、硬约束（这块屏的物理事实，规范全部据此推导）

| 项 | 值 | 规范含义 |
|---|---|---|
| 屏幕 | **400×300，1bpp（黑/白）** | 只有两色；"层级"靠**线宽/线密/反相**，不靠明度 |
| 帧缓冲 | **PSRAM**，每次落笔随机读改写 | **少画**是唯一性能杠杆 |
| 字体（唯一真相源 = `ui_common.c`） | 汉字 **16px `app.mix`** / 12px `app.zhsm`（粗 `zhsb`）；ASCII `app.texts`(8×12) / `textsb` / `textm`；大字 `app.big/bigc` | ⛔ **中英混排必须分两次调用**（`zhs/zhsm/zhsb` 遇 ASCII 是**空白格但照样占位**） |
| 折线 | `app.poly(点串,w,ct,cb)` **一次画整条** | 地图/图表/波形**必须用它**（实测跨界调用降 **17.5×**） |
| 图片 | `app.img.load(path[,opaque])`；`IMG_MAX=10`；宽 ≤2048 | 不透明图集可**完全去掉 clear** |
| 布局 | 状态栏 `y=0..23`，内容 `y=24..`，底栏按键区默认 `y=272..299` | 见第五节 |
| 弹窗 | `app.dlg.confirm/list/toast` + `app.dlg.result()` | 0.5s 自动关；**BACK 一律先关弹窗** |

---

## 四、栅格、间距、字体分级

```
400×300 布局骨架
┌──────────────────────────────────────────┐ 0
│ 状态栏 (24px)     ← app.sys.sbar 提供    │
├──────────────────────────────────────────┤ 24
│                                          │
│          内容区 (24 .. 271)              │
│                                          │
├──────────────────────────────────────────┤ 272
│ 底栏 / 按键区 (272 .. 299)               │
└──────────────────────────────────────────┘ 300
```

- **栅格**：一切按 **8px** 对齐；内边距 `4/8/12`，块间距 `8`，行高 `20`（16px 汉字）或 `16`（12px 汉字）
- **左右对称**：主内容左右留白相等；按键**贴左右两侧**（不要再居中竖排）
- **字体分级**（只用这 6 档，禁止另造）

| 用途 | API | 说明 |
|---|---|---|
| 正文中文 | `app.mix(x,y,s,inv)` | 16px，**主力** |
| 次要中文 | `app.zhsm(x,y,s,inv)` | 12px |
| 强调中文 | `app.zhsb(x,y,s,inv)` | 12px 粗 |
| 正文 ASCII | `app.texts(x,y,s,inv)` | 8×12 |
| 次要/数值 ASCII | `app.textsb(x,y,s,inv)` | 8×16 数字更清晰 |
| 大数字（时/速/分） | `app.big(x,y,s,scale,inv)` / `bigc` 居中 | `scale` 1~4 |
| 中英混排 | **分两次调用**：中文用 `mix/zhsm`，ASCII 用 `texts/textsb`，用 `app.mixw(s)` 量中文宽 | ⛔ 绝不把 ASCII 塞进 `zhs*` |

---

## 五、组件 API 规范（`_ui.lua`）

> 命名一律 `UI.<组件>`；所有函数**只画不刷屏**，调用方在 `render()` 末尾统一 `app.redraw()`。

```lua
local UI = dofile("/sdcard/apps/_ui.lua")

-- 常量
UI.W, UI.H            -- 400, 300
UI.ST = { NORMAL=0, PRESSED=1, FOCUS=2, DISABLED=3 }   -- 四态（对标 LVGL state）
UI.SIZE_CONTENT = -1

-- 基础图元（1bpp 优化：圆角逐行裁角，不用 fill_rect 冒充）
UI.frame(x,y,w,h[,r])              -- 描边框（r>0 圆角；R≥6 才看得出弧度）
UI.fill(x,y,w,h)                   -- 实心块
UI.fill_round(x,y,w,h,r)           -- ★ 圆角填充（尊重圆角，不盖平方角）
UI.hline/vline/line/poly           -- 直连 app.*，poly 用于折线

-- 文本
UI.label(x,y,s[,opt])              -- opt={font="mix|zhsm|texts|textsb", inv=bool}
UI.labelc(cx,y,s[,opt])            -- 水平居中
UI.labelw(s[,font])                -- 量宽（混排时分两次量再加）

-- 组件（全部返回命中矩形，供 action/touch 复用）
UI.button(x,y,w,h,text,state)      -- 状态: NORMAL=描边 / PRESSED=反相 / DISABLED=虚线
UI.list(x,y,w,h,items,sel,top)     -- 滚动列表；返回可见区间
UI.bar(x,y,w,h,pct[,inv])          -- 进度/电量
UI.switch(x,y,on)                  -- 开关（反相=ON）
UI.check(x,y,on)                   -- 勾选
UI.table(x,y,colw,rows,sel)        -- 极简表格

-- 布局
UI.row(x,y,h,{w1,w2,...})          -- 横向分栏（返回各列 x）
UI.col(x,y,w,{h1,h2,...})          -- 纵向分行
UI.center(w,h)                     -- 居中矩形

-- 命中
UI.hit(px,py,x,y,w,h)              -- 触摸命中测试（返回 bool）
```

**状态在 1bpp 下的表现**（禁止用颜色区分）：

| 状态 | 表现 |
|---|---|
| `NORMAL` | 1px 描边 + 正常字 |
| `FOCUS` | 1px 描边 + **左/上下加粗 2px 或加角标** |
| `PRESSED` | **整块反相**（黑底白字） |
| `DISABLED` | **虚线描边** + 字用 `inv=false` 的正常色但跳点 |

---

## 六、性能规范（照抄项目 1bpp 技能，违反必掉帧）

1. **render() 里不刷白底** —— 一帧只在开头 `app.clear()` 一次；浮层面板才需要局部刷白
2. **静态/动态分层** —— 静态层只在"换页/换关"重建；动态层每帧只**擦上一帧占用格 + 重画新格**（用 `y*100+x` 压整数存表，前后帧换表复用）
3. **脏标记两道独立闸门** —— `if not dirty then return end` **且** `if now-last < GAP then return end`
   ⛔ 旧写法 `if not dirty and now-last<GAP then return end` 是错的（过了 GAP 就无条件重绘）
4. **静态页不周期置 dirty** —— 只在 `action()` 里置；只有场上有东西动才逐帧置
5. **小件用 key 缓存** —— 状态条只在数值变化时重画（`if hud_key == key then return end`）
6. **折线一律 `app.poly`**；**指标/列表整屏重绘前先量**
7. ⛔ **不要逐行 `fseek` 读文件**（SPIFFS 每次 seek 扫索引 ⇒ task_wdt）；大文件用 `app.fs.read_at`
8. ⛔ **`app.textsb/texts/textm` 是纯 ASCII**；值里带汉字必须换 `app.mix`
9. ⛔ **弹窗一律 `app.dlg.*`**；全屏/独立程序**必须挂退出确认**

---

## 七、应用骨架（照这个写，四件套不能少）

```lua
--@app name=我的应用
--@app cat=程序
local UI = dofile("/sdcard/apps/_ui.lua")
local M, T = {}, {}                    -- 状态表 / 临时量
local A_OK, A_BACK, A_UP, A_DOWN = 0, 1, 2, 3   -- 动作常量用数值字面量

function on_enter()  ... end           -- 初始化（加载数据/启动服务）
function render()                      -- 只画，不刷屏
  if not M.dirty then return end
  if app.millis() - (M.last or 0) < 33 then return end
  M.dirty, M.last = false, app.millis()
  app.clear()
  UI.label(8, 4, "标题", {font="mix"})
  ...
end
function poll()  ... end               -- 每帧：把 needs_redraw 置 M.dirty
function action(a)
  if a == A_BACK then return false end -- ★ 必须 return false，否则吞掉"退出程序?"确认框
  ...
end
function on_exit() ... end             -- 释放资源（如 app.gps.stop()）
```

---

## 八、发布前自检清单

- [ ] `tools/lua_check.py` 通过（字库里没有的字符会被抓出来，如 `·` U+00B7）
- [ ] 中英混排分两次调用；没有把 ASCII 塞进 `zhs*`
- [ ] 顶层 `action` 的 BACK 返回 `false`
- [ ] 有退出确认；弹窗全走 `app.dlg.*`
- [ ] render 里只 `app.clear()` 一次、无多余刷白
- [ ] 脏标记是两道闸门；静态页不逐帧置 dirty
- [ ] 折线用了 `app.poly`；大文件用 `read_at`
- [ ] 真机连抓两次 `fbdump` 比对墨点数（差得多 = 抓在渲染中途）
- [ ] 帧耗时看串口 `OS: render 慢: 总 N us`（阈值 50ms），**绘制部分**要压到 30ms 以内
