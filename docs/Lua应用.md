# Lua 应用开发

> 只讲**怎么用**。API 参数/返回值唯一权威 = [API完整参考手册](API完整参考手册.md)。
> UI 交互硬约束 = [UI模块](UI模块.md)；宿主与底层 = [系统开发](系统开发.md)。路由见 [开发总索引](开发总索引.md)。

## 1 运行环境与放置

| 项 | 事实 |
|---|---|
| Lua | 5.5；`LUA_32BITS=1` ⇒ 整数 32 位、小数 **float（约 7 位有效）** |
| 状态机 | 全局单例，一次一个应用，退出即 `lua_close` |
| 执行位置 | 所有回调都在 `app_main` 主循环里，**没有独立任务** |
| 内存 | 分配器只走 PSRAM，无硬上限；耗尽报 "not enough memory" |
| 屏幕 | 400×300 1bpp（0=黑 1=白）；Lua 页为**全屏页**，顶部 24px 归应用 |
| 每帧顺序 | `os_tick → poll()` → `os_render → render()` → flush |

**放哪**：源码 `apps/lua/apps/{app,ops}/`；设备 `/sdcard/apps/{app,ops}/`；私有数据 `/sdcard/apps/data/`。
应用管家扫三个目录，**同名 `/store` 优先**；应用 ID = `lua:<文件名>`，改名等于换应用。

## 2 最小应用与生命周期

```lua
--@lapp v1
--@app name=你好 cat=软件 desc=最小示例 version=1

local S = {}
function on_enter() S.t0 = app.millis() end
function render()
  app.clear()
  app.mixc(app.W // 2, 110, "你好 LinTOS")
end
function poll() end
function action(a)
  if a == app.BACK then return false end     -- false = 交框架弹「退出程序?」
  return true
end
function on_exit() end                        -- 可选但必须写：归还资源
```

- **五个契约函数必须是全局**；写成 `local function render()` = 界面全白且无报错（`lua_check.py` 会拦）。
- 最小可运行只需 `render()`。

| 回调 | 时机 | 做什么 | ⛔ 别做 |
|---|---|---|---|
| `on_enter` | 脚本加载后一次 | 建目录、读配置、加载素材、开 GPS | 长阻塞、等网络 |
| `poll` | 每帧（弹窗/压栈时不跑） | 轮询触摸与按键电平、推进状态 | 整文件读、同步 HTTP |
| `render` | 每帧 | 只画 | IO / 网络 / NVS / 改状态 |
| `action` | 按键 | 改状态，返回真值=已消费 | 耗时操作 |
| `on_exit` | 退出（`app.exit`/BACK/退栈/HOME） | 停任务、`freeall`、写存档 | 依赖网络、长等待 |

- `action()` 抛错会**立刻退出应用**（防 BACK 被吞）；`on_exit` 只在脚本卡死在 C 调用里时不执行。
- `app.exit()` 在 `on_enter` 里调用不弹确认。

## 3 八条硬规则（能避掉大部分事故）

1. **回调里不许阻塞**：`app.delay`、`app.tone`、`http_get`、大文件整读 = 整机 UI 停摆（连返回键都没反应）。按键音用 `tone_async`。
2. **`render()` 每帧必调**；`app.redraw()` 是"这帧别跳过"，**不是**触发重绘 ⇒ 频率闸门自己写（§6）。
3. **没有回调式异步**：弹窗、网络、录音、GPS 都是"发起 → 每帧问结果"。忘轮询 = 永远收不到结果。
4. **触摸不转发**：必须在 `poll` 里调 `app.touch()`，并判 `type(x)=="number"`（没按时返回 **0 个值**，不是 nil）。
5. **中英混排一律 `app.mix/mixc`**；`app.zh` + `app.texts` 拼两次会错位（汉字族 API 遇 ASCII 画空白格）。
6. **颜色参数必须整数**（`app.BLACK/app.WHITE`），传 `true/false` 设备上直接报错；坐标可小数。
7. **32 位整数 + float**：经纬度用微度整数、`app.util.epoch()` 返回字符串、`app.util.crc32` 返回 8 位十六进制串；别用 ≥2³¹ 的字面量或位运算。
8. **图片池只有 10 槽**：满了顶掉**最早加载**那张且不报错 ⇒ 核心图集先加载，临时图后加载，用完 `free`。

## 4 界面与输入

**字体**（只用固件字模，不自绘）：带 `m/sb` 的返回下一字符 x 可接续排版；排版前用 `textw/mixw` 量宽。

