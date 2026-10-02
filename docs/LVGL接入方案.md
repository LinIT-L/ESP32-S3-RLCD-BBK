# LVGL 接入方案与裁剪记录（2026-09-22）

## 结论速览

| 项 | 值 |
|---|---|
| LVGL 版本 | **9.6.0**（ESP Component Registry `lvgl/lvgl`） |
| 移植层 | `components/system/lvgl_port/` |
| 自检页 | `components/apps/pages/page_lvgl.c`（应用管家 → 应用 → LVGL演示） |
| **裁剪后 FLASH 占用** | **146.3 KB**（141.9K 代码 + 4.5K 数据）+ 移植层 0.8 KB |
| 未裁剪时 | 351 KB → **裁剪省下 201 KB** |
| `LinTOS.bin` | 4,389,744 B（接入前 4,235,728 B，净增 **150.4 KB**） |
| `factory` 余量 | 5 MB 中空闲 **833 KB（16%）** |
| RAM 增量 | 内部 RAM ≈ **0**（缓冲 2×2000 B 在 PSRAM；LVGL 对象走 libc malloc） |

---

## 一、为什么放得下了

原来 `factory` 只剩 247.6 KB，LVGL 进不来。分区调整为
**factory 5 MB / appdata 6.625 MB / apps 4 MB** 后（见 `partitions.csv`），
再叠加下面这份裁剪，占用压到 146 KB。

---

## 二、裁剪清单（全部在 `sdkconfig.defaults`）

### 1. 显示与核心
```
CONFIG_LV_COLOR_DEPTH_1=y            # 1bpp 单色, 与 ST7305 一致
CONFIG_LV_DEF_REFR_PERIOD=33
```

### 2. 主题与字体
```
CONFIG_LV_USE_THEME_MONO=y           # 单色屏专用主题 (彩色主题的渐变/阴影/抗锯齿全部不编)
CONFIG_LV_USE_THEME_DEFAULT=n
CONFIG_LV_USE_THEME_SIMPLE=n
CONFIG_LV_FONT_DEFAULT_UNSCII_16=y   # 官方给单色屏推荐的无衬线等宽位图字体
```
> 中文不进 LVGL 内置字体 —— 用自有字体驱动接固件字库（appdata mmap），见第四节。

### 3. 渲染器只留单色路径 ★省得最多
```
CONFIG_LV_DRAW_SW_SUPPORT_I1=y       # 屏幕本身
CONFIG_LV_DRAW_SW_SUPPORT_A8=y       # alpha 掩码(混合/遮罩必需)
CONFIG_LV_DRAW_SW_SUPPORT_L8=y       # 灰度/抗锯齿
# 其余色深全部关掉: RGB565 / RGB565A8 / RGB565_SWAPPED / RGB888 / XRGB8888 /
#                   ARGB8888 / ARGB8888_PREMULTIPLIED / AL88
CONFIG_LV_USE_FLOAT=n                # 纯整数路径
CONFIG_LV_USE_MATRIX=n
CONFIG_LV_USE_VECTOR_GRAPHIC=n
CONFIG_LV_DRAW_SW_COMPLEX=n
```

### 4. 只留 10 个控件
`LABEL / BUTTON / BAR / ARC / IMAGE / LINE / LIST / SWITCH / SLIDER / CHECKBOX`

关掉：CHART、TABLE、TEXTAREA、CANVAS、TABVIEW、ROLLER、KEYBOARD、CALENDAR、MSGBOX、WIN、
SPAN、QRCODE、ANIMIMG、DROPDOWN、SPINNER、SCALE、MENU、ARCLABEL、IMGFONT、BUTTONMATRIX、
BARCODE、FILE_EXPLORER、3DTEXTURE、FFMPEG、BMP

### 5. 关掉自带示例与演示
```
CONFIG_LV_BUILD_EXAMPLES=n
CONFIG_LV_BUILD_DEMOS=n
```

### 6. 关掉调试/监控/文件系统
LOG / SYSMON / PERF_MONITOR / MEM_MONITOR / MONKEY / PROFILER / SNAPSHOT / TEST /
FONT_MANAGER / OBSERVER；`LV_USE_FS_*` 全关。

