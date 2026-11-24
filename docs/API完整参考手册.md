# API 完整参考手册

> 查接口用这份。Lua API 真相源 = `components/apps/lua_bridge/luat_lib_*.c`；
> C facade 真相源 = `components/system/os/include/os_app_api.h`、`components/engines/engine_api/include/engine_api.h`。

**稳定性**：第一部分全部 API 与两个 facade 头 → 承诺稳定，改名/删函数算破坏性变更；
facade 转发到的组件头 → 只增不改，改签名算 ABI 事故；不在 facade 里的内部头 → ❌ 不承诺，应用不得使用。
⛔ 应用直接 `#include` 驱动/服务头 = 分层违例，`tools/check_deps.py` 会拦下（构建失败）。

---

# 第一部分 Lua 公共 API

> 本节由 `tools/lua_api_doc.py` 生成，**不要手改** —— 改源码注释后重跑脚本即可。

<!-- LUA-API-BEGIN 本节由 tools/lua_api_doc.py 生成，勿手改 -->

> 由 `tools/lua_api_doc.py` 从 `components/apps/lua_bridge/luat_lib_*.c` **自动提取**。
> 「说明」一列取的是源码里函数上方的注释 —— **改代码就是改文档**，不要手改本节。
> 共 **233** 个 API / 19 个模块。「定义」列是源码位置。

### `espnow` — ESP-NOW 点对点/广播（≤238 字节/包，不用路由器；双机联机、遥控、信标）

