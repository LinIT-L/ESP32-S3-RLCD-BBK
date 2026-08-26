# 规划书：把系统重构为「标准化模块化 OS」

> 目标：把当前 `components/menu/menu_system.c`（约 1.5 万行、503 个 static 函数、394 行共享 `menu_state_t`）拆成一个**标准化模块化 OS**
> 内核只负责页面栈/事件路由/输入分发/内存生命周期；
> 状态栏/壁纸/触摸/虚拟手柄/每个应用页各自成独立、自包含模块（私有状态，互不穿透）。
>
> 本规划基于已确认决策：**新建 `components/os` / 彻底拆掉共享状态 / 内核→试点页→逐页推进 / 状态栏为全局 service**。
> WASM 应用商店（见 `规划_WASM应用商店.md`）排在本规划 **P4 之后**，作为 os 的一个新模块接入。

---

## 0. 已核实的关键现状（本规划依据）

| 现存物 | 状态 |
|---|---|
| `menu_system.c` 1.5万行 / 503 static / 394 行共享 `menu_state_t` | 巨石，含约 20 个内嵌 UI 模块 + 大量补丁过渡代码 |
| `components/engine_manager` | 已是统一 load/unload 引擎注册表（可加 ENGINE_WASM），**不动** |
| `game_run_engine_ops_t` + `game_run_loop` | menu_system 私有 static，**不强制复用** |
| terminal / whiteboard / diagnosis_v2 | 文件已独立，但**签名仍是 `(menu_state_t *st, ...)`，共享大结构** |
| keyboard / favorites / file_browser / sd_scan / wallpapers / app_cleanup / wifi_manager | 已独立组件，可轻对接 |
| 各模拟器引擎 + virtual_keys / web_gamepad / audio_player / book_reader / tone_player / usb_hid / input / app_board / board_battery | 已独立组件，**不动** |

---

## 1. 目标架构

```
┌────────────────────────────────────────────────────────────┐
│  外壳 main.c（不变：初始化 -> os_init -> 主循环喂 action）    │
├────────────────────────────────────────────────────────────┤
│  components/os  ——  标准化模块化 OS 内核                     │
│   ├─ ui_ctx_t     极薄公共上下文（lcd/current_page/        │
│   │               needs_redraw/modal_active/事件入队）      │
│   ├─ ui_module_t  标准模块契约（on_enter/on_exit/render/   │
│   │               action/touch/poll/modal）                │
│   ├─ kernel       页面栈 / 事件路由 / 输入分发 /             │
│   │               内存生命周期 / 返回链                     │
│   ├─ services     全局服务：statusbar / volume / battery / │
│   │               bt / power / screensaver-timer           │
│   └─ shell        注册表 + 页面装配 + 弹窗调度              │
├────────────────────────────────────────────────────────────┤
│  应用页模块（每页一个 .c，私有 static/struct state）          │
│   main_menu / settings / select_game / book / mp3 /        │
│   pomodoro / wallpaper / usb_hid / gamepad / icon_editor / │
│   wifi_kb / term / wb / diag ...                           │
├────────────────────────────────────────────────────────────┤
│  已独立组件（直接复用/轻对接）                                 │
│   引擎 engine_manager + 各模拟器 / keyboard / favorites /   │
│   file_browser / sd_scan / wallpapers / audio_player /      │
│   virtual_keys / input / app_board / board_battery ...      │
└────────────────────────────────────────────────────────────┘
```

### 核心原则
- **无共享大结构**：每模块状态私有；模块间不相互调用内部字段。
- **薄公共上下文**：`ui_ctx_t` 只给内核与模块传"世界句柄"。
- **全局服务单例**：状态栏/音量/电量/蓝牙 = service，任何页面可叠加，不占页面栈。
- **纯引擎/纯组件不动**：只做接口对接。

---

## 2. 模块契约定义（os/include/os.h）