| API | 字号 | ASCII |
|---|---|---|
| `texts` / `textm` / `textsb` / `text` | 8×12 / 12×18 / 8×16 / 16×24 | 正常 |
| `big` / `bigc` | 8×12 ×scale | 正常 |
| `zh` / `zh24` / `zhs` / `zhsm` / `zhsb` | 16×16 / 24×24 / 12×12 / 16×16 | **空白格** |
| `mix` / `mixc` | 16px 混排（汉字 16 / ASCII 8） | 正常 |

**触摸**（`poll` 内）：

```lua
local x, y = app.touch()          -- 没按 = 0 个值
local sx, sy = app.touch_start()  -- 按下起点（判拖动方向）
local ms = app.touch_hold()       -- 按住时长（长按用它，别自己攒）
```

判定：按下沿建状态 → 位移 >12px 记 moved → `touch_hold()>=480` 记 long → 抬起时 moved? 拖动 : 短按。

**按键**四通道：`action(a)` 事件 / `app.key()` 队列 / `key_down(a)` 电平（连续移动，覆盖手柄与虚拟键）/ `key_repeat(a)` 连发。
BACK 返回 `false`/`nil` 才交框架弹确认；HOME 应用内观测不到。关退出确认：`app.exit_confirm(false)`。

**软键盘**：`app.kbd.use("std"|"edit"|"rows"|"dialog", …)`（**只能传字符串**，没有 `STYLE_*` 常量）；`hit(x,y)` 取索引 → `press(i)`（有副作用，改 Caps/Shift/Fn）→ `char(i)`。文本缓冲自己维护。

**弹窗**（⛔ 禁止自绘）：

```lua
app.dlg.toast("已保存", 1200)   -- 非阻塞
app.dlg.confirm("删除这条?")     -- 发出即返回
local r = app.dlg.result()      -- 下一帧取：nil 未选 / -1 取消 / 0 确定 / ≥1 列表下标
```

弹窗期间 `poll` 不执行；弹窗回调里不要再开弹窗或退出页面。

**触觉**（新固件有 `app.haptic`；旧固件只有 `app.vibrate`）：

```lua
app.haptic.play("soft_click")            -- true/false, 第二值 "ok"|"cooldown"|"busy"|"rate"|"unknown"|"disabled"
app.haptic.sequence{                     -- 自定义: strength 0 = 静默段(即间隔)
  { strength = 30, duration = 15 },
  { strength = 0,  duration = 25 },
  { strength = 70, duration = 30 },
}
app.haptic.stop()                        -- 立即停
app.haptic.active()                      -- 是否在播
app.haptic.list()                        -- {name, ms, pulses, strength, priority, cooldown}
```

- 24 个具名效果：`light_tap` `soft_click` `sharp_click` `confirm` `cancel` `back` `delete` `fav` `step` `alert` `double_click`
  `triple_click` `short_short_long` `ramp_up` `ramp_down` `pulse` `success` `error` `warning`
  `long_press` `slider_tick` `boundary` `game_hit` `game_pickup`。
- 系统已接语义：列表/轮播移动一格 = `slider_tick` · **调值一格(音量/音调)= `step`** · 确认/点中 = `confirm` ·
  返回 = `back` · 删除/卸载/取消收藏 = `delete` · 收藏 = `fav` · 到头 = `boundary` ·
  **关机确认框 = `alert`(两下 90ms 重击, 优先级最高)** · 游戏 = `game_hit`。
- 可配参数：`strength` 0..100 · `duration` ms（单段 ≤5000）· 间隔（`strength=0` 段）· 段数 ≤32 · 优先级（表内固定）。
- ⛔ 不支持频率 / 波形 / 制动 —— 底层只有"占空比 + 开关"（190Hz 固定），传了也不认，别做假按钮。
- 仲裁：优先级大者打断小者，同级不打断；同名有冷却（如 `error` 600ms）；全局最小间隔 25ms，
  连点会被 `rate` 拒（返回 false，看第二值）。
- 非阻塞：内部 esp_timer 推进，回调里调用立即返回；应用退出自动停播。
- **「丰富震动」开关**（设置 → 触摸与反馈，缺省开，NVS `haptic/rich`）：
  关 ⇒ `play/sequence` 一律返回 `false, "disabled"`，系统 UI/点击/游戏回退到基础震动（旧手感）；
  开 ⇒ 系统 UI 用 `soft_click`/`slider_tick`、点击用 `confirm`、游戏用 `game_hit`（调用点不改，效果库按语义接管），
  Lua 仍可显式 `play()` 任意效果。这与「菜单震动 / 游戏震动」是两个维度：那两个管**要不要震**，这个管**震得简单还是丰富**。