| API | 说明 | 定义 |
|---|---|---|
| `app.espnow.start` | app.espnow.start() → bool 幂等: 会顺带把 WiFi 射频拉起来 | `luat_lib_espnow.c:306` |
| `app.espnow.stop` | app.espnow.stop() → bool 只关 esp-now, 不动用户的 WiFi | `luat_lib_espnow.c:307` |
| `app.espnow.ready` | app.espnow.ready() → bool | `luat_lib_espnow.c:308` |
| `app.espnow.mac` | app.espnow.mac() → "aa:bb:..." 本机 MAC (= WiFi STA MAC, 就是别人要发给你的地址) | `luat_lib_espnow.c:309` |
| `app.espnow.name` | app.espnow.name([s]) → string 读/写本机设备名 (发现应答里带出去, 最长 15 字节) | `luat_lib_espnow.c:310` |
| `app.espnow.channel` | app.espnow.channel() → int 查当前信道 * app.espnow.channel(n) → int 强制切到 n 信道 (1..13), 返回切完的信道 * ⚠ 连了路由器时信道由 AP 决定, 强制切会掉线 ——… | `luat_lib_espnow.c:311` |
| `app.espnow.scan` | app.espnow.scan([ms]) → bool 开始设备发现 (默认 1500ms 窗口, 上限 20000) * 广播 DISCOVER, 同信道的其它 LinTOS 会回 ANNOUNCE | `luat_lib_espnow.c:312` |
| `app.espnow.scanning` | app.espnow.scanning() → bool 是否还在发现窗口内 | `luat_lib_espnow.c:313` |
| `app.espnow.devices` | app.espnow.devices() → table 已发现的设备列表 (数组, 可直接 ipairs) * 每项: { mac="aa:bb:..", name="..", rssi=-42, seen=<毫秒前>, paired=b… | `luat_lib_espnow.c:314` |
| `app.espnow.pair` | 加密对端最多 6 台 * (ESP-NOW 驱动硬限制), 明文 peer 总数最多 20 | `luat_lib_espnow.c:315` |
| `app.espnow.unpair` | app.espnow.unpair(mac) → bool 解除配对 (同时通知对端删掉本机) | `luat_lib_espnow.c:316` |
| `app.espnow.paired` | app.espnow.paired() → int 已配对设备数 (掉电保存, 重启后仍在) | `luat_lib_espnow.c:317` |
| `app.espnow.add` | 发给某个固定对端前必须先 add (广播不用) | `luat_lib_espnow.c:318` |
| `app.espnow.del` | app.espnow.del(mac) → bool | `luat_lib_espnow.c:319` |
| `app.espnow.send` | app.espnow.send(mac, data) → bool 点对点 (≤238 字节) | `luat_lib_espnow.c:320` |
| `app.espnow.bcast` | app.espnow.bcast(data) → bool 广播 (同信道所有未加密设备都能收到) | `luat_lib_espnow.c:321` |
| `app.espnow.recv` | Lua 字符串是二进制安全的, 直接用 | `luat_lib_espnow.c:322` |
| `app.espnow.pending` | app.espnow.pending() → int 队列里还有几条没取 | `luat_lib_espnow.c:323` |
| `app.espnow.stats` | app.espnow.stats() → table {rx, drop, tx, fail, ctrl, bad, rssi} 排障用 | `luat_lib_espnow.c:324` |
| `app.espnow.info` | app.espnow.info() → table 一次性状态快照 * { running=bool, mac="..", channel=int, name="..", paired=int, devices=int, scan=bool… | `luat_lib_espnow.c:325` |

### `fs` — 文件（TF 卡，`fs` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.fs.exists` | app.fs.* : TF 卡文件存取 | `luat_lib_fs.c:172` |
| `app.fs.read` | （源码未写注释） | `luat_lib_fs.c:173` |
| `app.fs.read_at` | （源码未写注释） | `luat_lib_fs.c:174` |
| `app.fs.write` | 与 web_files 的上传路径同一口径: 收尾失败 = 没落盘 | `luat_lib_fs.c:175` |
| `app.fs.append` | （源码未写注释） | `luat_lib_fs.c:176` |
| `app.fs.remove` | （源码未写注释） | `luat_lib_fs.c:177` |
| `app.fs.rename` | 录音机改文件名、整理目录都用它 | `luat_lib_fs.c:178` |
| `app.fs.size` | app.fs.size(path) -> 字节数 \| nil * 读之前先问大小: 分块地图的最小/最大块判据、下载落地自检都要用它 | `luat_lib_fs.c:179` |
| `app.fs.mkdir` | 已存在也返回 true (幂等), 免得调用方每次都要先 exists() 判一遍 | `luat_lib_fs.c:180` |
| `app.fs.listdir` | （源码未写注释） | `luat_lib_fs.c:181` |

### `gfx` — 绘制与字模（图元 / 8 套字模 / 位图 / 字体宽度）

| API | 说明 | 定义 |
|---|---|---|
| `app.clear` | （源码未写注释） | `luat_lib_gfx.c:471` |
| `app.px` | （源码未写注释） | `luat_lib_gfx.c:472` |
| `app.fill` | （源码未写注释） | `luat_lib_gfx.c:473` |
| `app.store_rect` | （源码未写注释） | `luat_lib_gfx.c:474` |
| `app.rect` | （源码未写注释） | `luat_lib_gfx.c:475` |
| `app.statusbar` | 应用只表态, 时间/电量/Wi-Fi/蓝牙由 OS 状态栏服务每帧刷新 | `luat_lib_gfx.c:476` |
| `app.statusbar_h` | 内容起点一律用 app.statusbar_h(), 不要写死 20/24 (写死值一旦与系统不一致, * 顶栏就会被盖掉几行或留一条空白) | `luat_lib_gfx.c:477` |
| `app.hline` | （源码未写注释） | `luat_lib_gfx.c:478` |
| `app.vline` | （源码未写注释） | `luat_lib_gfx.c:479` |
| `app.text` | （源码未写注释） | `luat_lib_gfx.c:480` |
| `app.textc` | （源码未写注释） | `luat_lib_gfx.c:481` |
| `app.textw` | （源码未写注释） | `luat_lib_gfx.c:482` |
| `app.big` | （源码未写注释） | `luat_lib_gfx.c:483` |
| `app.bigc` | （源码未写注释） | `luat_lib_gfx.c:484` |
| `app.zh` | （源码未写注释） | `luat_lib_gfx.c:485` |
| `app.zh24` | （源码未写注释） | `luat_lib_gfx.c:486` |
| `app.bmp` | （源码未写注释） | `luat_lib_gfx.c:487` |
| `app.bmpr` | 数据走 luaL_checklstring (长度感知, 二进制安全, 不做 UTF-8 转换) | `luat_lib_gfx.c:488` |
| `app.zhs` | app.zhs(x,y,str[,inv]): 12×12 中文点阵 (23×23 字库 2×2 降采样, 照 page_diag dzh) | `luat_lib_gfx.c:489` |
| `app.texts` | 下面这些 API 一一对应 ui_common.c 里的固件字体函数 (各 C 版应用页面用的就是它们), * 于是 Lua 版与 C 版 UI 同源同形: * app.texts 8×12 ASCII (draw_ascii_small)… | `luat_lib_gfx.c:490` |
| `app.textt` | 字形与 app.text 逐像素一致 | `luat_lib_gfx.c:491` |
| `app.textm` | （源码未写注释） | `luat_lib_gfx.c:492` |
| `app.textsb` | （源码未写注释） | `luat_lib_gfx.c:493` |
| `app.zhsm` | （源码未写注释） | `luat_lib_gfx.c:494` |
| `app.zhsb` | （源码未写注释） | `luat_lib_gfx.c:495` |
| `app.mix` | （源码未写注释） | `luat_lib_gfx.c:496` |
| `app.mixc` | （源码未写注释） | `luat_lib_gfx.c:497` |
| `app.mixw` | （源码未写注释） | `luat_lib_gfx.c:498` |
| `app.poly` | （源码未写注释） | `luat_lib_gfx.c:499` |
| `app.rotate` | （源码未写注释） | `luat_lib_gfx.c:500` |

### `gps` — 定位（NMEA 解析结果，经纬度走整数微度）

| API | 说明 | 定义 |
|---|---|---|
| `app.gps.start` | 启动顺序必须是 唤醒 → 再起服务 | `luat_lib_gps.c:241` |
| `app.gps.start_async` | 需要"不可用就走模拟"的仍用 start() | `luat_lib_gps.c:242` |
| `app.gps.stop` | app.gps.stop() → bool: 停后台解析任务 + **把模块送回待机** (2026-10-09) * 应用退出时务必调用: 否则解析任务常驻, 且 GPS 会保持 ~10mA 连续跟踪 * (待机只需 46µA) —— 直… | `luat_lib_gps.c:243` |
| `app.gps.fix` | app.gps.fix() → table \| nil * 全是整数定标值 (见文件头), 括号里是还原公式 | `luat_lib_gps.c:244` |
| `app.gps.sats` | sys: 0=GPS 1=GLONASS 2=Galileo 3=BeiDou 4=QZSS 5=NavIC 6=混合 * el=仰角 0..90, az=方位 0..359 (真北=0 顺时针), snr=-1 表示无, used=1 参… | `luat_lib_gps.c:245` |
| `app.gps.demo` | app.gps.demo([on]) → bool: 开/关演示数据 (不读 UART) | `luat_lib_gps.c:246` |
| `app.gps.reset` | app.gps.reset(): 清定位状态与统计 (不关演示开关) | `luat_lib_gps.c:247` |

### `haptic` — 触觉效果库（24 个具名效果 + 自定义序列；`haptic` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.haptic.play` | app.haptic.play(name) -> ok, reason | `luat_lib_haptic.c:135` |
| `app.haptic.sequence` | app.haptic.sequence{ {strength=0..100, duration=ms}, ... } -> ok, reason | `luat_lib_haptic.c:136` |
| `app.haptic.stop` | 没在播时什么都不做(不会误伤别的振动) | `luat_lib_haptic.c:137` |
| `app.haptic.active` | app.haptic.active() -> 是否正在播放效果 (bool) | `luat_lib_haptic.c:138` |
| `app.haptic.list` | app.haptic.list() -> 数组, 每项 {name, ms, pulses, strength, priority, cooldown} | `luat_lib_haptic.c:139` |

### `hid` — USB 键鼠（`hid` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.hid.start` | （源码未写注释） | `luat_lib_hid.c:127` |
| `app.hid.stop` | （源码未写注释） | `luat_lib_hid.c:128` |
| `app.hid.connected` | （源码未写注释） | `luat_lib_hid.c:129` |
| `app.hid.running` | app.hid.running(): HID 栈是否已启动 (区别于 connected: 对端是否枚举上来) | `luat_lib_hid.c:130` |
| `app.hid.key` | （源码未写注释） | `luat_lib_hid.c:131` |
| `app.hid.key_str` | （源码未写注释） | `luat_lib_hid.c:132` |
| `app.hid.mouse_move` | （源码未写注释） | `luat_lib_hid.c:133` |
| `app.hid.mouse_click` | （源码未写注释） | `luat_lib_hid.c:134` |
| `app.hid.shortcut` | （源码未写注释） | `luat_lib_hid.c:135` |
| `app.hid.cdc_read` | } [modsplit] 共享 | `luat_lib_hid.c:136` |
| `app.hid.cdc_connected` | app.hid.cdc_connected(): 目标电脑的 CDC 串口是否已连接 (日志诊断页显示 USB 状态用) | `luat_lib_hid.c:137` |

### `img` — 大图与精灵（`img` 子表：1bpp BMP 载入 PSRAM + 窗口位块传输，地图/角色用）

| API | 说明 | 定义 |
|---|---|---|
| `app.img.load` | app.img.load(path[, opaque]) -> id \| nil | `luat_lib_img.c:281` |
| `app.img.load_mem` | 单文件应用靠它把"包里的地图"直接变成可 blit 的图, **不落盘、不建目录 | `luat_lib_img.c:282` |
| `app.img.blit` | #define IMG_ROW_MAX 64 上限 512 像素宽 (我们的屏 400) | `luat_lib_img.c:283` |
| `app.img.size` | app.img.size(id) -> w, h | `luat_lib_img.c:284` |
| `app.img.free` | app.img.free(id) / app.img.freeall() | `luat_lib_img.c:285` |
| `app.img.freeall` | （源码未写注释） | `luat_lib_img.c:286` |

### `input` — 时间与输入（按键 / 按住状态 / 自动重复 / 触摸 / 重绘）

| API | 说明 | 定义 |
|---|---|---|
| `app.millis` | Lua: 时间/输入/声音/震动/退出 | `luat_lib_input.c:215` |
| `app.key` | （源码未写注释） | `luat_lib_input.c:216` |
| `app.delay` | （源码未写注释） | `luat_lib_input.c:217` |
| `app.key_down` | 做"按住持续移动/连发"全靠它 | `luat_lib_input.c:218` |
| `app.keys_mask` | 支持多键同时: `if m & 1 ~= 0 and m & 4 ~= 0` = 左上一起按(斜向) | `luat_lib_input.c:219` |
| `app.key_repeat` | （源码未写注释） | `luat_lib_input.c:220` |
| `app.touch` | ⚠ 脚本 poll() 里 `local x,y = app.touch()` 的写法天然兼容(nil 就是没触摸), * 不需要改任何应用 | `luat_lib_input.c:221` |
| `app.touch_start` | 无手势返回 nil * 与 app.touch() 同帧使用: 起点固定、当前点变化, 两者相减即位移 | `luat_lib_input.c:222` |
| `app.touch_hold` | 脚本据此实现"长按" | `luat_lib_input.c:223` |
| `app.redraw` | app.redraw(): 标记本帧需重绘刷屏. Lua 应用 poll() 里逐帧刷新用(雷达/触屏绘制). | `luat_lib_input.c:224` |
| `app.vk` | page_lua.c 读它决定 enable / draw / 按键映射 | `luat_lib_input.c:225` |

### `json` — 

| API | 说明 | 定义 |
|---|---|---|
| `app.json.pick` | app.json.pick(json, array_key, fields[, max]) -> 表数组 | `luat_lib_json.c:325` |
| `app.json.num` | app.json.num / str / bool | `luat_lib_json.c:326` |
| `app.json.str` | （源码未写注释） | `luat_lib_json.c:327` |
| `app.json.bool` | （源码未写注释） | `luat_lib_json.c:328` |

### `kbd` — 软键盘（`kbd` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.kbd.use` | app.kbd.use(style[, ...]) — 一次选定 样式 + 几何 + 布局表 | `luat_lib_kbd.c:177` |
| `app.kbd.reset` | （源码未写注释） | `luat_lib_kbd.c:178` |
| `app.kbd.draw` | （源码未写注释） | `luat_lib_kbd.c:179` |
| `app.kbd.hit` | （源码未写注释） | `luat_lib_kbd.c:180` |
| `app.kbd.geom` | （源码未写注释） | `luat_lib_kbd.c:181` |
| `app.kbd.height` | （源码未写注释） | `luat_lib_kbd.c:182` |
| `app.kbd.count` | （源码未写注释） | `luat_lib_kbd.c:183` |
| `app.kbd.press` | （源码未写注释） | `luat_lib_kbd.c:184` |
| `app.kbd.char` | （源码未写注释） | `luat_lib_kbd.c:185` |
| `app.kbd.label` | （源码未写注释） | `luat_lib_kbd.c:186` |
| `app.kbd.layer_on` | （源码未写注释） | `luat_lib_kbd.c:187` |
| `app.kbd.get` | （源码未写注释） | `luat_lib_kbd.c:188` |
| `app.kbd.set` | （源码未写注释） | `luat_lib_kbd.c:189` |
| `app.kbd.preview` | 一句话: 按名字把一套真实键盘画到 (x,y) 并返回像素高, 未知名字返回 -1 | `luat_lib_kbd.c:190` |

### `net` — 网络（`net` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.net.is_connected` | （源码未写注释） | `luat_lib_net.c:340` |
| `app.net.mirrors` | app.net.* : 网络状态 / HTTP / ping app.net.mirrors() → { "https: bbk.linit.cn/bbk", ... } * * 分发包的**镜像源清单**, 取自固件里的 os_sourc… | `luat_lib_net.c:341` |
| `app.net.get_ip` | （源码未写注释） | `luat_lib_net.c:342` |
| `app.net.http_get` | 现在改成 net_client 的同步包装 —— 行为不变 (调用方仍阻塞等结果), 但底层只剩一个客户端 | `luat_lib_net.c:343` |
| `app.net.http_url` | 失败 → nil, 0 | `luat_lib_net.c:344` |
| `app.net.fetch_start` | 本次请求是否流式落盘 (决定 poll 返回几个值) | `luat_lib_net.c:345` |
| `app.net.fetch_poll` | （源码未写注释） | `luat_lib_net.c:346` |
| `app.net.fetch_got` | 非流式也能看"收了多少") | `luat_lib_net.c:347` |
| `app.net.ping` | （源码未写注释） | `luat_lib_net.c:348` |
| `app.net.port` | V1.2 新增: 局域网扫描 (netscan 服务) —— 解锁 netdect 类应用 | `luat_lib_net.c:349` |
| `app.net.arp` | （源码未写注释） | `luat_lib_net.c:350` |
| `app.net.arp_list` | （源码未写注释） | `luat_lib_net.c:351` |
| `app.net.ipinfo` | 返回 ip, 掩码, 网关, DNS, 接口名 (未联网 / 无 netif 时返回 0 个值) | `luat_lib_net.c:352` |
| `app.net.vendor` | 未命中返回 nil (OUI 字典常驻固件, 与 netdect 的厂商显示同源) | `luat_lib_net.c:353` |

