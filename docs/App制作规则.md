# LinTOS App 制作规则

> 面向"给 LinTOS 新增一个 App"的完整规范：需要提供哪些信息、图标/命名/介绍/权限/UI 怎么做、如何注册发布。
> 本文件由 **docs/开发指南.md** 的「应用发布规则」+ `os_app_t` 注册结构 + UI/图标约定 提炼独立出来，作为 App 的唯一标准。
> **单文件 Lua 程序**（放 TF 卡免重编固件运行）走另一套植入规则，见 [docs/Lua单文件程序规则.md](Lua单文件程序规则.md)。

---

## 1. 一个 App 需要提供哪些信息

| 字段 | 必填 | 说明 |
|---|---|---|
| **id** | 必填 | 全局唯一标识。编译期应用用英文短名（如 `settings`）；商店下载的应用前缀 `wasm:`（如 `wasm:snake`）。 |
| **名称 label** | 必填 | 显示名，UTF-8。主菜单/应用管家/商店显示。建议 ≤ 8 个汉字（图标下放得下）。 |
| **图标 icon** | 必填 | 见第 2 节。进入 `icon_idx` 图标库索引。 |
| **应用介绍 desc** | 建议 | 商店列表里"中文名下方"的一行简介。 |
| **归属分类 cat** | 必填 | 商店/应用管家子分类：`游戏 / 程序 / 软件 / 系统 / 运维 / 学习 / 网络 / 手册`。NULL=不归类（隐藏工具类）。 |
| **入口页 page** | 必填 | 启动后进入的 `os_page_t` 页面，由 `os_module_t` 提供 render/action/touch。 |
| **体积 size** | 可选 | 商店展示的包体大小。 |
| **权限/系统能力** | 按需 | 见第 3 节，用到哪个加哪个。 |

---

## 2. 图标

### 2.1 规范

- **尺寸：64 × 64 px，1bpp 黑白**（单色点阵，**MSB 在左**）。  
- 主菜单图标默认 **64×64**；被选中（居中）临时放大到 **100×100**（内部三线性插值，不依赖图像源）。  
- 黑白屏反色由 `inverted` 参数控制，不另存反色图。  
- 绘制统一走 `draw_main_icon_stretched()` / `ui_main_icon_bitmap()` —— **不允许在 page 里直接 `st7305_blit_1bit` 一个图标数组**，入口必须经过 `ui_common`。

### 2.2 图标文件放置位置（**唯一来源** = 桌面 PNG，固件里只放自动生成的 C 数组）

| 用途 | 桌面 PNG 路径 | 生成脚本 | 落盘到固件 | 运行时数组 |
|---|---|---|---|---|
| 主菜单 / 应用管家 33 个图标 | `/Users/linit/Desktop/完美图标/` | `tools/image_convert/gen_main_icons_64.py` | `components/ui/ui_common/icons_main.inc` + `icons_main_enum.h` | `main_icons[ICON_*]` |
| 应用管家侧栏 4 个分类图标 | `/Users/linit/Desktop/完美图标/应用管理/` | `tools/image_convert/gen_cat_icons_64.py` | `components/ui/ui_common/cat_icons.inc` | `cat_icons[cat_id_t]` |
| 状态栏小图标 | `/Users/linit/Desktop/完美图标/状态栏/` | `tools/image_convert/gen_status_icons_12.py` | `components/ui/ui_common/status_icons.inc` | `status_icons[]` |
| 诊断/工具类零碎图标 | `/Users/linit/Desktop/完美图标/` 散落 | `tools/image_convert/gen_diag_icons.py` 等 | 各自 page 内部 `.h` | 各自 page 局部 |
| 散装 / 成品库补充 | `/Users/linit/Desktop/其他图标/` | 同上（MAP 路径写这里） | 同上 | 同上 |

> ⚠ **"完美图标" 与 "其他图标" 是两套独立根目录**。`gen_main_icons_64.py` 里的 `SRC = /Users/linit/Desktop`，MAP 项里相对路径以 `完美图标/...` 或 `其他图标/...` 开头；不要写到项目 `tools/` 或 `assets/` 下 —— 那是"工作目录约定"。