---

## 三、⚠️ 踩过的三个坑（改配置时必看）

| # | 现象 | 真因 | 处置 |
|---|---|---|---|
| 1 | 改了 `sdkconfig.defaults` 但**尺寸一点没变** | `sdkconfig` 已存在时，defaults **不会覆盖已存在的项** | 删 `sdkconfig` 让它重新生成（该文件已在 `.gitignore`，是生成物） |
| 2 | `lv_spinbox: lv_textarea is required` | LVGL **v9.6 的 spinbox 无条件编译**（无独立开关），却 `#include` 且依赖 textarea | 关 `LV_USE_SPINBOX=n`（我们本来不用） |
| 3 | examples 报 `lv_subject_*` 未声明 | `LV_BUILD_EXAMPLES/DEMOS` **默认 y**，示例会引用已关掉的控件与 OBSERVER | 显式 `=n` |

---

## 四、移植层（`components/system/lvgl_port/`）

### 归层
放在 **system（rank 3）**，与 `os_hid.c` / `os_net.c` 同类 —— 它是**系统级硬件胶水**，
需要 `input`(system) 与 `st7305`(drivers)。放 `ui`(rank 1) 会构成 `ui → system` **反向依赖**，
会被 `tools/check_deps.py` 拦下（实测报错 `lvgl_port[ui] -> input[system]`）。

### 关键设计
1. **整行宽缓冲**：缓冲宽 = 屏宽 400 → 每行 50 字节，与 `st7305_blit_1bit()` 的
   `bytes_per_row = (w+7)/8` 完全一致，无需处理 stride 差异。
   带高 40 行 → 单缓冲 2000 B，双缓冲 4 KB，全在 PSRAM。
2. **刷屏时机**：等 `lv_display_flush_is_last()` 为真（所有带画完）才 `st7305_flush()`，
   否则每条带都推一次 SPI，撕屏且慢。
3. **位极性**（唯一需要上机确认的常量 `LP_BIT1_WHITE`）：
   驱动提供两个只写"命中像素"的批量接口 —— `st7305_blit_1bit()` 位=1 写**黑**、
   `st7305_blit_1bit_white()` 位=1 写**白**，两者都不碰未命中像素。
   所以一个区域要**先铺底色再盖亮色**，共两次 blit：
   `LP_BIT1_WHITE=1` → 先 `blit_1bit`(全 0xFF 铺黑) 再 `blit_1bit_white`(LVGL 缓冲)。
   **若上机发现整屏反色，把 `LP_BIT1_WHITE` 取反即可。**
4. **心跳**：`esp_timer` 每 2 ms 喂 `lv_tick_inc`；独立任务跑 `lv_timer_handler`。
5. **输入**：触摸 pointer indev（接 `input_get_touch_pos`）。

### 屏幕归属
自检页是 `fullscreen` 模块，**打开期间屏幕归 LVGL 所有**，该页 `render()` 故意不画。
另在 **LVGL 任务上下文**里挂一个 500 ms `lv_timer` 周期性 invalidate 整屏
（**不能**在 OS 的 render/poll 里直接调 `lv_obj_*` —— 跨任务访问 LVGL 对象不是线程安全的）。

---

## 五、下一步（待办）

- [ ] **上机确认位极性**：进「应用管家 → 应用 → LVGL演示」，若整屏反色则翻转 `LP_BIT1_WHITE`
- [ ] **中文字体驱动**：写一个 `lv_font_t`，`get_glyph_dsc/get_glyph_bitmap` 回调走
      `font_zh16_find_utf8()` + `font_zh16_get_bitmap_by_index()`（appdata mmap，**0 flash**），
      再用 `LV_USE_CUSTOM_FONT_DEFAULT` + `LV_FONT_CUSTOM_INCLUDE` 把它设成默认字体
- [ ] **Lua 侧 `lv_obj` 绑定**：把 `lv_obj_*` 子集暴露成 `app.lv.*`，让 Lua 应用在运行时建界面
- [ ] **设计器流程**（SquareLine Studio / EEZ Studio）：导出 C 代码用于**固定界面**；
      导出坐标用于**可后装应用的排版参考**（见下）

---

