# UI 模块与交互

> UI 硬约束唯一来源。API 参数见 [API完整参考手册](API完整参考手册.md)；
> 驱动/引脚见 [硬件配置](硬件配置.md)、[系统开发](系统开发.md)；Lua 调用见 [Lua应用](Lua应用.md)。

## 1 输入方案：只有一个判据

```c
bool touch_scheme = input_scheme_touch();   /* 触摸在位 && 未连 BLE 手柄 */
```

⛔ 不要自己拼 `s_has_touch && !bt_manager_is_connected()`。

| 方案 | 默认高亮 | 方向键 |
|---|---|---|
| 触屏 | **一行都不画** | 按过方向键后才出现光标 |
| 手柄 | 画出当前项 | 移动光标 |

```c
bool sel = !input_scheme_touch() && (idx == s_cursor);                    /* 标准 */
bool sel = (!input_scheme_touch() || s_key_cursor) && (idx == s_cursor);  /* 触屏也要支持按键 */
```

- 触屏点完**不要把高亮留在那一行**（会被读成"已选中"）；无光标时首次按确认：**先立光标、不执行动作**。
- 例外（属"事实标记"不是选中，可保留）：当前值/当前项（【】、当前章）、正在执行状态、键盘焦点。

## 2 弹窗结构（四段，缺一不合格）

```
2px 外框
├ 标题（居中；下方 1px 线）
├ 内容区（滚动归 ui_scroll）
├ 1px 分割线   ← 必须在"返回"行正上方
└ 底部"返回"行（居中；左侧留给一次性提示）
```

- 分割线画在 `foot_y`，从左内框到右内框（`bx+2 … bx+bw-3`）。
- ⛔ 分割线必须在**裁剪区内**：先 `clip_set(..., foot_y, ..., foot_y1-1)`；写成 `foot_y+1` 会把线整条丢掉（阅读器踩过）。
- 底栏高度固定（16px 字时 44px），不随字号变。
- **几何只有一份**：`dlg_layout()` / `bm_layout()` 产出，绘制、命中、滚动三处共用。⛔ 不要各算一遍（曾出现"画一处点另一处"）。

## 3 触摸命中：任何位置都要消费

| 位置 | 行为 |
|---|---|
| 框外 / 底栏 | 退一级（子菜单→主菜单→关闭）|
| 标题区 | 消费但不动作（避免误触关闭）|
| 内容区 | `k = (y - (list_y0 - px)) / row_h`，与绘制同一公式；`idx = scroll + k` |
| 底部余数带 | **夹到最后一行**，不得 `return false` |

⛔ `return false` = 不消费、不震动、菜单也不关 ⇒ 用户感受"点一下没反应"。可视高不能被行高整除时，那条余数带是最大死区。

## 4 长按 → 弹窗（到点即弹，不等松手）

- 阈值 **1000ms**，按满当帧就弹；⛔ 不许"记下来等松手"。
- 松手由 OS 处理（`s_swallow_tap` 吞掉本次手势后续点击，抬手后 200ms 解除）；应用**不要**自己写延迟。
- 应用只做两件：弹窗打开时复位本页手势状态机；抬手前不再起新手势。
- 结果语义别混：`confirm` = 0 确定 / 1 取消 / -1 返回；`list` = **0 基下标** / -1 返回。

## 5 Lua 弹窗契约（⛔ 禁止自绘）

| 用途 | 写法 |
|---|---|
| 退出确认 | `action(a)` 里 BACK **`return false`（不消费）**，框架弹系统确认框 |
| 单意图确认 | `app.dlg.confirm(msg)` |
| 一次性提示 | `app.dlg.toast(msg[, ms])` |
| 多选项 | `app.dlg.list(title, items, sel)`（350×250）|

1. `app.dlg.result()` 是**取走式**（读到即清零）：在 `poll()` 每帧读一次，`nil` 未选 / `-1` 取消 / `0` 确定 / `≥1` 列表下标。
2. **子页返回要消费**（`return true`），主界面返回不消费（交给框架）。
3. `app.exit_confirm(false)` 只给无进度可丢的纯浏览页。
4. 自绘禁止的硬理由：① 和系统框处处不同；② 1bpp 屏无半透明遮罩，白框盖不住黑色位图，只能整屏铺白 ⇒ 背景全丢、像换了页面。
5. 弹窗只传文案，坐标/字体/按钮全由框架定。

## 6 状态栏：只有 OS 画，应用只声明