### `pkg` — 

| API | 说明 | 定义 |
|---|---|---|
| `app.pkg.self` | app.pkg.self() | `luat_lib_pkg.c:378` |
| `app.pkg.open` | app.pkg.open(path) | `luat_lib_pkg.c:379` |
| `app.pkg.exists` | app.pkg.exists(h, name) | `luat_lib_pkg.c:380` |
| `app.pkg.size` | app.pkg.size(h, name) | `luat_lib_pkg.c:381` |
| `app.pkg.read` | app.pkg.read(h, name) -> string | `luat_lib_pkg.c:382` |
| `app.pkg.list` | app.pkg.list(h) -> {name=size} | `luat_lib_pkg.c:383` |
| `app.pkg.close` | app.pkg.close(h) | `luat_lib_pkg.c:384` |

### `rec` — 

| API | 说明 | 定义 |
|---|---|---|
| `app.rec.start` | （源码未写注释） | `luat_lib_rec.c:172` |
| `app.rec.stop` | （源码未写注释） | `luat_lib_rec.c:173` |
| `app.rec.active` | （源码未写注释） | `luat_lib_rec.c:174` |
| `app.rec.ms` | （源码未写注释） | `luat_lib_rec.c:175` |
| `app.rec.bytes` | （源码未写注释） | `luat_lib_rec.c:176` |
| `app.rec.peak` | （源码未写注释） | `luat_lib_rec.c:177` |
| `app.rec.mic_available` | （源码未写注释） | `luat_lib_rec.c:178` |
| `app.rec.spec` | app.rec.spec(bin) -> 0~100 第 bin 段能量 (录音时才有值) | `luat_lib_rec.c:179` |
| `app.rec.spec_bins` | （源码未写注释） | `luat_lib_rec.c:180` |
| `app.rec.play` | （源码未写注释） | `luat_lib_rec.c:181` |
| `app.rec.mplay` | 保留旧函数名只为不破坏已上架应用(录音机/电子木鱼), 语义已对齐 | `luat_lib_rec.c:182` |
| `app.rec.mplaying` | （源码未写注释） | `luat_lib_rec.c:183` |
| `app.rec.mstop` | （源码未写注释） | `luat_lib_rec.c:184` |
| `app.rec.stop_play` | （源码未写注释） | `luat_lib_rec.c:185` |
| `app.rec.playing` | （源码未写注释） | `luat_lib_rec.c:186` |
| `app.rec.play_ms` | （源码未写注释） | `luat_lib_rec.c:187` |
| `app.rec.play_total_ms` | （源码未写注释） | `luat_lib_rec.c:188` |
| `app.rec.preload` | 之后用 play_buf 播放零 IO, 不碰 flash、不关 cache | `luat_lib_rec.c:190` |
| `app.rec.play_buf` | 播放已预载的音效缓存 (异步) | `luat_lib_rec.c:191` |
| `app.rec.unload` | 释放 PSRAM 里的音效缓存, 无返回值 | `luat_lib_rec.c:192` |
| `app.rec.buf_ready` | 预载缓存是否可用 | `luat_lib_rec.c:193` |

