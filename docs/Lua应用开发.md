# Lua 应用开发

> 写一个能进商店的 Lua 应用，照这篇做即可。API 全表见 `Lua桥API参考.md`（自动生成，不要手改）。
> 历史设计与踩坑过程在 `archive/`。

## 1 文件与目录

- 一个应用 = 一个 `.lua` 文件，放 `apps/lua/`（或子目录），发布时同步到 SPIFFS `apps` 分区。
- 目录与分类必须自洽：`apps/` → `cat=软件`、`engine/` → `cat=游戏`、`ops/` → `cat=运维`。
- 商店索引里 `cat` 只能是 **游戏/软件/标准/运维/学习** 五类之一。

## 2 头部（LAPP 元信息）

文件开头必须是注释行，固件静态解析，不执行代码：

```lua
--@lapp v1
--@app name=QQ堂冒险 cat=游戏 desc=泡泡炸砖块探地牢 version=1 build=202610300638 icon=<base64 64x64 1bpp>
```

- `icon`：64×64 1bpp，512 字节 base64。用 `tools/pack_app.py` 从 PNG 或内置图标生成。
- `need=wifi`：启动前需要联网，固件会做门禁；不写＝不需要。
- **两种写法都支持**：一行里写多个键（上面这样），或者每行一个 `--@app key=value`
  （老包如「矢量地图」就是每行一个）。固件与上传页都能解析。
- 字段约束：`name` ≤24 字 · `cat` 只能是 **游戏/软件/标准/运维/学习** · `desc` ≤40 字。
  写 `工具/程序/应用` 会归一到「软件」，`系统/设置`→「标准」，`网络`→「运维」。
- ⚠ 版本号的键名是 **`version`**（不是 `ver`）。`tools/pack_app.py`、上传页 `server/upload/lib.php`
  和全部已上架包都只认 `version`；写 `ver` 不会报错，但版本号读不到（会被当成 1）。

### 2.1 版本号与构建时间（2026-10-30 规则）

| 字段 | 作用 | 谁写 |
|---|---|---|
| `version` | **只给人看**。`1` / `1.1` / `1.1.1` 都行，三段式随便用 | 手工（或不写，默认 1）|
| `build` | **商店判断"有没有新版"的唯一依据**。`YYYYMMDDHHMM` 12 位定长数字 | `pack_app.py` 自动写当前时间 |

- **不用再手工递增版本号**。发版只要重新 `pack` 一次，`build` 变成新时间，设备端就会看到「更新」。
- `build` 必须是 12 位纯数字、无分隔符（不能写 `2026-10-30 06:38`）——
  定长数字串的字典序 == 时间先后，设备端直接 `strcmp` 比较，不转数值。
  中间夹空格还会把 `--@app` 的 key=value 扫描切断。
- 比较规则（`am_store.c: am_pkg_has_update`）：
  1. 云端有 `build` + 本地有 `build` → `strcmp(云端, 本地) > 0` 才算有新版
  2. 云端有 `build` 但本地没有（2026-10-30 之前发布的包）→ 提示一次更新，把设备迁到新体系
  3. 云端没有 `build`（老索引）→ 退回 `云端 ver > 本地 ver`
- 资源包（`type=asset`，如仙剑的 `pal.zip`）**不写 `build`**：它没有 LAPP 头，
  设备端读不到本地构建时间，写了 `build` 反而会永远提示更新。
- `index.json` 里每条包的 `build` 由 `tools/store_build.py` 从 LAPP 头带过来。

## 3 骨架（四件套）

入口是**全局函数**，不是 `return` 表。宿主 `page_lua.c` 用 `lua_getglobal("render"/"poll"/
"action"/"on_enter"/"on_exit")` 逐个取，`return { render = ... }` **不会被识别**
（2026-10-30 按源码核实，旧版本文档写的是 return 表，照抄会写出跑不起来的应用）。

```lua
local S = { ... }                       -- 私有状态(不要用全局)

function on_enter() ... end             -- 可选: 进页面初始化
function render()   ... end             -- 只画, 不做 IO
function poll()     ... end             -- 可选: 每帧调用(触摸轮询/动画/计时)
function action(a)  ... end             -- 按键: a = app.UP/DOWN/LEFT/RIGHT/OK/CONFIRM/BACK/HOME
function on_exit()  ... end             -- 可选: 退出清理(停任务/释放资源)
```

- **触摸不走框架回调**：RUN 态 `lua_touch()` 固定返回 false，应用自己在 `poll()` 里调
  `app.touch()`（当前点）/ `app.touch_start()`（按下起点）/ `app.touch_hold()`（按住毫秒）轮询。
- `action` 里处理 BACK：返回真值 = "我消费了"，框架不退出；返回 `false`/`nil`
  = 交回框架（全屏程序默认弹「退出程序?」，见 §6）。
- ⛔ 顶层 `return true` 会吞掉退出确认框。

## 4 界面