### 2.3 制作流程（新增 / 修改一个图标）

1. **准备 PNG**：64×64（或更高自由比例），灰度或彩色均可；脚本会灰度化 + 阈值化（`<128` 视为黑像素 = bit=1）。  
   - 建议**纯黑底白线/白字**（黑底图），这样 1bpp 反相后是白底黑字，符合 1bpp 屏阅读习惯。  
   - 透明背景：脚本不处理透明，存盘前**填充白色背景**再交给脚本，否则会被 PIL 当白色、整张图标变全黑/全白。  
2. **放入桌面文件夹**：按用途放到 2.2 表格里对应的目录（如新主菜单图标 → `完美图标/应用管家/应用/XXX.png`）。  
3. **改 MAP**：打开 `tools/image_convert/gen_main_icons_64.py`，在 `MAP` 列表**追加**一行：  
   ```python
   ("ICON_NEWAPP",  "完美图标/应用管家/应用/XXX.png"),   # N 注释名
   ```  
   **顺序即下标**（也即 `ICON_*` 枚举值）；**新图标永远追加在末尾**，不要插队改已有值 —— 已注册的应用 `.icon_idx = ICON_OLD` 是数字硬绑的，插队会让全部已注册图标错位。  
4. **跑脚本**：  
   ```bash
   python3 tools/image_convert/gen_main_icons_64.py
   ```  
   自动重写 `components/ui/ui_common/icons_main.inc` + `icons_main_enum.h`。  
5. **注册到应用表**：`components/apps/apps_registry.c` 的 `s_builtin_apps[]` 写 `.icon_idx = ICON_NEWAPP`（**用枚举名**，不要再写魔法数字）。  
6. **编译**：脚本只动两个文件，下一次 `idf.py build` 自动重编 `icons_main.inc` 所在组件，无需 `fullclean`。  
7. **预览**：`tools/icons_preview_v2.png`（脚本生成的缩略图组）能离线对比当前 33 个图标的视觉效果 —— 调色/线条粗细先在 PNG 阶段反复改，预览满意再烧固件。

### 2.4 删除一个图标（节省 IRAM/Flash）

1. **查引用**：`grep -rn "ICON_XXX" components/` 看哪些页面在用。  
2. **改应用表**：被引用的页面改成别的图标（或新图标），没引用的直接置 `.icon_idx = -1`。  
3. **删 MAP 行** + **删 `components/apps/pages/` 里仅此图标用的辅助头**（如 `mp3_icons_auto.h`）。  
4. **跑脚本 + 编译**：`icons_main.inc` 自动瘦身。  
5. **基准**：每减一个图标省 **64×64/8 = 512 字节** Flash（1bpp 位图）。33→27 减 6×512 = **3KB Flash**；同时省 IRAM 里的 `main_icons[]` 指针表 6×4 = **24 字节**。

### 2.5 关键文件与产物对照

| 文件 | 内容 | 谁生成 | 谁 `#include` |
|---|---|---|---|
| `components/ui/ui_common/icons_main_enum.h` | `ICON_*` 枚举 + 尺寸宏 (`MAIN_ICON_W/H/BYTES/COUNT`) | `gen_main_icons_64.py`（自动） | 全局（任何要写 `.icon_idx` 的地方） |
| `components/ui/ui_common/icons_main.inc` | 33 个图标位图数组 + `main_icons[]` 指针表 + `icon_transfer[]` | 同上 | `ui_common.c`（编译进 .a） |
| `components/ui/ui_common/cat_icons.inc` | 4 个侧栏分类图标位图 + `cat_icons[]` | `gen_cat_icons_64.py` | `ui_common.c` / `page_app_manager.c` |
| `components/ui/ui_common/status_icons.inc` | 状态栏小图标 | `gen_status_icons_12.py` | `ui_common.c` |
| `components/apps/pages/mp3_icons_auto.h` | MP3 页内"播放/暂停/下一首"等小图标 | `gen_mp3_iconts.py` | `page_mp3.c` |

### 2.6 常见坑

