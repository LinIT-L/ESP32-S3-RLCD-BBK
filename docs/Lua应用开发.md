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
--@app name=QQ堂冒险 cat=游戏 desc=泡泡炸砖块探地牢 icon=<base64 64x64 1bpp> ver=1
```

- `icon`：64×64 1bpp，512 字节 base64。用 `tools/pack_app.py` 从 PNG 或内置图标生成。
- `need=wifi`：启动前需要联网，固件会做门禁；不写＝不需要。
- **两种写法都支持**：一行里写多个键（上面这样），或者每行一个 `--@app key=value`
  （老包如「矢量地图」就是每行一个）。固件与上传页都能解析。
- 字段约束：`name` ≤24 字 · `cat` 只能是 **游戏/软件/标准/运维/学习** · `desc` ≤40 字 ·
  `ver` 必须是 **1~999 的整数**。写 `工具/程序/应用` 会归一到「软件」，`系统/设置`→「标准」，`网络`→「运维」。
- `ver` 不是装饰：商店靠它判断有没有新版，**发布更新必须比商店里的当前版本大**，否则会被拒收。

## 3 骨架（四件套）

```lua
local S = { ... }                       -- 私有状态(不要用全局)

function render()  ... end              -- 只画, 不做 IO
function action(a) ... end              -- 按键: a = app.UP/DOWN/LEFT/RIGHT/OK/CONFIRM/BACK/HOME
function touch(x, y, ev) ... end        -- 可选
return { render = render, action = action, touch = touch }
```

- `action` 里处理 BACK：`return false` 交回系统，或 `app.exit()` 自己退。
- ⛔ 顶层 `return true` 会吞掉退出确认框。

## 4 界面

- 屏幕 400×300，1bpp，无背光（反射式）。左右留白相等，按键贴左右两侧，不要居中竖排。
- 弹窗尺寸固定 350×250（底栏 40，分隔线左右内缩 6px）——用 `app.dlg.*` 自动对齐，别自绘。
- 字体只用这几档：
  | 用途 | API |
  |---|---|
  | 中文 16px | `app.mix` |
  | 中文 12px | `app.zhsm` |
  | ASCII 16 / 12 | `app.texts` / `app.textsb` |
  ⛔ 中英混排分两次调用（中文串里 ASCII 是空白占位）。
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

- **版本号必须递增**。提交时若 `ver` ≤ 商店当前版本，会被直接拒收并提示当前版本。
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