- 屏幕 400×300，1bpp，无背光（反射式）。左右留白相等，按键贴左右两侧，不要居中竖排。
- 弹窗尺寸固定 350×250（底栏 40，分隔线左右内缩 6px）——用 `app.dlg.*` 自动对齐，别自绘。
- 字体只用这几档：
  | 用途 | API |
  |---|---|
  | 中英混排 16px | `app.mix`（汉字 16 宽，ASCII 用 FONT8X12 正常渲染）|
  | 纯汉字 12 / 24px | `app.zh` / `app.zh24` |
  | 纯 ASCII 16 / 12 | `app.texts` / `app.textsb` |
  ⛔ `app.zh` / `app.zh24` / `app.zhs` 是**汉字专用**：ASCII 落进去是空白格。
  要写「12 条 234K」「10:30」这类混排，直接用 `app.mix`，**不要拆成两次调用**
  （`ui_text16()` 里 ASCII 走 FONT8X12，本身就是为混排准备的）。
- 弹窗一律 `app.dlg.*`（list / confirm / toast / wheel）；回调里**不要** `app.dlg.pop()`，会关掉上层页面。

## 5 绘制性能

- 每帧不要 `app.fill(0,0,399,299)`（12 万像素）。只画变化区。
- 长列表用「离屏整列 + 只抄可视窗」：见 `ui_col` 范式；跟手滚动 1:1（手指移多少内容动多少），不要翻页跳格。
- `render` 里禁止文件 IO / 网络 / NVS；慢操作放后台，完成后置脏标记再重绘。
- 静态内容（边框、标题、图标）缓存一次，滚动时只重画数据行。

## 6 退出确认

全屏程序 / 游戏必须弹「退出程序?」——用 `app.sys.exit_confirm()`（等同 `os_request_exit_confirm()`），确认后由系统在下一帧 pop。

⛔ 例外（不要自己弹）：引擎类、弹窗类（usb_bt / usb_net / modtool）、页内 BACK 自带语义的（wifiprobe / netdect / diag / sdr / book / usb_hid / lua / sponsor）。

## 7 发布

### 7.1 第三方 / 远程提交（网页）

<https://bbk.linit.cn/upload/>

1. **邮箱登录**：填邮箱 → 收 6 位验证码 → 第一次用某个邮箱会自动建账号（不需要单独注册）。
2. **选文件**：页面会**自动读出** `--@app` 头里的名称、分类、说明、版本，并画出 64×64 图标预览。
   读不到或不规范的会**逐条提示缺什么**（缺 name / cat 不在五类 / icon 不是 512 字节 / 没有 render …）。
3. **提交**：包进入待审区，站点会给维护者发一封通知邮件。
4. **审核**：维护者打开审核页（能看代码预览 + 危险能力扫描），点「通过」才真正写进 `index.json`。
   通过后版本号写入索引，设备上刷新商店：**没装的显示「下载」，装了旧版的显示「更新」**。

规则：

- **版本号不用再手工递增**（见 §2.1）：商店改按 `build` 构建时间判断有没有新版。
  ⚠ 但网页上传页（`server/upload/*.php`）目前**仍按 `version` 校验递增**，
  走网页提交时还要让 `version` 比线上大，或者等上传页也改成按 `build` 判断。
- 同一个应用已经有待审的一份时，第二份会被挡住（等审核完再传）。
- 危险能力会被标出来供审核参考：`app.hid.*`（能模拟 USB 键鼠）、`app.fs.remove`、`app.net.*`、
  `app.espnow`、`app.rec.*`、直连 IP、`io./os.` 标准库逃逸。
- 上架会覆盖 `bbk/pkgs/<应用名>.lua`，旧包自动备份到 `bbk/pkgs_backup/`。

### 7.2 本地打包（自己发版）

```bash
python3 tools/sync_apps.py           # 收 apps/lua → 生成 apps_l.bin + 商店索引
esptool ... write_flash 0xC00000 build/apps_l.bin
```

- 「安装」＝复制，**永不自动启动**。
- 改动 Lua 后不需要重编固件；只有改 C 才 `build.sh`（且必须先 build 再 sync）。
- ⚠ 手工改服务器上的 `bbk/index.json` 之后**必须 `nginx -s reload`**：
  nginx 的 open_file_cache 会持有旧句柄，原地重写后设备端会拉到空内容。
  程序写入一律「写 `.tmp` → rename」。

## 8 坑清单

- `local` 只对它后面可见；`local a=1, b=2` 语法错；`{...}[k]` 不能直接索引。
- `LUA_32BITS` ⇒ 数字是 **float**（~7 位）：大地坐标用整数微度，先减后乘。
- `app.fs.read` ≤512KB，大文件用 `read_at`（≤1MB 一次）。
- `app.net.http_get` 只支持 HTTP 且 ≤4KB；HTTPS/大包用 `app.net.fetch_*`。
- 没有 `crc32` API。
- 改完内嵌网页类资源必须过 `tools/check_embed_js.py`（build_fast.sh 已内置）。