- 受系统总开关约束：设置里「UI 震动 / 游戏震动」两个都关 ⇒ 底层不输出（返回值仍是 true，别拿它判断"硬件好没好"）。
- 与旧接口共存：`app.vibrate(ms[, strength])` 保留；丰富震动开启且效果库在播时，旧入口让路（不会被半路清零）；
  空闲/关闭时两者都是基础 tap，**最后写者赢**。MP3 音乐随动逐帧写占空比，不受本库仲裁（开着音乐震动时效果会被覆写）。

## 5 数据 IO

| 能力 | 要点 |
|---|---|
| `fs.read` | ≤512KB；超限/空返回 0 值 |
| `fs.read_at(p, off, len)` | 单次 ≤1MB，大文件唯一手段 |
| `fs.write/append` | 返回 bool **必须判**；无 fsync，重要数据写完读回校验 |
| `fs.listdir/exists/mkdir/size/rename/remove` | 路径必须带 `/sdcard` 或 `/store` 前缀 |
| `nvs.set_str` | 1 秒/键节流，返回值要判 |
| `db.get` | 缓冲 128B，值 ≥127B 返回 nil |
| `net.http_get` | **阻塞**、仅 http、≤4KB ⇒ 只在 `on_enter` 用 |
| `net.fetch_start(url, ms[, dst])` | 异步单槽；未联网返回 false；给 dst 则流式落盘 |
| `json.pick(raw, "arr", "k1,k2", max)` | 只取需要的字段，不建中间树 |
| `gps.start/fix` | 启动幂等；`fix()` 取快照，数值全为整数定标 |

`fetch_poll()` 返回：`"wait"` / `"ok", body, code` / `"ok", nil, code, nbytes`（流式）/ `"err", "", code, name` / `"idle"`。
**先判状态字再看 body**（流式成功时 body 是 nil）。

**短音效必须预载**：`on_enter` 里 `rec.preload(bin)` → 播放只调 `rec.play_buf()` → `on_exit` 里 `unload()`。
原因：`/store` 在 flash 上，读它要关 cache，52KB 约 188ms，期间一帧不出、甚至 `IllegalInstruction`（木鱼重启事故）。
另：WAV 必须存**立体声 L=R**（单声道会被按双声道读成 2 倍速）。

## 6 性能

`render` 第一件事是闸门（两道分开写，别合成一个条件）：

```lua
if not G.dirty then return end
if (now - G.last_draw) < 16 then return end
G.last_draw = now; G.dirty = false
```

| 调用 | 量级 | 处置 |
|---|---|---|
| `tone(f,ms)` | =ms（内部 vTaskDelay） | 换 `tone_async` |
| `net.http_get` | 1–3s | 换 `fetch_*` |
| `fs.read/listdir` | 随文件数/大小 | 分块、限条数，不放 render |
| `rec.play(path)` | ~188ms/52KB | 预载 + `play_buf` |
| `img.load` / `wiki.open` | 整文件读 / ~1.2s | 只在 `on_enter`，配 loading |

- 大文件按 `(off,size)` 索引分块读，缓存按"离视口距离"淘汰。
- 内存：PSRAM 无上限、无 GC 调优 ⇒ 大字符串/大表要克制，用完置 nil。
- 动画不必为省电砍短：交互升频由框架按帧间隔自动续。代码注释里的历史性能数字不是承诺，别当预算。
- ⛔ `app.kbd.STYLE_*`、`app.dlg.wheel` 不存在；图片池满会**静默**顶掉 slot0（某图突然不显示）；异步发起后忘轮询 ⇒ UI 永远卡在"正在…"。

## 7 错误处理与资源释放

- 报错：串口日志 + 屏底错误条（带行号）6s；累计 90 次（约 45s）退回列表；成功一次即清零。
- 所有外部返回都要判：`fs.*` / `nvs.set_*` / `fetch_poll` 状态字 / `rec.start` 返回码。
- ⛔ 不靠"没反应"当错误处理，用 `dlg.toast` 或页内文字。

| 开了什么 | `on_exit` 必须 |
|---|---|
| `img.load` | `img.free` / `freeall` |
| `rec.start` / `play` / `preload` | `stop` / `stop_play` / `unload` |
| `gps.start` / `espnow.start` / `hid.start` | `stop` |
| `wifi.probe_init` | `probe_stop` / `release` |
| `wiki.open` | `close` |
| `net.fetch_start` | 至少 `fetch_poll()` 一次取走结果 |

