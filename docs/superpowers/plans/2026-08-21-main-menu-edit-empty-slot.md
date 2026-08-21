# 主菜单编辑模式「中间预留空槽」修复实施计划

> **For agentic workers:** 按任务逐项实施（可选用 subagent 或本会话内联执行）。步骤用 checkbox（`- [ ]`）跟踪。

**Goal:** 修好主菜单编辑（排序）模式下「中间图标被原地隐藏而非腾出空槽」的问题：中间被拿起/移走后，应形成一个清晰可见的 100px 预留空槽（放置目标），左右图标保持原位，左右拖动旋转时始终「跳过中间」，邻居图标经中央槽滑过。

**Architecture:** 编辑模式的渲染在 `menu_system.c` 的 `render_main()` 里分两路：`s_hold_active`（已拿起）走「居中空槽」逻辑，非拿起走通用轮盘循环（含错误的「中间原地闪烁隐藏」）。修复 = ① 给中间预留槽补一个淡淡的虚线框标记，让「空位」肉眼可辨；② 去掉「原地闪烁隐藏」路径；③ 触控命中尺寸表与渲染尺寸表统一（扁平 100/64）。

**Tech Stack:** C / ESP-IDF v5.5.5 / ESP32-S3 / ST7305 显示屏。

---

## 现状与根因（已核实）

文件 `components/menu/menu_system.c`：

- 拿起状态下标：`render_main()` L9201-9218，对 `n` 个其余图标按与相机偏移摆到整数槽，`o==0` 时 `continue`（中间真空），邻居保持在 100px 网格上。**此逻辑方向正确**，但中间只是个「空白」，没有任何「这是放置槽」的提示，肉眼看起来就是「图标被藏起来了」。
- 非拿起但仍在编辑（放下后）：else 分支 L9227-9228 用
  `if (isCenter && !s_hold_active && !((now_ms/400)&1)) continue;`
  把中间图标**原地闪烁隐藏**，相邻图标不动 —— 这就是「隐藏了，不是腾出空间」的直接来源，必须删掉。
- 触控命中尺寸表与实际渲染尺寸不一致：
  - L12997（`draw_main_touch` 主菜单命中）`size = (ad==0)?100:(ad==1)?70:(ad==2)?50:40`
  - L11410（长按删除命中的 size 表）同上。
  两处都应改成扁平 `ad<=0 ? 100 : 64`，与渲染一致。

用户确认的交互模型：中间图标拿起后下移到下方托盘，左右图标仍在原位；左右拖动旋转时其余图标「当作中间不存在，跳过中间」，中间永远是待放置的空槽（100px）。

---

### Task 1: 新增「预留空槽」淡虚线框绘制函数

**Files:**
- Modify: `components/menu/menu_system.c`（在 `draw_vline` L207 之后、`draw_rect_outline` L212 之前的函数区追加）

- [ ] **Step 1: 在 `draw_vline`（L207-L210）与 `draw_rect_outline`（L212）之间插入新函数**

在 `components/menu/menu_system.c` 的 `draw_vline` 定义之后追加：

```c
/* V1.0.9x: 画一个淡虚线框, 表示编辑模式下「预留的空白放置槽」(中间空位).
 * 虚线比实线淡, 明确提示此处留空待放置, 而非图标被简单隐藏. */
static void draw_slot_marker(st7305_handle_t *lcd, int cx, int cy, int size) {
    int x0 = cx - size / 2, x1 = cx + size / 2;
    int y0 = cy - size / 2, y1 = cy + size / 2;
    /* 顶/底边: 每 4 点画 1 点 的虚线 */
    for (int x = x0; x <= x1; x += 4)
        st7305_draw_pixel(lcd, x, y0, ST7305_COLOR_BLACK);
    for (int x = x0; x <= x1; x += 4)
        st7305_draw_pixel(lcd, x, y1, ST7305_COLOR_BLACK);
    /* 左/右边: 每 4 点画 1 点 的虚线 */
    for (int y = y0; y <= y1; y += 4)
        st7305_draw_pixel(lcd, x0, y, ST7305_COLOR_BLACK);
    for (int y = y0; y <= y1; y += 4)
        st7305_draw_pixel(lcd, x1, y, ST7305_COLOR_BLACK);
    /* 虚线框中心再放一个 1px 的十字十字准星, 提示「此处可放置」 */
    st7305_draw_pixel(lcd, cx, cy - 2, ST7305_COLOR_BLACK);
    st7305_draw_pixel(lcd, cx, cy + 2, ST7305_COLOR_BLACK);
    st7305_draw_pixel(lcd, cx - 2, cy, ST7305_COLOR_BLACK);
    st7305_draw_pixel(lcd, cx + 2, cy, ST7305_COLOR_BLACK);
}
```

