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