- **PNG 路径含中文**：`SRC = /Users/linit/Desktop` 在 macOS 上没问题；脚本本身按 UTF-8 解析路径。**不要把 PNG 复制到含空格/特殊符号的目录**（如 `~/Library/Mobile Documents/`），PIL 偶发 read 失败。  
- **PNG 尺寸非 64×64**：脚本自动 `resize(64,64, LANCZOS)`，但建议源图就用 64×64，免缩放糊。  
- **透明 PNG**：脚本不处理 alpha，全透明会被 PIL `convert("L")` 当黑（`getpixel` 返 0），结果图变全黑 —— **务必 PNG 落盘前合成白底**。  
- **改图标后忘记跑脚本**：直接在 C 里改 `.icon_idx` 没用，新图标必须重新生成 `icons_main.inc`。  
- **插队改 MAP**：见 2.3 步 3 的红字警告 —— 已注册应用用枚举名也没用，编译器把枚举当 int 处理，运行时拿 `main_icons[idx]` 的是数字下标。**任何对 MAP 已有行的删/改/插都等于改 ABI**，必须全量改 `s_builtin_apps[]` 的 `.icon_idx` 字段。  
- **"完美图标" 被移走/重命名**：脚本找不到 PNG 会 `FileNotFoundError` 直接挂掉 —— 报错信息直白，按路径恢复即可。

---

## 3. 权限 / 系统能力（需要哪个就声明哪个）

| 能力 | 使用方式 | 说明 |
|---|---|---|
| 安全凭据 | `components/keyvault`（NVS ns=`ln`） | 密码/密钥/WiFi 口令，统一走 keyvault 接口，不自己明文存。 |
| 文件存取 | `sd_scan` / 标准 C 文件 API + FAT | TF 卡 `/sdcard`；需先确认挂载。 |
| 网络 | `wifi_manager` + MQTT | WiFi 连接/校时；MQTT 通信。 |
| 蓝牙 | `bt_manager`(手柄) / `bt_core` | 手柄接入/扫描/直连。 |
| USB 外设 | `usb_hid` / `usb_net` / `usb_bt` | 仿真键鼠 / 网卡共享 / USB 蓝牙适配器。 |
| 音频 | `tone_player` / `audio_player` | 音调播放 / PCM→I2S 音乐。 |
| 震动 | `vibrator`(TM6604) | 按键反馈 / 通知震动。 |
| 屏幕/LCD | `st7305` + `ui_common` | 400×300 1bpp 黑白屏；用统一绘制助手与字体。 |

> 权限即「组件依赖」：在 App 所属组件的 `CMakeLists.txt` 的 `REQUIRES` 加上对应组件，并在 C 里 `#include` 相应头文件。

---

## 4. UI 规范

- **屏**：ST7305，400×300，**1bpp 白底黑字**。
- **字体**：中文统一 `font_zh16`(16×16)；英文 `8×16`；小字用 `am_draw_small` 等。
- **顶部状态栏**：高 24px（y=0..23），内容从 y=24 起。
- **弹窗**：统一 `os_dialog_confirm_ex(ctx,msg,timeout,flags,cb,ud)`；回调 `cb(ctx,result,ud)`，`result=-1`=BACK 取消。弹窗打开时底层不退出，关闭后自动恢复。
- **布局**：左右至少留 4px；行距紧凑、竖排对齐。
- **反色/高亮**：黑白屏用 `inverted=true` 反相实现。
- 一律走 `ui_common` 的统一字体/绘制工具，不自行造字体。

---

## 4.1 滚动列表渲染规范（离屏预渲染整列 + 滚动只抄可视窗 + 硬裁剪出框）

> **长列表/网格滚动（应用管家、商店、任何可拖动列表）一律用本方案**，不要在逐帧渲染里重画整页。
> 参考实现：`components/apps/pages/page_app_manager.c` 的 `am_col_*` 函数组。

**核心原语**：`st7305_blit_1bit(lcd, x0, y0, w, h, bitmap)` —— 标准 1bpp 位图直抄（每行 `bpr=(w+7)/8`，MSB 在左，1=黑，0=白跳过），**自带屏幕边界硬裁剪、只画黑像素**，是"滚动只抄可视窗"的地基。

