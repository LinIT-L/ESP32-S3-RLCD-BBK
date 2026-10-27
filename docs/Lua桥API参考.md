# Lua 桥 API 参考（自动生成，勿手改）

> 由 `tools/lua_api_doc.py` 从 `components/apps/lua_bridge/luat_lib_*.c` **自动提取**。
> 「说明」一列取的是源码里函数上方的注释 —— **改代码就是改文档**，不需要手改本文件。
> 共 **182** 个 API / 16 个模块。加 API 见技能 `lintos-lua-app-pipeline` §18。「定义」列是源码位置。

## `espnow` — ESP-NOW 点对点/广播（≤250 字节/包，不用路由器；双机联机、遥控、信标）

| API | 说明 | 定义 |
|---|---|---|
| `app.espnow.start` | app.espnow.start() → bool 幂等: 会顺带把 WiFi 射频拉起来 | `luat_lib_espnow.c:180` |
| `app.espnow.stop` | app.espnow.stop() → bool 只关 esp-now, 不动用户的 WiFi | `luat_lib_espnow.c:181` |
| `app.espnow.ready` | app.espnow.ready() → bool | `luat_lib_espnow.c:182` |
| `app.espnow.mac` | app.espnow.mac() → "aa:bb:..." 本机 MAC (= WiFi STA MAC, 就是别人要发给你的地址) | `luat_lib_espnow.c:183` |
| `app.espnow.channel` | app.espnow.channel() → int 查当前信道 * app.espnow.channel(n) → int 强制切到 n 信道 (1..13), 返回切完的信道 * ⚠ 连了路由器时信道由 AP 决定, 强制切会掉线 ——… | `luat_lib_espnow.c:184` |
| `app.espnow.add` | 发给某个固定对端前必须先 add (广播不用) | `luat_lib_espnow.c:185` |
| `app.espnow.del` | app.espnow.del(mac) → bool | `luat_lib_espnow.c:186` |
| `app.espnow.send` | app.espnow.send(mac, data) → bool 点对点 (≤250 字节) | `luat_lib_espnow.c:187` |
| `app.espnow.bcast` | app.espnow.bcast(data) → bool 广播 (同信道所有未加密设备都能收到) | `luat_lib_espnow.c:188` |
| `app.espnow.recv` | Lua 字符串是二进制安全的, 直接用 | `luat_lib_espnow.c:189` |
| `app.espnow.pending` | app.espnow.pending() → int 队列里还有几条没取 | `luat_lib_espnow.c:190` |
| `app.espnow.stats` | app.espnow.stats() → table {rx, drop, tx, fail, rssi} 排障用 | `luat_lib_espnow.c:191` |

## `fs` — 文件（TF 卡，`fs` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.fs.exists` | app.fs.* : TF 卡文件存取 | `luat_lib_fs.c:164` |
| `app.fs.read` | （源码未写注释） | `luat_lib_fs.c:165` |
| `app.fs.read_at` | （源码未写注释） | `luat_lib_fs.c:166` |
| `app.fs.write` | （源码未写注释） | `luat_lib_fs.c:167` |
| `app.fs.append` | （源码未写注释） | `luat_lib_fs.c:168` |
| `app.fs.remove` | （源码未写注释） | `luat_lib_fs.c:169` |
| `app.fs.rename` | 录音机改文件名、整理目录都用它 | `luat_lib_fs.c:170` |
| `app.fs.size` | app.fs.size(path) -> 字节数 \| nil * 读之前先问大小: 分块地图的最小/最大块判据、下载落地自检都要用它 | `luat_lib_fs.c:171` |
| `app.fs.mkdir` | 已存在也返回 true (幂等), 免得调用方每次都要先 exists() 判一遍 | `luat_lib_fs.c:172` |
| `app.fs.listdir` | （源码未写注释） | `luat_lib_fs.c:173` |

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

## `gps` — 定位（NMEA 解析结果，经纬度走整数微度）