| 角色 | 做 | 不做 |
|---|---|---|
| OS | 每帧判断画不画（`os_sb_should_show`），可见就整条落笔 | 不按"内容没变"跳过 |
| C 页 | `os_statusbar_show(true)`；内容从 `os_statusbar_height()` 之下开始 | 不自画时间/电量；不靠 `fullscreen` 藏 |
| Lua 页 | `app.statusbar(true/false)` 声明；起点用 `app.statusbar_h()` | 不自拼时间/电量；不写死 20/24 |

- 声明是**三态事件**（AUTO/SHOW/HIDE）：⛔ 不要在 render 里每帧下发布尔——某帧取到 false 会永久写 HIDE（"录音机状态栏永久消失"的根因）。
- 声明黏在本页，换页回 AUTO（`os_statusbar_auto()` 复位，不是 `show(false)`）。
- 内容由 OS 定时置脏刷新（分钟跳变、电量 60s、连接变化），应用不必为刷状态栏重绘。
- 引擎页走 `svc_statusbar_draw_game()`，同一个组件不是第二套。

## 7 页面版式（不画标题 · 苹果风 · 谷线导航条）

- **应用内不显示标题栏**：状态栏之下直接内容（顶边距 4–8px）。当前页由底部提示或选中态表达。
- **风格基线**：一屏一个主角（最大字号 + 最大留白）；留白 + 1px 细线分区，不堆边框（1bpp 多层框会整屏发黑）；动作按钮用圆/胶囊，文字按钮不加框；文案克制（动词，不用"点击此处"）；同类信息右对齐成列。
- **谷线导航条（Valley Nav）**：左右两条 45° 斜坡夹一条谷线，斜坡内各放一个选项，中间缺口放提示。几何只算一次，**绘制与命中共用同一套边界函数**；中间提示不响应点击；需要第三个动作时改滑动切页或放进弹窗，别往斜坡塞第三个选项。参考 `page_netdect.c: nd_draw_nav`。

## 8 列表滚动：只用 `ui_scroll`

`components/ui/ui_common/ui_scroll.c/h`。⛔ 任何页面不得自己算 `off` / 判 `dy` / 写吸附 / 调惯性系数。

```c
ui_scroll_config(&sc, UI_SCROLL_LIST, 0);                     /* 行吸附（设置/文件/应用列表）*/
ui_scroll_config(&sc, UI_SCROLL_PIXEL, 0);                    /* 像素自由（正文/终端/大图）*/
ui_scroll_config(&sc, UI_SCROLL_PAGE, 0);                     /* 整页翻（电子书/漫画）*/
ui_scroll_config(&sc, UI_SCROLL_LIST, UI_SCROLL_F_NO_FLING);  /* 开关可叠加 */

static ui_scroll_t s_sc;                                      /* ① 页面私有 */
ui_scroll_init(&s_sc, ROW_H, VIEW_H, count * ROW_H);          /* ② 进页/内容变化 */
if (ui_scroll_touch(&s_sc, ty, down)) ctx->needs_redraw = true;   /* ③ poll 每帧喂触摸 */
if (ui_scroll_poll(&s_sc))            ctx->needs_redraw = true;   /* ④ 推进惯性 */
if (ui_scroll_dragging(&s_sc) || ui_scroll_busy(&s_sc)) return;   /* ⑤ 拖动/未收敛：不点选、不重排 */
int y0 = LIST_Y - ui_scroll_offset(&s_sc);                        /* ⑥ 渲染起点 */
ui_scroll_drawbar(&s_sc, lcd, 396, LIST_Y, LIST_Y + VIEW_H);      /* ⑦ 统一滚动条 */
```

- 开关：`F_NO_FLING`（松手即停）/ `F_NO_BOUNCE`（硬边界）/ `F_HORIZ`（横向，配 `touch_xy`）/ `F_LOOP`（循环）。
- 手感（定死，不要各页调）：拖动阈值 12px（以下算点击）；跟手 1:1、越界最多半屏；惯性每帧 ×0.86、<0.4px 停、限速 ±60px；回弹每帧 ×0.8、<2px 归零；`row_px>1` 停稳吸整行。
- 其余 API：`set_content` / `first_row` / `ensure_visible` / `to_top` / `cancel`（切类/退页用）。
- ⛔ 禁止：自写 `drag_last_y`/`fling`/吸附；滚动期间重排整页；在滚动条上画实线；自调系数。

## 9 字体系统

| 层 | 格式 | 位置 | 谁用 |
|---|---|---|---|
| 系统 UI 字库 | `ZH1FNT01` | appdata `+0x400000`(24px) / `+0x480000`(16px)，mmap XIP | 全部 UI |
| 阅读器字体 | **LBF v1** | TF `/sdcard/fonts/*.lbf`，整块读入 PSRAM | 电子书正文/菜单 |
| 阅读器兜底 | 复用 UI 字库 | — | 无 LBF 时 |