**三步走：**

1. **离屏预渲染整列**
   - 内容区整列（宽 `W`、高 `H`，含所有行）一次性预渲染成 PSRAM 里的标准 1bpp 位图：
     `bpr = (W+7)/8`，行号 `y` 对应偏移 `y*bpr`。
   - 渲染函数全部**只写列缓冲**（`am_col_hline/vline/rect_outline/label/text_l/icon/...`），不碰屏幕。
   - 只在此列**内容/焦点/安装状态变化时**才重渲染整列（`dirty` 标记），拖动过程零重渲染。

2. **滚动只抄可视窗**
   - 相机 `cam` 以"行"为单位、可小数（拖动跟手，松手平滑吸附焦点行）。
   - 每帧只算可视窗起点：`src_y = round(cam * row_h)`（**像素级偏移**）。
   - 只把可视窗 `[src_y, src_y+win_h)` 抄到屏幕：
     ```c
     st7305_blit_1bit(lcd, grid_x0, content_top, W, win_h, s_col + src_y * bpr);
     ```
   - `win_h` 按屏高裁剪（超出列底就截断），顶从 `content_top` 起，永不穿透顶部子分类条/状态栏。

3. **硬裁剪出框**
   - 出框内容交给 `st7305_blit_1bit` 的屏幕边界裁剪，代码**不做逐像素判断**。

**效果**：拖动时**半行/半字/半按钮自然可见**（像素偏移，绝不等待"完整行"）；滚动流畅（每帧只抄 ≤400×300/8 的字节）。

**性能要点**：
- 列缓冲放 PSRAM（`EXT_RAM_BSS_ATTR` / `heap_caps_malloc(MALLOC_CAP_SPIRAM)`），不够再退回内部 RAM。
- 不要在 `render` 里做文件 I/O / NVS 读 / 扫描——慢操作一律后台任务 + `dirty` 标记 + 完成后取货重建。
- 图标/位图嵌入用 `ui_main_icon_bitmap(idx,&w,&h)` 取原始 1bpp 数据直写列缓冲，不做逐点缩放。

---

## 5. 注册与发布

### 5.0 ⚡ 一键脚手架（V1.2 起，推荐）

新应用**不要手写接线**，用生成器（自动处理 6 处）：

```bash
# 创建
python3 tools/new_app.py snake --label "贪吃蛇" --cat "程序"
python3 tools/new_app.py mytool --label "我的工具" --fullscreen   # 全屏(隐藏状态栏)
python3 tools/new_app.py mytool --label "我的工具" --hidden       # 只进应用中心, 不上主菜单
python3 tools/new_app.py mytool --label "我的工具" --icon ICON_TERM   # 指定图标

# 删除 (与创建对称, 撤销全部接线)
python3 tools/new_app.py mytool --remove
```

自动改动的 6 处：

| # | 位置 | 内容 |
|---|---|---|
| 1 | `components/apps/pages/page_<id>.c` | 页面实现（由 `apps/templates/page_app_template.c` 生成，**直接可编译运行**） |
| 2 | `components/apps/CMakeLists.txt` | `SRCS` 追加 |
| 3 | `components/apps/include/apps_pages.h` | `os_page_<id>_register()` 声明 |
| 4 | `components/system/os/include/os.h` | `OS_PAGE_<UPID>` 枚举值（插在 `OS_PAGE_COUNT` 前） |
| 5 | `components/apps/apps_registry.c` | 装配调用 `os_page_<id>_register();` |
| 6 | `components/apps/apps_registry.c` | `s_builtin_apps[]` 应用表条目（图标/分类/隐藏） |

之后：`python3 tools/check_deps.py` 查越层 → `idf.py build`。

> **依赖纪律**：模板只 `#include "os_app_api.h"`（应用层唯一 API 面）。直接 include
> `st7305.h` / `bt_manager.h` / `board_battery.h` 等驱动头 = 越层，守卫脚本会报。

### 5.1 手工注册（了解原理/特殊场景）