| API | 说明 | 定义 |
|---|---|---|
| `app.gps.start` | 启动顺序必须是 唤醒 → 再起服务 | `luat_lib_gps.c:143` |
| `app.gps.stop` | app.gps.stop() → bool: 停后台解析任务 + **把模块送回待机** (2026-10-09) * 应用退出时务必调用: 否则解析任务常驻, 且 GPS 会保持 ~10mA 连续跟踪 * (待机只需 46µA) —— 直… | `luat_lib_gps.c:144` |
| `app.gps.fix` | app.gps.fix() → table \| nil * 全是整数定标值 (见文件头), 括号里是还原公式 | `luat_lib_gps.c:145` |
| `app.gps.sats` | sys: 0=GPS 1=GLONASS 2=Galileo 3=BeiDou 4=QZSS 5=NavIC 6=混合 * el=仰角 0..90, az=方位 0..359 (真北=0 顺时针), snr=-1 表示无, used=1 参… | `luat_lib_gps.c:146` |
| `app.gps.demo` | app.gps.demo([on]) → bool: 开/关演示数据 (不读 UART) | `luat_lib_gps.c:147` |
| `app.gps.reset` | app.gps.reset(): 清定位状态与统计 (不关演示开关) | `luat_lib_gps.c:148` |

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
| `app.net.is_connected` | （源码未写注释） | `luat_lib_net.c:279` |
| `app.net.mirrors` | app.net.* : 网络状态 / HTTP / ping app.net.mirrors() → { "https: bbk.linit.cn/bbk", ... } * * 分发包的**镜像源清单**, 取自固件里的 os_sourc… | `luat_lib_net.c:280` |
| `app.net.get_ip` | （源码未写注释） | `luat_lib_net.c:281` |
| `app.net.http_get` | 现在改成 net_client 的同步包装 —— 行为不变 (调用方仍阻塞等结果), 但底层只剩一个客户端 | `luat_lib_net.c:282` |
| `app.net.fetch_start` | 本次请求是否流式落盘 (决定 poll 返回几个值) | `luat_lib_net.c:283` |
| `app.net.fetch_poll` | （源码未写注释） | `luat_lib_net.c:284` |
| `app.net.fetch_got` | 非流式也能看"收了多少") | `luat_lib_net.c:285` |
| `app.net.ping` | （源码未写注释） | `luat_lib_net.c:286` |
| `app.net.port` | V1.2 新增: 局域网扫描 (netscan 服务) —— 解锁 netdect 类应用 | `luat_lib_net.c:287` |
| `app.net.arp` | （源码未写注释） | `luat_lib_net.c:288` |
| `app.net.arp_list` | （源码未写注释） | `luat_lib_net.c:289` |
| `app.net.ipinfo` | 返回 ip, 掩码, 网关, DNS, 接口名 (未联网 / 无 netif 时返回 0 个值) | `luat_lib_net.c:290` |
| `app.net.vendor` | 未命中返回 nil (OUI 字典常驻固件, 与 netdect 的厂商显示同源) | `luat_lib_net.c:291` |

## `store` — 持久化（`nvs` 掉电保留、`db` 核心互通库）

| API | 说明 | 定义 |
|---|---|---|
| `app.nvs.get_int` | （源码未写注释） | `luat_lib_store.c:219` |
| `app.nvs.set_int` | （源码未写注释） | `luat_lib_store.c:220` |
| `app.nvs.get_str` | （源码未写注释） | `luat_lib_store.c:221` |
| `app.nvs.set_str` | （源码未写注释） | `luat_lib_store.c:222` |
| `app.db.set` | （源码未写注释） | `luat_lib_store.c:225` |
| `app.db.get` | （源码未写注释） | `luat_lib_store.c:226` |
| `app.db.del` | （源码未写注释） | `luat_lib_store.c:227` |
| `app.db.seti` | （源码未写注释） | `luat_lib_store.c:228` |
| `app.db.geti` | （源码未写注释） | `luat_lib_store.c:229` |

## `sys` — 系统与提示（日志 / 音效 / 震动 / 退出 / 状态栏，以及 `sys`、`dlg` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.sys.log` | 用定长 C 缓冲更稳 | `luat_lib_sys.c:264` |
| `app.sys.tone` | （源码未写注释） | `luat_lib_sys.c:265` |
| `app.sys.tone_async` | app.tone_async(freq, ms[, amp]) → 非阻塞单音 (不卡帧). 连打 = 天然排队成小旋律 | `luat_lib_sys.c:266` |
| `app.sys.melody` | （源码未写注释） | `luat_lib_sys.c:267` |
| `app.sys.vibrate` | （源码未写注释） | `luat_lib_sys.c:268` |
| `app.sys.volume` | ⚠ 与系统音量是**同一个值**: 应用改完退出, 系统里看到的也是新值(符合直觉) | `luat_lib_sys.c:270` |
| `app.sys.mute` | 与应用内的"暂停"是两件事 | `luat_lib_sys.c:271` |
| `app.sys.bt` | app.bt() → 0=未启用 1=射频就绪(未连) 2=已连接(手柄/耳机) * 应用不需要自己开蓝牙: 开机不拉射频, 进设置页/手柄页才由 os_hw 拉起 | `luat_lib_sys.c:272` |
| `app.sys.exit` | （源码未写注释） | `luat_lib_sys.c:273` |
| `app.sys.exit_confirm` | 只在脚本没消费 BACK 时生效: 脚本把 BACK 当返回键用 (关子窗/收菜单) 依旧优先 | `luat_lib_sys.c:274` |
| `app.sys.sbar` | 注意: 脚本自己负责在 on_exit 里还原成 false, 否则会带到下一页 | `luat_lib_sys.c:275` |
| `app.sys.battery` | app.sys.battery(): 返回电压 mV 或 nil | `luat_lib_sys.c:277` |
| `app.sys.heap_free` | （源码未写注释） | `luat_lib_sys.c:278` |
| `app.sys.time_ms` | （源码未写注释） | `luat_lib_sys.c:279` |
| `app.sys.clock` | 系统时间未同步 (没联网/没设时间) 时返回的就是开机以来的错值, 脚本自行容忍 | `luat_lib_sys.c:280` |
| `app.dlg.toast` | 这样完全贴合已有契约 (on_enter/render/poll/action), 不需要动桥的调用约定 | `luat_lib_sys.c:283` |
| `app.dlg.confirm` | app.dlg.confirm(msg): 通用确认框 (确定=0 / 取消=-1) | `luat_lib_sys.c:284` |
| `app.dlg.list` | app.dlg.list(title, {项...}[, 初始选中]): 列表选择框 (返回选中下标 1-based) | `luat_lib_sys.c:285` |
| `app.dlg.result` | >=0 确定(0/1)/列表下标(1..n) | `luat_lib_sys.c:286` |