```c
/* 极薄公共上下文 —— 内核给所有模块的“世界句柄” */
typedef struct ui_ctx ui_ctx_t;
struct ui_ctx {
    st7305_handle_t *lcd;      /* 屏句柄 */
    int  current_page;         /* 当前模块 id */
    bool needs_redraw;         /* 脏标记 */
    bool modal_active;         /* 是否有模态覆盖 */
    /* 私有闲置: 每模块 state 留在各自 .c, 不放进这 */
};

/* 标准模块契约 —— 任何“页面/应用/服务”都实现这组 ops */
typedef struct ui_module {
    const char *name;
    void (*on_enter)(ui_ctx_t *);               /* 进入: 分配资源/加载 */
    void (*on_exit)(ui_ctx_t *);                /* 退出: 释放内存 + 存档 */
    void (*render)(ui_ctx_t *);                 /* 渲染整页 */
    bool (*action)(ui_ctx_t *, menu_action_t);  /* 按键: true=已消费不向上 */
    bool (*touch)(ui_ctx_t *, int x, int y);    /* 触摸 hit-test: true=命中 */
    void (*poll)(ui_ctx_t *);                   /* 每帧后台轮询(可选) */
    bool modal;                                 /* 是否模态覆盖页 */
} ui_module_t;

/* 模块 id 枚举（迁移后的页面清单） */
enum {
    PAGE_MAIN, PAGE_SELECT_GAME, PAGE_SETTINGS, PAGE_GAMEPAD,
    PAGE_SETTINGS_DISPLAY, PAGE_SETTINGS_TIME, PAGE_SETTINGS_BT, PAGE_SETTINGS_SD,
    PAGE_SETTINGS_INFO, PAGE_BTINFO, PAGE_KEY_CONFIG, PAGE_VOLUME, PAGE_RETURN_GAME,
    PAGE_FILE_BROWSER, PAGE_MP3_PLAYER, PAGE_GAME_SETTINGS, PAGE_GB_GAME, PAGE_BOOK,
    PAGE_WALLPAPER, PAGE_POMODORO, PAGE_USB_HID, PAGE_APP_MANAGER, PAGE_ICON_EDITOR,
    PAGE_DIAGNOSIS, PAGE_WQX, PAGE_VPET, PAGE_MESHTASIC, PAGE_WHITEBOARD, PAGE_TERMINAL,
    PAGE_COUNT
};
```

> 说明：`menu_action_t` / `menu_page_t` 语义保留（enum 值不变），做到 main.c / input.c / 各引擎交互尽量少改。

---

## 3. 内核职责（components/os/kernel）

- **页面栈**：push/pop/peek，返回链（返回主菜单保持位置）。
- **事件路由**：`input` 动作 → 当前模块 `action()`；未消费再走服务/退栈。
- **输入分发**：触摸 hit-test → 当前模块 `touch()`；未命中回退普通确认。
- **内存生命周期**：模块 `on_enter` 分配 / `on_exit` 释放；回到主菜单统一 `unload_all`，引擎残留复核。
- **公共服务接口**：
  ```c
  void os_init(ui_ctx_t *ctx, st7305_handle_t *lcd);
  void os_register(const ui_module_t *mod);   /* 编译期+运行期均可 */
  void os_push(int page_id);                  /* 进入页(调用 on_enter, 记录返回链) */
  void os_pop(void);                          /* 退出当前页(调用 on_exit) */
  void os_handle_action(ui_ctx_t*, menu_action_t);
  bool os_handle_touch(ui_ctx_t*, int x, int y);
  void os_render(ui_ctx_t*);
  /* services */
  void svc_statusbar_render(ui_ctx_t*, const char *center);  /* 全局可叠加 */
  void svc_volume_set(uint8_t v); void svc_volume_get(void);
  ...
  ```

---

## 4. 迁移清单（现状 → 归属）

| # | 模块 | 类别 | 现状 | 迁移方式 |
|---|---|---|---|---|
| K | os 内核 | 内核 | 无 | 新建 |
| S1 | statusbar 状态栏 | 全局服务 | menu_system | 抽 service（复用 status_icons*.inc / bt_icon_png） |
| S2 | volume | 全局服务 | menu_system | 抽 service |
| S3 | power 软关机/锁屏 | 全局服务 | menu_system | 抽 service（复用 input / touch_panel） |
| S4 | screensaver-timer | 全局服务 | menu_system | 抽 service（复用 wallpapers） |
| 1 | main_menu 主菜单 | 应用页 | menu_system | 试点页（相机动画/拖动/图标 icons_main.inc） |
| 2 | select_game 游戏分栏 | 应用页 | menu_system | 复用 engine_manager + 各引擎 |
| 3 | settings 设置 | 应用页 | menu_system | 复用 diagnosis_v2 | | 4 | book_ui 电子书 UI | 应用页 | menu_system | 复用 book_reader |
| 5 | mp3_ui | 应用页 | menu_system | 复用 audio_player |
| 6 | pomodoro | 应用页 | menu_system | 复用 tone_player |
| 7 | wallpaper_set | 应用页 | menu_system | 复用 wallpapers |
| 8 | usb_hid_set | 应用页 | menu_system | 复用 usb_hid |
| 9 | wifi_kb | 应用页 | menu_system | 复用 keyboard |
| 10 | gamepad_ui | 应用页 | menu_system | 复用 bt_manager / bt_map |
| 11 | icon_editor | 应用页 | menu_system | 复用 SD |
| 12 | app_manager | 应用页 | menu_system | 复用 favorites |
| 13 | term / wb / diag | 应用页 | 已拆但共享大结构 | **改签名为 ui_ctx + 私有 state** |
| D | dialog 通用弹窗 | 内核/工具 | menu_system | 抽 kernel 弹窗调度 + draw_dialog_frame（在 ui_common） |