**新增一个编译期 App：**
1. 建页面 `components/apps/pages/page_xxx.c`，实现 `os_module_t`（`render/action/touch`）。
2. 提供 `os_page_xxx_register()`。
3. 在 `components/apps/apps_registry.c` 的 `s_builtin_apps[]` 加一条：
   `{ .id="xxx", .label="名称", .icon_idx=N, .page=OS_PAGE_XXX, .hidden=true, .cat="分类" }`
4. 在 `apps_register_all()` 调用 `os_page_xxx_register()`。

> V1.2 分层后：**应用装配在 L4 `apps` 组件**（`apps_register_all()` 由 `main` 在
> `os_init()` 后调用）；`os_page_dialog_register` 仍属 os 内核自带弹窗页。
> `os_page_*_register` 的声明在 `apps/include/apps_pages.h`（不在 os 层）。

### 5.2 分类规则（2026-09-22 定稿，**Lua 应用与 C 应用同一套**）

| 层 | 由什么决定 | 说明 |
|---|---|---|
| **左侧分类** | **文件放在哪个目录** | `apps/lua/apps/app/` → 应用；`ops/` → 运维；`engine/` → 引擎。C 应用则由 `s_engine_order[]` / `cat` 决定 |
| **顶部子分类** | **LAPP 头部的 `cat=`** | `程序`/`软件` → 软件；`系统`/`工具` → 系统；`游戏` → 游戏；`学习` → 学习；`运维` → （应用分类里那一格显示为"壁纸"）。`cat` 为空 → 只出现在「全部」 |

- 规则落地在 `am_lua.c`：扫描时读 `meta.cat` → `app.cat`，再用 `page_app_manager.c` 的
  `am_sub_want_cat()` 过滤（与 C 注册项**同一套判据**，不会两处跑偏）。
- ⛔ **目录与 `cat` 必须自洽**：`ops/` 里的应用一律写 `cat=运维`（曾经 term/wifeprobe/netdect 写成"系统"，
  与目录矛盾，虽然靠白名单兜住了，但语义脏）。
- ⚠ 加了 `cat=运维` 之后，`am_lua.c` 里那份**文件名白名单**只是给"没打包的裸 .lua"兜底，新应用不用去改它。
- 图标：`sync_apps.py --icon-from-main ICON_X` 把主图标表的图打进 LAPP；`am_lua.c` 会**反查**
  出对应下标（`os_app_t` 只有 `icon_idx`，没有"自带位图"字段）；反查不到 → 回退小游戏机图标。

**隐藏/发布规则：**
- 默认 `hidden=true`：只进**应用管家**（按分类），**不上主菜单**；用户"添加到主菜单"才显示。
  **Lua 应用同理**（长按即可加/删桌面上图标，启动走 `am_lua_open_if_entry()` 先设 boot 路径）。
- 分类固定：引擎(`s_engine_order` 固定顺序)/ 游戏 / 应用 / 运维。
- **新引擎**：同时 ① `apps/apps_registry.c` 加 `hidden=true` `.reveal=true` 条目；② `page_app_manager.c` 的 `s_engine_order[]` 加引擎 id。
- **非必须软件**(如修改机)默认**不启动/不加载**；只在应用管家选"后台运行"时才启动。
- **禁止自启动**：后台服务用 `os_register_service`（不占页面栈、不占主菜单图标）。

**商店下载型 App（运行时注册）：**
- store 安装 → `os_app_register(&os_app)`（id 以 `wasm:` 前缀）→ 主菜单自动出现。
- 商店列表 = 注册表中**排除固定系统软件**的应用；“分类可见=已装备→卸载”，不可见→下载。

**固定系统软件**（不进商店、无卸载/下载）：`设置 / 存储 / 音乐 / 阅读器 / 步步高 / 手柄`。

---

## 6. 参考
- `docs/开发指南.md` —— 完整开发流程/UI 规范
- `components/os/include/os.h` —— `os_app_t` 结构与注册接口
- `components/apps/apps_registry.c` —— 内置应用注册表 + 装配入口 `apps_register_all()`
- `components/apps/pages/page_app_manager.c` —— 商店/分类/卸载逻辑