## `util` — 

| API | 说明 | 定义 |
|---|---|---|
| `app.util.after` | （源码未写注释） | `luat_lib_util.c:446` |
| `app.util.every` | （源码未写注释） | `luat_lib_util.c:447` |
| `app.util.cancel` | （源码未写注释） | `luat_lib_util.c:448` |
| `app.util.rand` | rand(n) = 1..n * app.util.randf() → [0,1) 浮点 * 硬件 TRNG (esp_random), 不用播种 | `luat_lib_util.c:449` |
| `app.util.randf` | （源码未写注释） | `luat_lib_util.c:450` |
| `app.util.crc32` | 校验 / 编码 * app.util.crc32(s) → 8 位十六进制字符串 (如 "e413aa54") * app.util.b64(s) → base64 串 * app.util.unb64(s) → 原始串 \| nil(格式错… | `luat_lib_util.c:451` |
| `app.util.b64` | （源码未写注释） | `luat_lib_util.c:452` |
| `app.util.unb64` | （源码未写注释） | `luat_lib_util.c:453` |
| `app.util.serialize` | （源码未写注释） | `luat_lib_util.c:454` |
| `app.util.unserialize` | （源码未写注释） | `luat_lib_util.c:455` |
| `app.util.epoch` | 时间 * app.util.epoch() → "1700000000" (字符串, 32 位整数装不下 10 位数) * app.util.sleep(ms) → 让出 CPU 指定毫秒 (不要用在大循环里, 会掉帧) | `luat_lib_util.c:456` |
| `app.util.sleep` | （源码未写注释） | `luat_lib_util.c:457` |
| `app.util.clamp` | 游戏常用数学 * app.util.clamp(v,lo,hi) / lerp(a,b,t) / dist(x1,y1,x2,y2) / round(v) / sign(v) * 这些用 Lua 写也不难, 但每个游戏都要写一遍且容易写错边… | `luat_lib_util.c:458` |
| `app.util.lerp` | （源码未写注释） | `luat_lib_util.c:459` |
| `app.util.dist` | （源码未写注释） | `luat_lib_util.c:460` |
| `app.util.round` | （源码未写注释） | `luat_lib_util.c:461` |
| `app.util.sign` | （源码未写注释） | `luat_lib_util.c:462` |

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

## `wiki` — 

| API | 说明 | 定义 |
|---|---|---|
| `app.wiki.open` | app.wiki.open([dir]) -> 篇数 \| nil,错误 * dir 省略时自动找 /sdcard/wiki 下第一个库 | `luat_lib_wiki.c:160` |
| `app.wiki.info` | app.wiki.info() -> {count=, name=} | `luat_lib_wiki.c:161` |
| `app.wiki.search` | app.wiki.search(q[, max]) -> {{id=,title=},...} * 标题前缀匹配（大小写敏感，和索引排序一致） | `luat_lib_wiki.c:162` |
| `app.wiki.title` | app.wiki.title(id) -> string | `luat_lib_wiki.c:163` |
| `app.wiki.search_py` | 库没拼音索引时返回空表 | `luat_lib_wiki.c:164` |
| `app.wiki.titles` | app.wiki.titles(from[, n]) -> {{id=,title=},...} 浏览列表用，一次读一段 | `luat_lib_wiki.c:165` |
| `app.wiki.load` | app.wiki.load(id) -> 行数 \| nil,错误 | `luat_lib_wiki.c:166` |
| `app.wiki.line` | app.wiki.line(i) -> string 0 基 | `luat_lib_wiki.c:167` |
| `app.wiki.lines` | app.wiki.lines() -> int | `luat_lib_wiki.c:168` |
| `app.wiki.close` | app.wiki.close() | `luat_lib_wiki.c:169` |

---

## 约定（所有 API 通用）

- 动作码：`1=UP 2=DOWN 3=LEFT 4=RIGHT 5=CONFIRM(OK) 6=BACK 7=HOME`；
  `app.UP..app.HOME` 与此一一对应，`consts` 模块用 `_Static_assert` 与 `os_action_t` 钉死。
- 颜色：仅黑白两色，`app.BLACK=0` / `app.WHITE=1`（反射式 1bpp 屏，白纸黑字）。
- 屏幕：`400×300`；16px 中文请用 **`app.zh`**（`font_zh16` 真 16×16，与系统 UI 同款），
  ⛔ 不要用 `app.zhsb`（那是 24×24 降采样，会糊）。ASCII 用 `app.textsb`（8×16）。
- 回调契约：`on_enter / render / poll / action(a) / on_exit`；按键**只走 `action(a)`** 一条路。