## 六、关于「UI 在电脑上制作」

分两条路，都可以走，不冲突：

| 路线 | 做什么 | 产出 | 适合 |
|---|---|---|---|
| **设计器 + 导出 C** | SquareLine Studio / EEZ Studio 拖控件 | `lv_obj_create()` 一堆 C 代码，**编译期**进 factory | 主菜单/设置/状态栏这类**固定界面** |
| **运行时 API** | 固件把 `lv_obj_*` 暴露给 Lua | 脚本在设备上现场建界面 | **可后装、可换的应用**（Lua 单文件） |

本机屏幕是 400×300 **单色**，设计器默认按彩色/触摸布局出图，导出后仍需手调；
且设计器用的是 TTF 转位图字体，与我们的 16/24px 字库不是一套 ——
**所以设计器主要用来"定版式"（坐标/尺寸/间距），字体仍走固件字库。**

---

## 七、与存量 UI 体系的合并分析（2026-09-22 linit 提问）

### 7.1 项目自己的规范（`docs/开发指南.md §3`）—— 这是"历史窗口要求"的原文

> **弹窗（唯一源头 os_dialog，不自画）**，四类：

| 类型 | API | 行为 | 用途 |
|---|---|---|---|
| 提示 | `os_dialog_toast` / `toast_ms` | 居中提示，默认 1s 自动关，任意按键/触摸立即关 | 成功/失败/IP/已输入/生效 |
| 确认 | `os_dialog_confirm_ex` | 确定/取消两键，等用户选择，可配 timeout 超时自动完成 | 删除/格式化/恢复/关机 |
| 列表 | `os_dialog_list` / `os_pane` | 单选+底部返回行，触屏点哪选哪+手柄方向键 | 选择/浏览（文件/WiFi/蓝牙/PIN） |
| 复杂交互 | `os_dlg_stack` custom（`on_render/on_touch/on_key/on_poll`）| 自定义渲染/触摸/轮询 | 滚轮/连接中/抽屉/按键映射 |

> 并明确：**提示 vs 确认严禁混用**；`cb(ctx, result, ud)`，`result=-1` = BACK 取消。

### 7.2 逐类对照 LVGL

| 规范四类 | 存量实现 | LVGL 对应物 | 语义一致性 | 代码可复用? |
|---|---|---|---|---|
| ① 提示 toast | `os_dialog_toast_ms` | `lv_label` + `lv_anim` + `lv_timer` | ✅ 一致（LVGL 还能加淡入/位移）| ❌ 渲染模型不同 |
| ② 确认 confirm | `os_dialog_confirm_ex` | `lv_msgbox`(+`lv_timer` 超时) 或 `lv_obj`+2×`lv_button` | ✅ 一致；LVGL 内置 `LV_OBJ_FLAG_MODAL` 与焦点/手柄导航 | ❌ |
| ③ 列表 | `os_dialog_list` / `os_pane` | `lv_list`(自带滚动+点选+焦点)、`lv_roller`、`lv_dropdown` | ✅ 一致；LVGL 滚动是**像素级平滑**，存量 `os_pane` 要自己维护 `scroll_px` | ❌ |
| ④ 复杂交互 | `os_dlg_stack` custom 四回调 | 直接建 `lv_obj` 树 + `lv_event` + `lv_anim` | ⚠ **模型不同**：存量是"你自己实现四个回调"，LVGL 是"声明对象+事件" | ❌（LVGL 更适合这类）|

**外观侧**：存量靠 `draw_dialog_frame`（弹窗外框）/`draw_rect_outline_side_round`（侧圆角）/
`draw_slot_marker`（选中块）+ `os_theme`(1.3 KB 配色)。
LVGL 侧是 `lv_theme_mono` + `lv_obj_set_style_*`。
→ **能对齐，但要重设样式**（LVGL mono 主题的默认样式与我们手绘的框线/反白块不同）。
→ **字体仍走固件字库**（appdata mmap，0 flash），不用 LVGL 的位图字体。

### 7.3 结论：**功能语义一致，实现体系不兼容**

