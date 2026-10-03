# Lua 桥 API 参考（自动生成，勿手改）

> 由 `tools/lua_api_doc.py` 从 `components/apps/lua_bridge/luat_lib_*.c` **自动提取**。
> 「说明」一列取的是源码里函数上方的注释 —— **改代码就是改文档**，不需要手改本文件。
> 共 **129** 个 API / 13 个模块。加 API 见技能 `lintos-lua-app-pipeline` §18。「定义」列是源码位置。

## `fs` — 文件（TF 卡，`fs` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.fs.exists` | app.fs.* : TF 卡文件存取 | `luat_lib_fs.c:84` |
| `app.fs.read` | （源码未写注释） | `luat_lib_fs.c:85` |
| `app.fs.write` | （源码未写注释） | `luat_lib_fs.c:86` |
| `app.fs.append` | （源码未写注释） | `luat_lib_fs.c:87` |
| `app.fs.remove` | （源码未写注释） | `luat_lib_fs.c:88` |
| `app.fs.listdir` | （源码未写注释） | `luat_lib_fs.c:89` |

## `gfx` — 绘制与字模（图元 / 8 套字模 / 位图 / 字体宽度）

| API | 说明 | 定义 |
|---|---|---|
| `app.clear` | （源码未写注释） | `luat_lib_gfx.c:341` |
| `app.px` | （源码未写注释） | `luat_lib_gfx.c:342` |
| `app.fill` | （源码未写注释） | `luat_lib_gfx.c:343` |
| `app.store_rect` | （源码未写注释） | `luat_lib_gfx.c:344` |
| `app.rect` | （源码未写注释） | `luat_lib_gfx.c:345` |
| `app.hline` | （源码未写注释） | `luat_lib_gfx.c:346` |
| `app.vline` | （源码未写注释） | `luat_lib_gfx.c:347` |
| `app.text` | （源码未写注释） | `luat_lib_gfx.c:348` |
| `app.textc` | （源码未写注释） | `luat_lib_gfx.c:349` |
| `app.textw` | （源码未写注释） | `luat_lib_gfx.c:350` |
| `app.big` | （源码未写注释） | `luat_lib_gfx.c:351` |
| `app.bigc` | （源码未写注释） | `luat_lib_gfx.c:352` |
| `app.zh` | （源码未写注释） | `luat_lib_gfx.c:353` |
| `app.zh24` | （源码未写注释） | `luat_lib_gfx.c:354` |
| `app.bmp` | （源码未写注释） | `luat_lib_gfx.c:355` |
| `app.bmpr` | 数据走 luaL_checklstring (长度感知, 二进制安全, 不做 UTF-8 转换) | `luat_lib_gfx.c:356` |
| `app.zhs` | app.zhs(x,y,str[,inv]): 12×12 中文点阵 (23×23 字库 2×2 降采样, 照 page_diag dzh) | `luat_lib_gfx.c:357` |
| `app.texts` | 下面这些 API 一一对应 ui_common.c 里的固件字体函数 (各 C 版应用页面用的就是它们), * 于是 Lua 版与 C 版 UI 同源同形: * app.texts 8×12 ASCII (draw_ascii_small)… | `luat_lib_gfx.c:358` |
| `app.textm` | （源码未写注释） | `luat_lib_gfx.c:359` |
| `app.textsb` | （源码未写注释） | `luat_lib_gfx.c:360` |
| `app.zhsm` | （源码未写注释） | `luat_lib_gfx.c:361` |
| `app.zhsb` | （源码未写注释） | `luat_lib_gfx.c:362` |
| `app.mix` | （源码未写注释） | `luat_lib_gfx.c:363` |
| `app.mixc` | （源码未写注释） | `luat_lib_gfx.c:364` |
| `app.mixw` | （源码未写注释） | `luat_lib_gfx.c:365` |

## `gps` — 

