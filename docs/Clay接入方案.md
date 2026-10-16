# Clay 接入方案（LinTOS · 1bpp · Lua 应用）

> **结论先行**：Clay **可用**，已实测编译通过（ESP32-S3，`text` 仅 **45.7KB**）。
> 定位：**Clay 只做"算布局"，绘制仍然走你现有的 `app.gfx`** —— 所以 1bpp 的那套性能优化一点不丢，Lua 应用照旧放 TF 卡即玩。

---

## 一、验证结果（已跑，非推测）

| 项 | 结果 |
|---|---|
| 源码 | `reference/clay/`（npm `clay.c@0.14.2` + Gitee 镜像 `gitee.com/mirrors/clay`） |
| 主头文件 | `package/clay/clay.h`（**274KB / 单头 / C99 / 零依赖**，连 libc 都不用） |
| ESP32-S3 交叉编译 | ✅ **EXIT=0, 0 error, 0 warning** |
| 固件体积代价 | **text 45,714 B + data 152 B**（app 分区尚余 0.86MB ⇒ 放得下） |
| 依赖 | **无**；**无 malloc**（静态 arena，符合项目内存铁律） |

⚠ **版本要锁 0.14.2**：Gitee `master` 与 npm 0.14.2 的 API **不一致**（`Clay_EndLayout` 参数、`CLAY()` 宏形态都变了）。以 npm 版 + 其 README 为准。

## 二、这一版的真实 API（踩过坑，务必照抄）

```c
#define CLAY_IMPLEMENTATION
#include "clay.h"

Clay_Initialize(arena, (Clay_Dimensions){400,300}, (Clay_ErrorHandler){Err});
Clay_SetLayoutDimensions((Clay_Dimensions){400,300});
Clay_SetMeasureTextFunction(fn, userData);      /* ⚠ 两个参数 */

Clay_BeginLayout();
CLAY(CLAY_ID("Root"), {
  .layout = { .sizing = { CLAY_SIZING_FIXED(400), CLAY_SIZING_FIXED(300) },
              .padding = CLAY_PADDING_ALL(8), .childGap = 8,
              .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER } },
  .backgroundColor = {255,255,255,255}
}) {
  CLAY(CLAY_ID("Btn"), {
    .layout = { .sizing = { CLAY_SIZING_FIXED(160), CLAY_SIZING_FIXED(40) } }
  }) {
    CLAY_TEXT(CLAY_STRING("hi"), CLAY_TEXT_CONFIG({ .fontSize = 16, .textColor = {0,0,0,255} }));
  }   /* ⚠ CLAY_TEXT 后面必须带分号 */
}
Clay_RenderCommandArray rc = Clay_EndLayout();  /* ⚠ 无参数 */
/* ⚠ 用 rc.length / rc.internalArray[i]（不是 .count / .commands） */
```

**坑位汇总**
| 坑 | 正解 |
|---|---|
| `CLAY({...})` 报 `expected expression before '{'` | 第一参数是 **id**：`CLAY(CLAY_ID("x"), {...})` |
| `CLAY_TEXT(t, { .fontSize=16 })` 报无 `id` 成员 | 用 `CLAY_TEXT(t, CLAY_TEXT_CONFIG({...}))` |
| `Clay_SetMeasureTextFunction(fn)` 参数不足 | **两参数** `(fn, userData)` |
| `Clay_EndLayout()` 参数不足 / `.count` 不存在 | 0.14.2 是 **无参** + **`.length`** |

## 三、分层架构（关键：不碰现有绘制层）

```
Lua 应用（放 TF 卡，apps/{app,ops}/*.lua）
        │  app.ui.begin() / app.ui.el{...} / app.ui.end()
        ▼
components/apps/lua_bridge/luat_lib_clay.c   ← 新增：把 Clay 的声明/布局包成 Lua API
        │  产出 Clay_RenderCommandArray
        ▼
components/ui/…/clay_render.c                 ← 新增：render command → 现有 1bpp 图元
        │  RECTANGLE→app.fill / TEXT→app.mix / BORDER→app.rect / SCISSOR→裁剪 / IMAGE→app.img
        ▼
ST7305 1bpp 帧缓冲（PSRAM）
```

⇒ **`app.gfx` 那一层完全不动** ⇒ `hline/vline/poly` 行程合并、圆角逐行裁角、脏标记、静态/动态分层 **全部保留**。