- [ ] **Step 2: 编译验证新增函数语法**

Run（增量编译，快速）：`idf.py build`（用工程 build.sh 导出的环境）
Expected: 编译通过，无新增错误（旧 `sym_label` 初始化 warning 属既有警告，可忽略）。

---

### Task 2: 拿起状态下，在中间预留槽绘制淡虚线框标记

**Files:**
- Modify: `components/menu/menu_system.c` `render_main()` 拿起分支 L9201-9218

- [ ] **Step 1: 在拿起分支图标循环结束后追加标记绘制**

把 L9201-9218 的拿起分支改为末尾追加对 `draw_slot_marker` 的调用（在循环 `for` 关闭后、整个 `if` 分支结束前）：

```c
    if (s_reorder_active && s_hold_active && s_main_order_count > 0) {
        /* V1.0.9x: 拿起时中间真空缺口 + 随相机连续定位 — 每个图标按与相机的连续偏移摆放,
         * 中间(±0.5 内)不画, 滚动顺滑不闪格 */
        int n = s_main_order_count;
        float c = cam;
        for (int i = 0; i < n; i++) {
            float rel = (float)i - c;
            rel = fmodf(rel, (float)n);
            if (rel < 0) rel += n;
            if (rel > n * 0.5f) rel -= n;            /* 归到 (-n/2, n/2] */
            int o = (int)floorf(rel + 0.5f);         /* 整数槽位: 空位严格在屏幕正中 */
            if (o == 0) continue;                    /* 中间真空缺口(预留放置槽) */
            int cx = (int)roundf((float)center_x + (float)o * (float)icon_spacing);
            int size = 64;   /* 拿起时: 所有非中间图标统一 64×64 (中间为真空缺口) */
            if (cx + size <= 0 || cx - size >= SCREEN_W) continue;
            const bbk_module_t *m = bbk_module(menu_main_phys(state, i));
            draw_main_icon_stretched(lcd, cx, icon_center_y, size, size, main_icon_index(m ? m->icon_idx : 0));
        }
        /* V1.0.9x+: 中间预留 100px 槽位: 画淡虚线框标记, 让「腾出的空间」清晰可辨 */
        draw_slot_marker(lcd, center_x, icon_center_y, DRAW_SLOT_MARKER_SIZE);
    } else {
```

其中 `DRAW_SLOT_MARKER_SIZE` 采用与图标同宽的 64（`icon_size` 均为 64），确保虚线框正好框出「一个图标的位置」。

- [ ] **Step 2: 定义与渲染一致的尺寸常量**

在 `render_main()` 内 `draw_slot_marker` 调用上方（如 L9163 `int icon_spacing = 100;` 附近）加入一行：

```c
    int DRAW_SLOT_MARKER_SIZE = 64;   /* 中间预留槽标记与图标同宽: 100px 槽内框出 64px 虚框 */
```

（说明：`DRAW_SLOT_MARKER_SIZE` 用局部变量避免改动文件顶部宏区，命名同 Task 3 命中逻辑中变量风格。）

- [ ] **Step 3: 编译并对照 Task 1 Step 2 说明通过**

Run：`idf.py build`
Expected: 通过。

---

### Task 3: 删除「中间原地闪烁隐藏」路径

**Files:**
- Modify: `components/menu/menu_system.c` `render_main()` else 分支 L9227-9228

- [ ] **Step 1: 删除产生「隐藏中间图标」的两行**

当前 else 分支内两行：

```c
            int size = main_icon_size_cont(dist);
            bool isCenter = s_reorder_active && (g == (int)lroundf(cam));
            if (isCenter && !s_hold_active && !((now_ms / 400) & 1)) continue;  /* 未拿起时中间图标闪烁 */
```

改为只保留尺寸计算，删掉 `isCenter` 判定那两行：

```c
            int size = main_icon_size_cont(dist);
            if (cx + size > 0 && cx - size < SCREEN_W) {
```

（注意：保留下面的 `if (cx + size...)` 原判断块不动，仅删上面对 isCenter 的两行。）

- [ ] **Step 2: 编译验证**

Run：`idf.py build`
Expected: 通过。此改动后：编辑模式下只要未拿起，中间图标正常显示（100），不再原地闪烁隐藏。

---

### Task 4: 触控命中尺寸表与渲染统一（扁平 100/64）

