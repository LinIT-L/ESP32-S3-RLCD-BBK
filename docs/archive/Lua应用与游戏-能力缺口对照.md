# ⚠ 已归档（2026-10-05）

> 一次性对照表（"想做的 vs 现有能力"），多半已过期，属过程材料。
> （归档而非删除：结论与证据留痕。）

---
# Lua 应用/游戏 能力缺口对照

> 对照 **PICO-8**（Lua 幻想主机）、**Playdate**（Lua 掌机）、**Arduboy**、**LuatOS**（MCU Lua 运行时，
> 74 个核心库）的做法，逐条核对 LinTOS `app.*` 桥。
> 结论：**"画"和"存"够用；"输入"和"音效"是硬缺口 —— 尤其做游戏。**
> 事实来源：`components/apps/pages/page_lua.c`（`lua_setup_globals`，共 ~120 个 API）。

---

## 一、致命缺口（不补就做不出动作类游戏）

| # | 能力 | 主流做法 | LinTOS 现状 | 后果 |
|---|---|---|---|---|
| 1 | **按键"按住"状态** | PICO-8 `btn(i)` / Playdate `buttonIsPressed` / Arduboy `buttons.held()` | ❌ **只有边沿 FIFO**（`app.key()` 取一次就没了） | **按住左键不能持续移动**；只能"点一下走一格" |
| 2 | **多键同时按** | 同上，天然支持对角线 | ❌ 每次动作独立成事件，无"当前按下的集合" | 斜向移动 / 组合键 全做不了 |
| 3 | **`btnp()` 带自动重复** | PICO-8 `btnp(i)` 有初始延时+重复率 | ⚠ 只有边沿 | 菜单长列表滚动要自己数毫秒 |
| 4 | **音效** | PICO-8 4 声道 `sfx()`；Arduboy `tone(freq,dur)` 队列 | ⚠ 只有 `app.tone(freq, ms)` —— **单音、无队列、无音量** | 只能"嘟"一声；连续按键音会互相打断 |
| 5 | **调试输出** | PICO-8 `printh()`；LuatOS `log` | ❌ **没有 `app.log`** | 出错只有一个 toast；排障靠猜（本次就是靠串口插桩） |

## 二、中等缺口（能绕过，但很费劲）

| # | 能力 | 主流做法 | LinTOS 现状 |
|---|---|---|---|
| 6 | **定频帧节拍** | `_update/_draw` 固定 30/60fps | ⚠ `poll()` 每帧调，但**帧率随刷屏耗时浮动**；只有 `app.millis()` 可自己算 dt |
| 7 | **瓦片地图** | PICO-8 `map()/mget()/mset()` 一行画整屏 | ❌ 无；关卡要手搓循环 |
| 8 | **精灵表 / 大图** | `spr(n,x,y)` 按索引取 | ⚠ `app.bmp(x,y,w,h,hex)` 可用，但**上限 2048 B（≈128×128）**，且**每帧传十六进制字符串**由 Lua 解码 |
| 9 | **随机数** | `rnd()/srand()` | ⚠ 标准库 `math.random` 已开，但**无种子** → 每次开机序列相同 |
| 10 | **定时器** | Playdate `timer.new()` | ❌ 自己用 `millis` 攒 |
| 11 | **应用自带资源** | cart 内打包精灵/地图 | ⚠ `app.fs` 只能按**绝对路径**读；store 与 TF 两份路径还不一样 |
| 12 | **长按/双击事件** | 普遍支持 | ❌ 框架只给 7 个动作（上/下/左/右/确认/返回/HOME），长按要脚本自己计时 |
| 13 | **错误恢复** | 报错但进程存活 | ⚠ 任何 `poll()/render()` 报错 → **直接退出应用**（一个 typo = 应用秒退） |

## 三、已具备且够用（不用补）

`app.clear/px/fill/rect/hline/vline`（图元）· `app.texts/textm/textsb/text/textc/big/bigc/mix/mixw/zh/zh24/zhs/zhsm/zhsb`（8 套字模，含中英混排）
· `app.touch/touch_start/touch_hold`（触摸+拖拽起点）· `app.millis/delay` · `app.vibrate`
· `app.fs.*`（TF 读写列目录）· `app.nvs.*` + `app.db.*`（存档，够 `dget/dset` 用）· `app.net.*` / `app.wifi.*` / `app.hid.*`（远超一般掌机）

---

## 四、已补强（2026-10-02 全部落地，可直接用）