| API | 说明 | 定义 |
|---|---|---|
| `app.gps.start` | app.gps.start() → bool: 初始化 UART 并起后台解析任务 (幂等) | `luat_lib_gps.c:88` |
| `app.gps.fix` | app.gps.fix() → table \| nil * 全是整数定标值 (见文件头), 括号里是还原公式 | `luat_lib_gps.c:89` |
| `app.gps.demo` | app.gps.demo([on]) → bool: 开/关演示数据 (不读 UART) | `luat_lib_gps.c:90` |
| `app.gps.reset` | app.gps.reset(): 清定位状态与统计 (不关演示开关) | `luat_lib_gps.c:91` |

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
| `app.json.pick` | app.json.pick(json, array_key, fields[, max]) -> 表数组 | `luat_lib_json.c:323` |
| `app.json.num` | app.json.num / str / bool | `luat_lib_json.c:324` |
| `app.json.str` | （源码未写注释） | `luat_lib_json.c:325` |
| `app.json.bool` | （源码未写注释） | `luat_lib_json.c:326` |

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
| `app.net.is_connected` | app.net.* : 网络状态 / HTTP / ping | `luat_lib_net.c:293` |
| `app.net.get_ip` | （源码未写注释） | `luat_lib_net.c:294` |
| `app.net.http_get` | （源码未写注释） | `luat_lib_net.c:295` |
| `app.net.fetch_start` | （源码未写注释） | `luat_lib_net.c:296` |
| `app.net.fetch_poll` | （源码未写注释） | `luat_lib_net.c:297` |
| `app.net.ping` | （源码未写注释） | `luat_lib_net.c:298` |
| `app.net.port` | V1.2 新增: 局域网扫描 (netscan 服务) —— 解锁 netdect 类应用 | `luat_lib_net.c:299` |
| `app.net.arp` | （源码未写注释） | `luat_lib_net.c:300` |
| `app.net.arp_list` | （源码未写注释） | `luat_lib_net.c:301` |
| `app.net.ipinfo` | 返回 ip, 掩码, 网关, DNS, 接口名 (未联网 / 无 netif 时返回 0 个值) | `luat_lib_net.c:302` |
| `app.net.vendor` | 未命中返回 nil (OUI 字典常驻固件, 与 netdect 的厂商显示同源) | `luat_lib_net.c:303` |

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
| `app.wifi.init` | app.wifi.* : 无线探测 | `luat_lib_wifi.c:117` |
| `app.wifi.scan` | （源码未写注释） | `luat_lib_wifi.c:118` |
| `app.wifi.scan_done` | （源码未写注释） | `luat_lib_wifi.c:119` |
| `app.wifi.ap_count` | （源码未写注释） | `luat_lib_wifi.c:120` |
| `app.wifi.ap_info` | （源码未写注释） | `luat_lib_wifi.c:121` |
| `app.wifi.probe_init` | （源码未写注释） | `luat_lib_wifi.c:122` |
| `app.wifi.probe_stop` | （源码未写注释） | `luat_lib_wifi.c:123` |
| `app.wifi.probe_count` | （源码未写注释） | `luat_lib_wifi.c:124` |
| `app.wifi.probe_info` | （源码未写注释） | `luat_lib_wifi.c:125` |
| `app.wifi.release` | （源码未写注释） | `luat_lib_wifi.c:126` |
| `app.wifi.ch_stat` | （源码未写注释） | `luat_lib_wifi.c:127` |
| `app.wifi.cur_ssid` | 联网链 (解锁 netdect 的「选网 → 输密码 → 连接」) | `luat_lib_wifi.c:128` |
| `app.wifi.connecting` | （源码未写注释） | `luat_lib_wifi.c:129` |
| `app.wifi.connect` | （源码未写注释） | `luat_lib_wifi.c:130` |
| `app.wifi.clear_saved` | （源码未写注释） | `luat_lib_wifi.c:131` |

---

## 约定（所有 API 通用）

- 动作码：`1=UP 2=DOWN 3=LEFT 4=RIGHT 5=CONFIRM(OK) 6=BACK 7=HOME`；
  `app.UP..app.HOME` 与此一一对应，`consts` 模块用 `_Static_assert` 与 `os_action_t` 钉死。
- 颜色：仅黑白两色，`app.BLACK=0` / `app.WHITE=1`（反射式 1bpp 屏，白纸黑字）。
- 屏幕：`400×300`；16px 中文请用 **`app.zh`**（`font_zh16` 真 16×16，与系统 UI 同款），
  ⛔ 不要用 `app.zhsb`（那是 24×24 降采样，会糊）。ASCII 用 `app.textsb`（8×16）。
- 回调契约：`on_enter / render / poll / action(a) / on_exit`；按键**只走 `action(a)`** 一条路。