**Files:**
- Modify: `components/menu/menu_system.c`
  - L12997（`menu_touch_main` 命中循环里的 size）
  - L11410（长按删除命中循环里的 size）

- [ ] **Step 1: 修正主菜单触摸命中尺寸表**

L12996-12997 当前：
```c
            int ad = diff < 0 ? -diff : diff;
            int size = (ad == 0) ? 100 : (ad == 1) ? 70 : (ad == 2) ? 50 : 40;
```
改为（与渲染 `main_icon_size_cont` 的 100/64 扁平一致）：
```c
            int ad = diff < 0 ? -diff : diff;
            int size = (ad == 0) ? 100 : 64;
```

- [ ] **Step 2: 修正长按删除命中尺寸表**

L11409-11410 当前：
```c
                        int ad = diff < 0 ? -diff : diff;
                        int size = (ad == 0) ? 100 : (ad == 1) ? 70 : (ad == 2) ? 50 : 40;
```
改为：
```c
                        int ad = diff < 0 ? -diff : diff;
                        int size = (ad == 0) ? 100 : 64;
```

- [ ] **Step 3: 编译验证**

Run：`idf.py build`
Expected: 通过。

---

### Task 5: 全量合并、生成镜像

- [ ] **Step 1: 合并 Flash 镜像**

Run（在工程根目录）：
```bash
bash merge_flash.sh
```
Expected: 生成 `dist/merged_16mb.bin`，大小 16777216 字节，SHA-256 打印。

- [ ] **Step 2: 记录新镜像信息**

记录 `dist/merged_16mb.bin` 的文件时间与 SHA-256，确保烧录的是本次最新产物（防止烧旧镜像，见经验教训）。

---

### Task 6: 烧录与真机验证

**前置**：让设备进入下载模式（按住 BOOT → 按一下 RESET → 松开 BOOT），待串口出现 `/dev/cu.usbmodem…`。

- [ ] **Step 1: 烧录**

Run（用先前验证可用的 115200 波特率，避免 460800 通信不稳）:
```bash
/Users/linit/.espressif/python_env/idf5.5_py3.13_env/bin/python -m esptool \
  --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 115200 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 "<工程根>/dist/merged_16mb.bin"
```
Expected: `Hash of data verified.` + `Hard resetting`.

- [ ] **Step 2: 真机验收（编辑排序场景）**

操作：主菜单 → 长按中间图标 3 秒不见松开。
验收（**逐条核对**）：
1. 中间图标下移到下方托盘（64×64）。
2. 中间原本位置出现**淡虚线框**（64×64 虚框 + 中心十字），明确显示「预留空槽」，而非一团空白/消失。
3. 左右相邻图标仍在其原位（±1 槽 100px），不塌缩。
4. 左右滑动（拖动/按键）时：邻居图标绕行滑过中央槽，**中央槽始终为空**（只有虚线框），即「跳过中间」。
5. 松手/放回：虚线框消失，图标落位，编辑继续；点下方托盘图标可放回中间。
若逐条符合 → 修复达成。

- [ ] **Step 3: 串口确认状态机（可选辅助）**

采集串口日志，重点确认拿起/放下打印 `编辑: 拿起 phys=... at slot ...` / `编辑: 放入中间槽 ...` / `编辑: 取消`，与操作一致。（现有日志已含这些打印。）

---

## Self-Review

**需求覆盖：**
- 中间图标下移后左右图标原位 → Task 3 保留邻居原位（不塌缩），Task 2 只在拿起时标记空槽。
- 拖动时「跳过中间」，中央始终空 → 拿起分支 `o==0 continue` + 中央虚线框标记（Task 2）。
- 空位一个槽 100px → `icon_spacing` 保持 100，虚线框 64 居中（Task 2）。
- 去掉「原地隐藏」 → Task 3。
- 命中与显示一致 → Task 4。

**占位符检查：** 所有步骤含实际代码与确切命令，无 TBD/「适当处理」类占位。

**类型/命名一致性：** `draw_slot_marker(lcd,cx,cy,size)` 在 Task 1 定义、Task 2 调用，参数一致；`DRAW_SLOT_MARKER_SIZE=64` 局部变量与调用一致；命中 size 扁平 `(ad==0)?100:64` 与 `main_icon_size_cont` 返回 100/64 一致。

**风险：** 虚线框仅在 `s_hold_active` 时绘制，非拿起编辑态（放下后仍编辑）中间正常显示图标，符合用户「只在拿起时腾空槽」的预期。