## 四、render command → 现有 API 映射

| Clay 命令 | 1bpp 画法 | 备注 |
|---|---|---|
| `RECTANGLE`（`backgroundColor` + `cornerRadius`） | `UI.fill_round(x,y,w,h,r, 墨/白)` | 颜色**二值化**：`alpha>0 且亮度<阈值 → 墨(0) 否则 白(1)`；半径≥6 才看得出弧度 |
| `TEXT`（`textColor` + `fontSize`） | 中文 `app.mix` / 12px `app.zhsm`；ASCII `app.texts`；大字 `app.big` | ⚠ **中英混排必须分两次画**（Clay 的 text 是一整串 ⇒ 需在渲染层按 ASCII/非ASCII 切分） |
| `BORDER`（四边宽） | 四条 `app.hline/vline` + 圆角 | 1bpp 用**线宽/线密**表达层级 |
| `SCISSOR_START/END` | 保存/恢复裁剪矩形 | 滚动容器必需 |
| `IMAGE` | `app.img.blit` | 不透明图集可免 clear |
| `CUSTOM` | 透传 `userData` | 给"地图层/波形"这类自绘留口子 |

## 五、Lua API 设计（与已交付规范同名，便于迁移）

```lua
local clay = app.ui          -- 桥注册 app.ui 子表
clay.begin(400, 300)
clay.el("Btn", { w=160, h=40, r=6, bg=1, pad=8, gap=4 })
clay.text("开始", { size=16, inv=0 })
clay.end()                   -- 内部：Clay 布局 → 遍历命令 → 调 app.gfx 画
```
- 布局声明**在 Lua 里**（放卡即玩 ✔）；Clay 与渲染器**在固件里**（只编一次）
- 元素 id 用**字符串索引**（`clay.el(name, idx)`），与 Clay 的 `CLAY_ID/CLAY_IDI` 对应
- **触摸**：`clay.pointer(x, y, down)`（对应 `Clay_SetPointerState`）+ `clay.hovered(id)`（对应 `Clay_Hovered`）
- **滚动**：`clay.scroll(id, enable, dx, dy, dt)`（对应 `Clay_UpdateScrollContainers`）

## 六、内存与体积

| 项 | 值 | 放哪 |
|---|---|---|
| Clay 代码 | **45.7KB** | 固件 flash |
| arena | `Clay_MinMemorySize()`（需在设备上实测；小 UI 几十 KB 足够，8192 元素官方称 ~3.5MB） | **PSRAM**（静态数组，**不进 bss**） |
| malloc | **零** | 符合项目铁律 |
| 渲染命令数组 | 由 arena 内分配 | PSRAM |

⚠ 与项目现有内存铁律一致：Clay 的 arena 是**静态数组** ⇒ 天然不做"运行期动态分配"。

## 七、里程碑

| 步 | 内容 | 验收 |
|---|---|---|
| **M1** | 把 `clay.h` 放进 `components/vendor/`，新增 `luat_lib_clay.c`：`begin/el/text/end/pointer/hovered/scroll` | `lua_check` 通过；真机 `app.ui.begin{}end()` 能画出一个框 |
| **M2** | `clay_render.c`：命令 → `app.gfx`（先只做 RECTANGLE + TEXT） | 抓屏看到圆角框 + 中英混排正确 |
| **M3** | 补 BORDER / IMAGE / SCISSOR（滚动容器） | 列表能滚 |
| **M4** | 性能：脏区 + 局部刷新（Clay 的 clip/invalidate 天然支持） | `OS: render 慢` 的绘制部分 <30ms |
| **M5** | 用它重写一个现有应用做示范（建议 `qqtang.lua` 的菜单页） | 你验收 |

## 八、与已交付物的关系

- **`_ui.lua` 不废弃** —— 简单界面（按钮/列表/弹窗/进度条）零依赖、立刻能用，**保留为回退方案**
- **`docs/Lua应用UI规范.md` 继续有效** —— 它定义的组件语义/状态表现/性能九条，Clay 只是把"布局"这一步外包了
- 迁移成本低：两套 API 形态一致（`UI.button` ↔ `clay.el{...}`），应用逻辑不动

---

## 九、待你确认的一件事

**要不要现在就开工 M1？**（把 `clay.h` 引进工程 + 写最小桥，先只让一个矩形+一行字能在真机上画出来 —— 半天内能看到效果。）