### 已独立、**直接复用不动**
`engine_manager` + 各模拟器、`keyboard / favorites / file_browser / sd_scan / wallpapers / app_cleanup / wifi_manager / virtual_keys / web_gamepad / audio_player / book_reader / tone_player / usb_hid / input / app_board / board_battery / touch_panel / vibrator / boot_switch`

### 可删残留
`diagnosis.c`（V1 死代码，不在编译列表）、`menu_system.c` 迁移完后整体删除。

> **ui_common（绘图内核）** 是本 OS 的绘制基础（已在 components/menu 半成品，含 fill_rect/draw_zh/draw_ascii/图标等）。建议迁入 os 的 drawing 子层并收口到可编译（见 P1），作为所有模块的最小依赖。

---

## 5. 分阶段任务（每步可编译、可烧录、可回退）

| 阶段 | 内容 | 产出 | 验收 |
|---|---|---|---|
| **P0** | 新建 `components/os`：ui_ctx + ui_module 契约 + kernel(page栈/事件路由/输入分发/内存)+注册表 | os 空壳 + 空桌面 | 编译 + boot 正常 |
| **P1** | 收口 ui_common(绘图层) → 迁 statusbar(全局service) + main_menu(试点页)，旧 menu_system 保留可回退 | 桌面级可用，可回退旧版 | 实机 boot + 主菜单正常 |
| **P2** | 逐页迁移 settings/select_game/book/mp3/pomodoro/wallpaper/usb_hid/gamepad/icon_editor/wifi_kb/app_manager，改签名 ui_ctx+私有 state | 全部应用页在新 OS 上跑 | 实机逐页回归 |
| **P3** | 重构已拆组件 term/wb/diag 签名为 ui_ctx+私有 state | 终端/白板/诊断在新 OS 上跑 | 实机回归 |
| **P4** | 收口：外壳 main.c 切到 os 内核，删 menu_system.c/diagnosis.c，全量回归 | 标准化模块化 OS 落地 | 全功能回归 |
| **P5** | WASM 应用商店 作为新模块接入（见 `规划_WASM应用商店.md` Phase 2+） | 商店 + wasm 应用 | Phase 验收 |

> 回退策略：全程保留旧 menu_system 与输入开关（`CONFIG_OS=1` 走新 os，否则回退旧 menu_system）。每迁移一个页面都验证"该页面功能不降级"。符合你"先解决基础，让现有功能正常运行"的要求。

---

## 6. 关键决策记录（已确认）

| 项 | 决策 |
|---|---|
| 骨架位置 | 新建 `components/os`，独立演进 |
| 共享状态 | 彻底拆掉 menu_state_t(394行)，模块私有 state 留各自 .c |
| 模块契约 | ui_ctx(薄) + ui_module(ops)；服务单例不占页栈 |
| 迁移节奏 | 内核 → 试点页 → 逐页推进；每步可编译/烧录/回退 |
| 状态栏 | 全局 service（复用 menu_draw_status_bar 语义） |
| 引擎/已拆组件 | 纯引擎不动；term/wb/diag 仅改签名 |
| 优先级 | 先基础(模块化 OS)让现有功能正常 → 再 WASM 商店 |

---

## 7. 风险清单

- 共享状态彻底拆掉 → 需同步改造 term/wb/diag 三处签名，回归面大 → 逐页迁移 + 保留旧版回退开关对冲。
- 1.5 万行巨石迁移周期内中间态多 → 用 CONFIG_OS 开关保持随时可回退到能跑版本。
- PSRAM / 性能：重构不改数据布局原则，关键仍内部 RAM，帧缓冲/大块 PSRAM。
- 各模块历史补丁逻辑迁错 → 每页迁移后单独实机回归该页所有入口。

---

## 8. 交付物
- 每阶段 `docs/` 记录：新增/修改/删除文件清单、自测方法（串口期望 / 菜单操作路径）、内存基线。
- P4 后输出：最终组件/模块拓扑图、模块注册表、公共服务清单。