### `store` — 持久化（`nvs` 掉电保留、`db` 核心互通库）

| API | 说明 | 定义 |
|---|---|---|
| `app.nvs.get_int` | （源码未写注释） | `luat_lib_store.c:330` |
| `app.nvs.set_int` | （源码未写注释） | `luat_lib_store.c:331` |
| `app.nvs.get_str` | （源码未写注释） | `luat_lib_store.c:332` |
| `app.nvs.set_str` | （源码未写注释） | `luat_lib_store.c:333` |
| `app.db.set` | （源码未写注释） | `luat_lib_store.c:336` |
| `app.db.get` | （源码未写注释） | `luat_lib_store.c:337` |
| `app.db.del` | （源码未写注释） | `luat_lib_store.c:338` |
| `app.db.seti` | （源码未写注释） | `luat_lib_store.c:339` |
| `app.db.geti` | （源码未写注释） | `luat_lib_store.c:340` |

### `sys` — 系统与提示（日志 / 音效 / 震动 / 退出 / 状态栏，以及 `sys`、`dlg` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.log` | 用定长 C 缓冲更稳 | `luat_lib_sys.c:295` |
| `app.tone` | （源码未写注释） | `luat_lib_sys.c:296` |
| `app.tone_async` | app.tone_async(freq, ms[, amp]) → 非阻塞单音 (不卡帧). 连打 = 天然排队成小旋律 | `luat_lib_sys.c:297` |
| `app.melody` | （源码未写注释） | `luat_lib_sys.c:298` |
| `app.vibrate` | （源码未写注释） | `luat_lib_sys.c:299` |
| `app.volume` | ⚠ 与系统音量是**同一个值**: 应用改完退出, 系统里看到的也是新值(符合直觉) | `luat_lib_sys.c:301` |
| `app.mute` | 与应用内的"暂停"是两件事 | `luat_lib_sys.c:302` |
| `app.bt` | app.bt() → 0=未启用 1=射频就绪(未连) 2=已连接(手柄/耳机) * 应用不需要自己开蓝牙: 开机不拉射频, 进设置页/手柄页才由 os_hw 拉起 | `luat_lib_sys.c:303` |
| `app.exit` | （源码未写注释） | `luat_lib_sys.c:304` |
| `app.exit_confirm` | 只在脚本没消费 BACK 时生效: 脚本把 BACK 当返回键用 (关子窗/收菜单) 依旧优先 | `luat_lib_sys.c:305` |
| `app.sbar` | 注意: 脚本自己负责在 on_exit 里还原成 false, 否则会带到下一页 | `luat_lib_sys.c:306` |
| `app.sys.battery` | app.sys.battery(): 返回电压 mV 或 nil | `luat_lib_sys.c:308` |
| `app.sys.heap_free` | （源码未写注释） | `luat_lib_sys.c:309` |
| `app.sys.time_ms` | （源码未写注释） | `luat_lib_sys.c:310` |
| `app.sys.date` | ★ 2026-10-30: app.sys.date() → month, day, weekday(1=周日..7=周六) * 用途: Lua 全屏页要自绘状态栏（系统不画），需要和桌面一致的"10-30 星期五" | `luat_lib_sys.c:311` |
| `app.sys.clock` | （源码未写注释） | `luat_lib_sys.c:312` |
| `app.dlg.toast` | 这样完全贴合已有契约 (on_enter/render/poll/action), 不需要动桥的调用约定 | `luat_lib_sys.c:315` |
| `app.dlg.confirm` | app.dlg.confirm(msg): 通用确认框 (确定=0 / 取消=-1) | `luat_lib_sys.c:316` |
| `app.dlg.list` | 不传或传 nil = 与旧行为完全一致 | `luat_lib_sys.c:317` |
| `app.dlg.result` | 契约本来就写着"由 app.dlg.result() 取走", 这里把实现补齐 | `luat_lib_sys.c:318` |