- **系统 UI 字体固定**：设置页不提供字体切换。2026-11-24 起备选字库槽（`+0x5C0000`/`+0x600000`）与 `font_switch.c/.h` 已整体删除，只剩默认一套（16px 文泉驿 / 24px HZK24S）；启动不再恢复 `uif16/uif24`。应用中心分发的是阅读器 LBF，两者无关。
- 设备端**不解析** OTF/TTF/BDF/PCF/HZK，全部主机侧 `tools/fontkit.py` 转换；OTF/TTF 只作输入。
- **字号只允许"原生"或"严格 2 倍"**（最近邻，无插值）：8/16 · 10/20 · 12/24 · 14/28 · 16/32 · 24/48；非法值按就近收敛（`snap_px`）。字号表唯一来源 `font_book.c: sizes_for_native()`。
- **字体作用域**：正文跟随阅读字体；**所有弹窗/菜单/设置用系统 16px**。入口只有 `read_draw_text()` / `ui_draw_text()`，⛔ 不许临时改全局字号。
- LBF 加载：≤2MB 且 PSRAM 够 → **整块预加载**（取字零 IO），否则 64 槽 LRU；预加载后立刻 `fclose`；保留 2 槽，离开书库 `release_all()`。

**UI 字库替换（只换字形，不动接口）**

```bash
python3 tools/fontkit.py ui16 <源> --size 16 --cell 16 \
    --index-from components/ui/fonts/font_zh16.bin \
    --out components/ui/fonts/font_zh16.bin --report r.json
```

- **字符表逐字节沿用旧库**（数量/Unicode/顺序全同）；缺字沿用旧字形并写进报告，⛔ 不静默减字。
- **满格字模直通**：源字形已是 `cell×cell` 且 `xoff=yoff=0`（HZK/点阵 ROM 形态）原样搬运；做基线重排会砍掉顶行/底行。紧致字形才做基线自适应（16px 基线为第 14 行）。
- 输出先写 `.tmp` 再原子替换；校验用 `tools/font_selftest.py`。

**授权边界（发布必看）**：16px 文泉驿点阵宋（GPL-2.0 + 字体嵌入例外）可发布，需随附许可与署名；**24px HZK24S 字模权利不明、方正像素 24 为商业授权未取得 —— 都不得进发布包**，发布前必须换成有明确可分发授权的 24×24 点阵。"工具 GPL ≠ 字模授权"。

## 10 显示（ST7305 400×300 1bpp）

**帧缓冲不是逐行 1bpp**，而是面板原生"2 列 × 4 行 / 字节"：

```
字节 = (x >> 1) * 75 + (y >> 2)      /* 75 = 300/4 */
位号 = 7 - ((y & 3) << 1 | (x & 1))
```

- 改一个像素必须**掩码写**，整字节赋值会连带改掉邻行邻列；一律走 `st7305_draw_pixel` / `write_masked_pixel`。
- 颜色常量 **`BLACK = 0`、`WHITE = 1`**（0 才是黑，容易记反）。整帧 15,000 B。

**灰阶**：源 4 级（GBC palette / NES 2bit）→ 每个源像素映射 2×2 物理点，黑点数由 `vid_tune` 曲线 dots(0..4) 决定；**2×2 最多 5 级**，这是硬上限。

- 两个引擎的图案必须同源：`board_shim_pat_get()`（NES 侧用 `s_rowbits[4][2][16][2]` 打包表）。**改一处必须同时改另一处**，否则 NES/GBC 灰阶不一致。
- 算法：`CLUSTER_2X2`（默认，位置无关）/ `BAYER_4X4`（实验，按绝对坐标）；串口 `gray 0|1` 切换后强制整屏重画。
- ⛔ `board_shim/gray_dither.c` 是**死代码**（未编译、无调用），不要照它改。

**局部刷新：结论是不做。** 整帧 SPI 仅 3–4.5ms、面板自刷新 16–24Hz；静止画面已由整帧校验和跳过传输；脏矩形依赖 CASET 换算、旋转方向无法验证，风险大于收益。

## 11 提交前自检

- 触屏未连手柄：进页面/开弹窗**没有反白行**；连手柄：有高亮且方向键可移。
- 弹窗"返回"上方有分割线，从左框画到右框（在裁剪区内）；点框外退一级，点标题区不误关，底部余数带有反馈。
- 几何只算一次（绘制/命中/滚动共用）；滚动只调 `ui_scroll_config` 且 `busy()` 时提前 return。
- 触摸按下反馈用 `input_touch_now()`；页面内无标题栏。