- 存量是 **立即模式手绘**：每帧 `render()` 从零画，状态存页面私有 `s_*`，触摸靠**坐标命中判定**。
- LVGL 是 **保留模式对象树**：框架负责失效重绘/命中测试/动画/滚动/焦点。
- → **LVGL 渲染不了 `os_dialog_*`**（那是直写 fb 的代码）；**`os_dialog_*` 也渲染不了 LVGL 对象**。
- → 所以"合并"只能发生在 **API 门面层**，不能发生在引擎层。

### 7.4 ⚠️ 硬冲突：framebuffer 归属（已用源码验证）

- `os_dialog_draw_overlay()` 由 `os_render()` 在**所有模块渲染之后、flush 之前**直写 fb
  （`os_dialog.c` 原注释："确保提示弹窗恒显示在最上 (不受全屏页如赞助二维码 st7305_clear 覆盖)"）。
- LVGL 的 flush 回调也直写**同一块** ST7305 fb，并自己 `st7305_flush()`。
- **两者同时活跃 = 互相擦。** → 必须加显式"屏幕归属"仲裁（`lvgl_port` 已有 `lvgl_port_pause()` 可复用）。

### 7.5 三种合并方案

| 方案 | 做法 | 成本 | 破坏"唯一源头"? | 评价 |
|---|---|---|---|---|
| **① 门面合并（推荐）** | `os_dialog_*` **签名一个字不改**，内部加 **双后端**：A=现有手绘、B=LVGL 对象；按**当前屏幕归属**选后端 | 中（后端 B 约 200–400 行）| 否 ✅ 规范原样成立 | ★ 47 个页面零改动，LVGL 页面也走同一套弹窗语义 |
| ② LVGL 吸收一切 | 47 个页面 + `os_pane` + `kbd_lib` + `svc_statusbar` 全改 `lv_obj` | 极高 | 否 | 不建议：每页都要重测交互/坐标/字体，且要重新对齐 0.6 版观感 |
| ③ 不合并，划界 | 规范改成"手绘页走 os_dialog；LVGL 页走 lv_msgbox" | 0 | **是**（变成两条源头）| 应急可用，长期会分叉 |

### 7.6 落地顺序（按方案 ①）

1. **屏幕归属仲裁**（~50 行）：`lvgl_port` 加 owner 标志；LVGL 独占时 `os_render` 不画、toast 改走 LVGL 后端。
   **这是所有后续工作的前提**（现在 demo 页是靠"约定 render() 里别画"，太脆）。
2. **toast 的 LVGL 后端**（最简单：1s 自动关 + 任意输入关）→ 验证仲裁生效。
3. **confirm**（两键 + timeout；映射 `no_footer/small/icon/fix_w/timeout_ms`）。
4. **list**（映射 `items[60][30]/count/sel/footer/auto_h/row_x/scroll_px`，手柄方向键导航）。
5. **`os_pane` 最后做**（它承载像素级滚动 + 双栏 + 右键回调，语义最多）。

### 7.7 明确"不合并"的部分

| 模块 | 原因 |
|---|---|
| **`virtual_keys`** | 物理键→虚拟键映射，LVGL 没有对应物 |
| **`kbd_lib` 软键盘** | LVGL 的 `lv_keyboard` **依赖 `lv_textarea`**（我裁剪时两者都关了，要用得开回来 +flash）；而 `kbd_lib` 是手绘独立实现、不依赖 textarea，已在 PIN/WiFi 密码等多处跑通 → **继续用 `kbd_lib`** |
| **`ui_common` 立即模式原语** | 47 个手绘页面在用，迁走纯风险 |
| **存量 6 个 Lua 应用**（calc/morse/pomodoro/diag/logdiag/winassist）| 目标是"和 C 版一模一样"，换 LVGL 反而破坏一致性 → **保持手绘** |

### 7.8 建议的规范增量（给 `开发指南.md §3` 补两句）

> - 手绘页面：弹窗唯一源头仍是 `os_dialog`（后端 A）。
> - **LVGL 页面：弹窗走 `os_dialog`（后端 B），不允许直接调 `lv_msgbox`** —— 保持"唯一源头"。
> - 同一屏幕同一时刻**只允许一套渲染体系拥有 framebuffer**（由 `lvgl_port` owner 标志仲裁）。