### 输入（游戏必需）
```lua
app.key_down(a)                        -- 该动作键当前是否**按住**  ← 持续移动/连发
app.keys_mask()                        -- 掩码, 与动作码同序:
                                       --   bit0=上 bit1=下 bit2=左 bit3=右 bit4=确认 bit5=返回
                                       --   多键同时: if m & 1 ~= 0 and m & 4 ~= 0 then ... -- 左上
app.key_repeat(a, first_ms, rate_ms)   -- 带自动重复的"按下沿"(btnP): 刚按下 true,
                                       --   按住超 first_ms 后每 rate_ms 再 true; 松手复位
                                       --   默认 first=400 rate=90; 列表滚动/连发直接用
```
> 数据源 = `input_get_held_gb_joypad()`（**低电平有效**的保持态掩码，GB/GBC 模拟器每帧在查），
> 覆盖 蓝牙手柄 / 设备物理键(KEY=A, BOOT=B) / 网页手柄 / 屏幕虚拟键 —— **没动 input 层**。
> 动作码 → GB 位：`1上=bit6 2下=bit7 3左=bit5 4右=bit4 5确认=A|Start 6返回=B`。
> ⛔ **HOME 没有"按住"**：它不在 GB 掩码里，且被 `os_core` 直接消费（=退出到桌面），应用内观测不到。
> ⚠ **触摸不算"按住"** —— 触摸走 `app.touch()` / `app.touch_hold()`。`app.key()` 保持原样（边沿 FIFO）。

### 调试
```lua
app.log(...)      -- 串口打印, 带 [APP] 前缀; 任意类型可拼。remcon/串口日志直接 grep
```
> 实现用定长 C 缓冲而不是 `luaL_Buffer` —— 后者在栈上留内部 userdata，
> 循环里再调 `luaL_tolstring` 会踩到它。⚠ 不要"优化"回 `luaL_Buffer`。

### 音效（原来会卡帧！）
```lua
app.tone(f, ms)                  -- 老 API, **阻塞** ms 毫秒 (整帧卡住). 只适合单发提示音
app.tone_async(f, ms [, amp])    -- 非阻塞单音: 喂完 PCM 就返回. 按键音/连发必须用这个
app.melody({{f,ms},{f,ms},...})  -- 非阻塞旋律, 后台任务顺序播 (≤48 音; 上一段没播完会忽略)
```
> `tone_beep()` 内部是 `vTaskDelay(ms)` —— **原来 `app.tone()` 每响一声就卡住整帧**，
> 8 个音的旋律 = 800ms 画面冻死。新增 `tone_beep_async()`（`tone_player.c`）只喂 PCM 不等待。
> `melody` 的音符缓冲是桥私有的静态数组（接口要求 notes 存活到播完），所以用
> "预估结束时刻"来拒绝重入，避免覆盖正在播的缓冲 —— 不需要给 `tone_player` 加状态。

### 报错不再"秒退应用"
> 旧行为：`poll()/render()/action()` 任何报错都 `lua_stop()` → 用户感受是"应用一打开就没了"，
> 一个 typo 白屏回列表，连错在哪都看不到。
> 新行为：**首次报错** → 串口打印 + 弹一次 toast，**应用继续跑**；
> 同一处**连续**报错到 90 次（`LUA_ERR_MAX`）才退回列表（防死循环刷屏/卡死）；任何一次成功即清零。
> ⚠ 例外：`action()` 报错时 BACK 仍会退出应用 —— action 挂了就不能把 BACK 吞掉，否则用户被卡在里面。

---

## 五、给"多应用/多游戏"的工程建议

1. ~~把上面的输入/日志做成"游戏基线 API"~~ ✅ 已完成 —— 新游戏直接用 §四 那一组，不要各自造轮子。
2. **精灵资源改成 PBM/P4 文件**（`apps/biossim/brand_icons/*.pbm` 已经这么做了，512 B / 64×64），
   比十六进制字符串省 2 倍且不用 Lua 侧解码 → 建议加 `app.bmp_file(path, x, y)`。
3. **应用目录自包含**：应用读资源时先试 `/store/apps/<cat>/` 再试 `/sdcard/apps/<cat>/`
   （固件已经有 `am_*` 的自指针重定向逻辑，可复用同一口径）。
4. **参考对象**：PICO-8 API 表（输入/图元/map/音效）＋ Playdate（定时器）＋ LuatOS（74 库，看它
   把 `io/sys/lcd/audio` 分层的方式）。⛔ 不建议照搬它们的"编辑器/多声道合成"——超出本机定位。