## 8 打包、安装与发布

**LAPP 头**（文件开头注释，固件静态解析）：

| 键 | 必需 | 说明 |
|---|---|---|
| `name` / `cat` / `desc` | 是 | 显示名 / 分类（游戏·软件·标准·运维·学习）/ 一句话 |
| `version` | 建议 | 只给人看 |
| `build` | 打包器自动 | 12 位 `YYYYMMDDHHMM`，**商店判更新的唯一依据** |
| `icon` | 否 | 64×64 1bpp，512B |
| `need` | 否 | 只认 `wifi`：启动前联网门禁，120s 超时 |

键名中间不能有空格；值里别出现" 别的键="。

```bash
python3 tools/lua_check.py [a.lua]        # 语法 + 契约 + 颜色参数（三道闸门）
python3 tools/lua_sim.py a.lua            # 本机渲染看界面（不覆盖真机特有问题）
python3 tools/pack_app.py pack …          # 生成 LAPP，自动写 build
python3 tools/sync_apps.py --check-only   # 闸门 + 打包校验，不出镜像
```

- 安装 = 复制到 `/store/apps/<目录>/`（不自动启动）；更新按 `build` 递增。
  传设备走串口：`python3 tools/hw_put.py "本地.lua" "/sdcard/apps/app/名字.lua"`，传完**退出应用重进**才加载
  （协议与判据见 [综合手册](综合手册.md) §1.1）。
- 新应用先发测试渠道（`tools/store_channel.py push/promote/drop`），自测通过再转正。

## 9 LPK 单文件资源包

**什么时候用**：C 引擎游戏、Lua 应用 + 资源 → 用；纯 Lua 无资源、单文件、>100MB → 不用。设备端**永不解包**，按偏移随机读；存档写真实 FAT。

```bash
python3 tools/pack_lpk.py --zip x_stored.zip -o out.lpk --name app   # 纯资源（只收 stored）
python3 tools/pack_lpk.py --assets ./res -o out.lpk --name app        # 从目录
python3 tools/pack_lpk.py --inspect out.lpk --verify                 # 检查 + 逐条 CRC
```

**格式**：`LPK1` 头 64B（`total_size` 必须等于真实大小、`count ≤ 8192`、`data_off` 512 对齐）；每条 32B（`off` 绝对、`size`、`crc32`、`name_off`）；无压缩；打开时全量校验（边界/对齐/重名/大小写），坏包当场拒绝。

**设备接入四条**：
1. 挂载点只能**短路径 `/lpk`**（2–15 字符、`/` 开头、不以 `/` 结尾），真实路径另存。
2. VFS **先剥挂载点**，回调收到 `/ABC.MKF` —— 按 `/lpk/...` 写会 EINVAL，编译期发现不了。
3. 读写分流：登记的可写路径（`save/`、配置、截图）走真实 FAT；其余只读，写意图 → `EROFS`；不跨类回退。
4. 大资源必须开 **4×64KB PSRAM 预读窗**（不开慢 6 倍：37.5s → 6.8s）；分配失败退化直读，**绝不回落内部 RAM**。

卸载顺序：停音频 → 停游戏任务 → `sd_remount()` → `lpk_vfs_unmount()`。

**manifest 必填**：`schema_version=1`、`app_id`（`com.lintos.<name>`，上线后永不改）、`label`、`version`、`package_type`(app|resource)、app 的 `runtime`(lua|native|engine) 与 `entry`（必须真实存在于包内）。
`permissions` 非空会触发联网门禁，纯离线应用别写；索引 `path` 主干必须等于 `app_id`（否则装完显示"未安装"）。

## 10 投稿与第三方安全

- **信任级由安装位置决定**（`/store/apps/` = 官方，其余 = 第三方），不认 LAPP 头声明。
- 第三方被移除：`os.execute/exit/tmpname`、`io.popen/tmpfile`、`loadfile/dofile/package.loadlib`、磁盘 `require`；拒绝 `app.hid.*`、`app.wiki.open`、`app.rec.start`；文件只能落 `/sdcard/apps/.data/<appid>/`；NVS/db 走 `appdb_<8hex>`；配额 **4MB / 单次 10s**。
- 路径守卫同时拒 `..` 与 `..\`（FatFs 把反斜杠也当分隔符）。
- 静态扫描只是审核辅助；**运行时沙箱才是边界**。
- 上传：网页投稿 → 待审 → 通过才发布；发布走事务 + 备份 + 原子写；回滚 = 旧内容 + **新版本号**（否则老用户收不到更新）。