### `util` — 

| API | 说明 | 定义 |
|---|---|---|
| `app.util.after` | （源码未写注释） | `luat_lib_util.c:450` |
| `app.util.every` | （源码未写注释） | `luat_lib_util.c:451` |
| `app.util.cancel` | （源码未写注释） | `luat_lib_util.c:452` |
| `app.util.rand` | rand(n) = 1..n * app.util.randf() → [0,1) 浮点 * 硬件 TRNG (esp_random), 不用播种 | `luat_lib_util.c:453` |
| `app.util.randf` | （源码未写注释） | `luat_lib_util.c:454` |
| `app.util.crc32` | 校验 / 编码 * app.util.crc32(s) → 8 位十六进制字符串 (如 "e413aa54") * app.util.b64(s) → base64 串 * app.util.unb64(s) → 原始串 \| nil(格式错… | `luat_lib_util.c:455` |
| `app.util.b64` | （源码未写注释） | `luat_lib_util.c:456` |
| `app.util.unb64` | （源码未写注释） | `luat_lib_util.c:457` |
| `app.util.serialize` | （源码未写注释） | `luat_lib_util.c:458` |
| `app.util.unserialize` | （源码未写注释） | `luat_lib_util.c:459` |
| `app.util.epoch` | 时间 * app.util.epoch() → "1700000000" (字符串, 32 位整数装不下 10 位数) * app.util.sleep(ms) → 让出 CPU 指定毫秒 (不要用在大循环里, 会掉帧) | `luat_lib_util.c:460` |
| `app.util.sleep` | （源码未写注释） | `luat_lib_util.c:461` |
| `app.util.clamp` | 游戏常用数学 * app.util.clamp(v,lo,hi) / lerp(a,b,t) / dist(x1,y1,x2,y2) / round(v) / sign(v) * 这些用 Lua 写也不难, 但每个游戏都要写一遍且容易写错边… | `luat_lib_util.c:462` |
| `app.util.lerp` | （源码未写注释） | `luat_lib_util.c:463` |
| `app.util.dist` | （源码未写注释） | `luat_lib_util.c:464` |
| `app.util.round` | （源码未写注释） | `luat_lib_util.c:465` |
| `app.util.sign` | （源码未写注释） | `luat_lib_util.c:466` |

### `wifi` — 无线（`wifi` 子表）

| API | 说明 | 定义 |
|---|---|---|
| `app.wifi.init` | app.wifi.* : 无线探测 | `luat_lib_wifi.c:134` |
| `app.wifi.scan` | （源码未写注释） | `luat_lib_wifi.c:135` |
| `app.wifi.scan_done` | （源码未写注释） | `luat_lib_wifi.c:136` |
| `app.wifi.ap_count` | （源码未写注释） | `luat_lib_wifi.c:137` |
| `app.wifi.ap_info` | （源码未写注释） | `luat_lib_wifi.c:138` |
| `app.wifi.probe_init` | （源码未写注释） | `luat_lib_wifi.c:139` |
| `app.wifi.probe_stop` | （源码未写注释） | `luat_lib_wifi.c:140` |
| `app.wifi.probe_count` | （源码未写注释） | `luat_lib_wifi.c:141` |
| `app.wifi.probe_info` | （源码未写注释） | `luat_lib_wifi.c:142` |
| `app.wifi.release` | （源码未写注释） | `luat_lib_wifi.c:143` |
| `app.wifi.ch_stat` | （源码未写注释） | `luat_lib_wifi.c:144` |
| `app.wifi.cur_ssid` | 联网链 (解锁 netdect 的「选网 → 输密码 → 连接」) | `luat_lib_wifi.c:145` |
| `app.wifi.connecting` | （源码未写注释） | `luat_lib_wifi.c:146` |
| `app.wifi.connect` | （源码未写注释） | `luat_lib_wifi.c:147` |
| `app.wifi.setup` | 另有 app.wifi.connected(): 是否已联网 (只读查询, 门禁/离线提示用) | `luat_lib_wifi.c:148` |
| `app.wifi.connected` | （源码未写注释） | `luat_lib_wifi.c:149` |
| `app.wifi.clear_saved` | （源码未写注释） | `luat_lib_wifi.c:150` |

### `wiki` — 

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

## 通用约定

- 动作码：`1=UP 2=DOWN 3=LEFT 4=RIGHT 5=CONFIRM(OK) 6=BACK 7=HOME`；
  `app.UP..app.HOME` 与此一一对应，`consts` 模块用 `_Static_assert` 与 `os_action_t` 钉死。
- 颜色：仅黑白两色，`app.BLACK=0` / `app.WHITE=1`（反射式 1bpp 屏，白纸黑字）。
- 屏幕：`400×300`；16px 中文请用 **`app.zh`**（`font_zh16` 真 16×16，与系统 UI 同款），
  ⛔ 不要用 `app.zhsb`（16×16 由 22/24 采样降下来，会糊）。ASCII 用 `app.textsb`（8×16）。
- 回调契约：`on_enter / render / poll / action(a) / on_exit`；按键主路径是 `action(a)`，
  桥同时把键压进按键状态，所以 `app.key()` / `app.key_down()` 也能读到同一批键。

<!-- LUA-API-END -->

---

# 第二部分 C facade 层

## 2.1 分工

```
L4 应用  ──只许 include──▶  os_app_api.h   （os / 绘制 / 输入 / 存储 / 网络 / 音频 / 射频）
L3 引擎  ──只许 include──▶  engine_api.h   （显示 / 绘制 / 输入 / 虚拟键 / shim / 音频）
                             ▲ tools/check_deps.py 静态校验，违例即构建失败
```

- 两个头都是**纯转发**，不实现功能；应用不碰寄存器，引擎不反向依赖 os。
- 引擎需要的上层动作（退出确认、状态栏、电源事件）用**回调注入**（`set_*_cb`），⛔ 不许 `#include "os.h"`。

## 2.2 `os_app_api.h`（应用层）

转发：系统契约（`os.h` 页面栈/弹窗/注册/动作码）· 绘制（st7305/ui_common/font_zh/font_zh16）· 输入（input）·
存储（sd_scan/svc_nvs/svc_cfg）· 安全（keyvault）· 网络（wifi_manager）· 蓝牙（bt_manager）· USB（usb_hid）·
音频与设备（audio_player/tone_player/vibrator/board_battery）· 引擎管家（engine_manager）· 射频计数（os_hw）· 专用（espsdr）。

## 2.3 `engine_api.h`（引擎层）

转发：显示（st7305）· 绘制（ui_common/fonts）· 输入（input）· 覆盖层（virtual_keys）· 适配（board_shim）·
金手指（cheater）· 手柄（bt_manager/web_gamepad）· 音频（audio_player）。

⚠ 头注释原文："现有引擎尚未全量迁移，新代码必须直接用本 facade"；老引擎直连 include 是历史欠债，不是范例。

## 2.4 新增能力

1. 定层：应用 → `os_app_api.h`；引擎 → `engine_api.h`。
2. 在**归属组件**实现（facade 只转发）。
3. facade 加一行 `#include`，并更新上一节的转发清单。
   （`caps` 能力位见 [系统开发](系统开发.md) 分层节。）

## 2.5 不算公共 API

组件私有头（未进 `include/`）· `static` / `*_internal` 函数 · 引擎上游第三方符号（gwenesis/sdlpal/smsplus）· 未文档化的 `remcon` 命令。

---

# 第三部分 返回值与错误处理

- **C**：`esp_err_t` 必须判，失败要降级 + 打日志；⛔ 不用 `ESP_ERROR_CHECK` 兜业务错误（会 abort 重启整机）。
  申请/释放成对（`os_hw_request` ↔ `os_hw_release`、每条 return 路径都 `free`），句柄用完置 `NULL`。
- **Lua**：`app.touch()` 没按时**返回 0 个值**（≠ `nil`），判断用 `type(x)=="number"`；
  `fs.write` / `rename` / `mkdir` 返回 bool 必须判；`fs.read`、`tone`、`net.http_get` 阻塞，正式路径别用阻塞版；
  `dlg.confirm` / `list` 发出即返回，下一帧用 `dlg.result()` 取。

## 参考

- 写 Lua 应用 → [Lua应用](Lua应用.md)
- 写 C 应用 / 架构与内存 / 审查标准 → [系统开发](系统开发.md)
- 加 API 流程 → [开发总索引](开发总索引.md)「增加 Lua API」
