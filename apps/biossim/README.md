# BIOS 模拟器 · 装机与固件选项学习台

**纯黑白（掌机 1bpp）** · 单文件 HTML · 双击即用（Safari / Chrome 都行，不联网、不装东西）。
**没有"电脑彩色版"** —— 只有黑底白字 + 选中整行反白这一种观感。
改这里**不会**影响你的真机 —— 它是练习台。

```
双击打开：  apps/biossim/biossim.html
```

---

## 7 家厂商界面（同一批选项，按各家真实归类与真实选项名摆放）

**7 家共用一套黑白配色**，区别在**菜单结构 / 选项叫法 / 身份值**——那才是"像不像真机"的实质。
选择界面**只显示图标 + 品牌短名**（Award / AMI / 华硕 / 戴尔 / 惠普 / 联想 / 华为），不显示介绍文字。

| 品牌 | 版式 | 真实菜单结构 |
|---|---|---|
| Award / Phoenix（90 年代台式机） | 左侧竖菜单 | Standard CMOS Features / Advanced BIOS/Chipset Features / Integrated Peripherals / Power Management Setup / PnP-PCI / PC Health / Load Fail-Safe Defaults / Save & Exit |
| AMI Aptio V（通用台机） | 顶部横排 | Main / Advanced / Security / Boot / Save & Exit |
| 华硕 ASUS（Aptio V · Ai Tweaker） | 左侧竖菜单 | My Favorites / Main / Ai Tweaker / Advanced / Monitor / Boot / Tool / Exit |
| 戴尔 Dell（OptiPlex / Latitude） | 左侧竖菜单 | General / System Configuration / Video / Security / Secure Boot / Performance / Power Management / POST Behavior / Virtualization Support / Maintenance / System Logs / Exit |
| 惠普 HP（F10 Setup） | 顶部横排 | Main / Security / Advanced / UEFI Drivers / Exit |
| 联想 ThinkPad（IBM 风格） | 左侧竖菜单 | Config / Date-Time / Security / Startup / Restart / Network / Display |
| 华为 HUAWEI（MateBook） | 顶部横排 | 信息概览 / 安全设置 / 启动配置 / 性能调节 / 退出 —— **菜单与选项名全中文** |

**品牌专属叫法与身份值**（避免"在戴尔上显示 HP EliteBook"这种穿帮）：

- 戴尔：`SATA Operation`、`System Model = OptiPlex 7090`、`Service Tag = J4K2L83`、BIOS `1.28.0 (06/11/2024)`
- 惠普：`SATA Emulation`、`Embedded Security Device`、`Product Name = HP EliteBook 840 G8`、`UUID Number = 4C4C4544-…`
- 联想：`SATA Mode`、`Setup Defaults`、`Product Name = ThinkPad X1 Carbon Gen 9`、EC `1.14`
- 华硕 `ASUS PRIME Z590-A` · AMI `AMI Aptio V Reference Board` · Award `Generic ATX i815EP System`

**真选项，不是编的**（来自真机 IFR dump，见 `ref/SOURCES.md`）：
`SATA Mode = IDE Mode / AHCI Mode / RAID Mode`、`SATA Mode = AHCI / RAID`、`NVMe RAID mode`、`RAID Type`、
`Storage Hierarchy`、`CSM Support`、`Boot option filter`、`Storage OpROM policy`、
`Secure Boot` + `Secure Boot Mode` + `Key Management`、
`TPM 2.0 UEFI Spec Version = TCG_1_2 / TCG_2`、`TPM 2.0 InterfaceType = CRB / TIS`、`AMD fTPM switch`、
`Security Device Support` / `TPM Device Selection (dTPM 2.0 / PTT)` / `TPM State`、
`SVM Mode`（AMD 的 VT-x）、`Intel(R) AMT` + `AMT SOL / IDE-R` + `Mebx Password`、`Intel(R) VT-x / VT-d`、
`XHCI Hand-off`、`Legacy USB Support`、`Bootup NumLock State`、`Network Stack`、
`Boot Priority Order`（真实设备名，可上下移动）……
只读项（BIOS Version / CPU / 内存 / 序列号）真机里也是灰的，这里也改不动。

**8 个装机任务**：Win7 传统 / Win7 UEFI+CSM / Win10 / **Win11** / **Linux** /
**RST→AHCI（不重装）** / **IDE→AHCI（不蓝屏）**。
任务条上实时给 `符合 6/8`；按 `F8` 逐条列 ✅/❌ 并写明"这一项为什么重要、现在这样会卡在哪一步"。

---

## 三个模式

进入程序后：**先选品牌（图标网格）→ 再选模式与方案 → 进全屏 BIOS**。

| 模式 | 里面有什么 | 判定 |
|---|---|---|
| **系统安装** | 限定"装哪个系统"：Win7 传统 / Win7 UEFI+CSM / Win10 / **Win11** / Linux；另有"已装好系统改 SATA 模式"（RST→AHCI 不重装、IDE→AHCI 不蓝屏） | 逐条判"这台机器能不能装" |
| **阵列配置** | 给**方案要求**，你去把 RAID 配出来：<br>· 双盘 RAID0 提速（剪辑临时盘，要最快顺序读写、不要冗余）<br>· 双盘 RAID1 镜像（坏一块还能开机）<br>· 三盘 RAID5 容错（容量利用率 + 容错一块） | 逐条判 RAID 配得对不对（SATA Mode=RAID / 级别 / 选盘 / 条带 / 创建 / 可引导） |
| **自由模式** | 什么规则都不判，随便翻随便改 | 不判定 |

> 阵列配置模式的完整设置都在 **Advanced → SATA Configuration → RAID Configuration** 里
> （7 个品牌都能走到，自测里有 BFS 可达性断言兜着）。

---

## 键位（跟真机一致）

| 键 | 作用 |
|---|---|
| ↑ ↓ | 移动选择 |
| ← → / Tab | 切一级菜单（在子菜单里 = 返回上级） |
| Enter | 进入子菜单 / 确认 / 枚举项直接改值 |
| `+` `-` / Space / PgUp PgDn | 改值；在**引导顺序**里是上移/下移 |
| Esc | 返回上级；最外层 = 回开机选择屏 |
| F1 | 帮助（这一项到底影响什么） |
| F8 | 检查结果（逐条 ✅/❌） |
| F9 | 载入默认值 |
| F10 | 保存并退出（模拟，等于重启，回到开机选择屏） |

**打开程序时先选**：第 1 步选厂商 → 第 2 步选装机场景 → 进全屏 BIOS。
**屏上不显示按键提示条**（掌机观感：只有内容）。

---

## 记住这个顺序（真机也这么走）

1. **先定 Boot Mode / CSM** —— 决定 GPT 还是 MBR、U 盘按什么方式引导
2. **再定 Secure Boot / TPM** —— 决定能不能装 Win11、Win7 会不会被签名挡住
3. **最后定 SATA 模式** —— AHCI / RAID(RST) / IDE，**"装系统看不到硬盘"九成出在这一步**

## 最容易踩的三个坑（任务里都覆盖了）

- **Win11**：UEFI + Secure Boot **开** + TPM **2.0** 开；`SATA` 必须是 **AHCI**（开着 RST/VMD 安装器看不到盘）
- **Linux**：`SATA` 必须 **AHCI**，有 VMD 一定要关 —— 否则安装器"没有可用磁盘"
- **已装好系统改 AHCI**：**先在系统里把 AHCI 驱动启用**（改注册表 `msahci`/`iaStorV` 的 `Start=0`，
  或先进一次安全模式），**再**进 BIOS 改 —— 顺序反了就是 `INACCESSIBLE_BOOT_DEVICE` / `0x7B` 蓝屏

---

## 自测（不用开浏览器也能验）

```bash
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --headless=new --disable-gpu --no-sandbox --virtual-time-budget=3000 \
  --dump-dom "file://$PWD/biossim.html?selftest=1" | grep -o '自测.*'
```

或点页面上的「跑自测」。**35 项断言**，覆盖：
判定引擎（默认值下 Win11 必须不通过、只改错一项必须报错、RAID 下 Linux 必须提示看不到盘……）、
7 皮肤 × 51 页都能列条目、每个品牌切过去首页必须有条目、每个页名必须在该品牌菜单栏里、
`PAGES/TASKS` 引用的 key 全部存在、子菜单全部有内容、只读项改不动、枚举回绕、
**单元格宽度恒等于列宽**、**排出的行宽不超过窗口**、**品牌身份项不串味**、**只做纯黑白**、
**三种模式都有任务**（os/raid/free）、**每个品牌都能走到 RAID 子菜单（BFS 可达）**、
F10 后 dirty 归零、子菜单进出、引导顺序可移动。

**深链**（便于分享"直接看这一页"）：
`?brand=dell&task=win11&page=Security&sub=Trusted Computing&boedit=1`

---

## 排版踩过的坑（改这个文件前先读）

| 坑 | 现象 | 处置 |
|---|---|---|
| **`--font` 必须有默认值** | 初始 `#scr` 不是主题类，`var(--font)` 解析失败 → 探针量到回退字体 → 列数算错 | `:root{--font:…}` + 量字宽前先 `applyTheme()` |
| **字宽探针必须挂在 `#scr` 里** | 挂 `body` 上量到另一套字体，误差 10%+ | `measureAdvance()` 把探针 append 到 `scr` |
| **不能只信探针** | 探针 3.6px/字 vs 真渲染 3.92px/字 → 整行溢出被 `overflow:hidden` 裁掉值列 | `render()` = `paint()` + `fitGrid()`，按真实字宽多退少补收敛列数 |
| **`page` 切品牌必须一起换** | 写死 `page='Main'` → IBM/戴尔/华为整屏空白（只剩标题） | `setBrand()` 把 page 落到该品牌真实存在的菜单上 |
| **单元格宽度不能写死** | 名称列写死 26 格 > 列宽 → 值列被挤掉 | `cellParts(nm,val,w)`：`nameW+valW === w` 恒等，且留 2 格间隔 |
| **标题栏 / footer 不能从词中间截** | 出现 `Enter 确` | 标题按**词边界**截（`fitText`），footer 按**段**丢（`fitSegs`） |
| **自测别用 `outerHTML.indexOf(...)` 自查** | `<script>` 里的那串字永远命中自己 → 假失败 | 查 `getComputedStyle(...).getPropertyValue('--selbg')` |

> ⚠ 无头 Chrome 用 `--window-size=400,300` 截图时**布局视口比图大**，会得到一张**被裁切**的图
> （看起来像"值列跑到屏幕外"）。截图核对请用 `--window-size=500,375`。

---

## 素材与许可

见 `ref/SOURCES.md`。要点：
- 选项名/取值/层级来自**真机 IFR dump**（`ref/asus_aptio_v_real_dump.json`，含厂商版权，**仅本机参考，不要外发**）
- 配色数值是**事实数据**（`BIOSConfigSimulation` 无 LICENSE，故只记数值、不抄代码）；当前版本已归档不用
- 参考的其他开源项目：`BIOSHsim`(Apache-2.0)、`bios-prompt`(MIT)、`DVUEFI`、`Virtual-UEFI-Bios`、`tianocore/edk2 (OVMF)`(BSD-2)

---

## 掌机版（Lua 单文件）· 从**应用商店**下载安装

**在设备上的用法**（2026-10-02 已烧进硬件）

```
应用管家 → 商店 → 「BIOS模拟器」 → 点它 → 弹「安装 BIOS模拟器 ?」 → 确认
        → 提示「已安装到内置」→ 回「应用」分类 → 就能看到并点开
        → 想删：在商店里选中它 → 弹「卸载 BIOS模拟器?」 → 确认
```

**原理**（`components/apps/pages/am_store.c`）—— 商店不联网，"下载"就是本地复制：

| 环节 | 路径 |
|---|---|
| 商店的**源**（待装包 = "可下载"） | SPIFFS 根 `/store/*.lua` 或 TF `/sdcard/store_pkg/*.lua` |
| 点安装 = **复制**到 | `/store/apps/{cat}/{name}.lua` |
| "已安装"判定 | `/store/apps/{cat}/` **或** `/sdcard/apps/{cat}/` 存在同名文件 |
| 列表显示名 / 简介 | 取 **LAPP 头**里的 `--@app name=` / `desc=`（可中文） |

所以打包时**只放 SPIFFS 根、不预装**（预装了商店就直接显示"已安装"，就没有"下载"这一步）：

```bash
python3 tools/sync_apps.py biossim      # → apps/lua/biossim.lua (根 = /store/biossim.lua)
                                        #   自动删掉 apps/lua/apps/app/ 里的预装副本
bash merge_flash.sh                     # → dist/merged_16mb.bin（含 apps 分区）
```

`tools/sync_apps.py` 里的 `STORE_ONLY = {"biossim"}` 是"这个包走商店"的开关。
顺手还改了 `am_store.c`：原来商店列表把**所有**包的图标硬写成迷你游戏，
现按包名映射（`biossim.lua → ICON_DIAG`），未知包才回落默认。

### 品牌图标（64×64，**可替换**）

掌机上品牌选择是 **4 列 × 2 行 的 64×64 图标网格**，图标下面是品牌名。图标是**独立资源**：

```
apps/biossim/brand_icons/<品牌>.pbm     P4 二进制 1bpp, 64x64 = 512 字节
   award.pbm / ami.pbm / asus.pbm / dell.pbm / hp.pbm / ibm.pbm / huawei.pbm
```

换图标（两种都行）：

```bash
# ① 自己把图存成 64x64 的 P4 .pbm 覆盖进去
python3 tools/biossim_gen_brand_icons.py --check     # 校验规格(存在/P4/64x64/数据长度)
python3 tools/biossim_gen_lua.py                     # 图标嵌进 Lua 的 BRAND_ICON 表

# ② 给我 PNG，我转成 .pbm 并跑上面两步
```

- 生成器**默认不覆盖已存在的 .pbm**（保护你换过的图）；要重画占位图用 `--force`。
- 当前是**占位图**：统一的"显示器"外形 + 屏幕里品牌缩写（AWARD / AMI / ASUS / DELL / HP / IBM / HUAWEI），
  看着是一套；换成真实 logo 后风格自然统一。
- 没找到 `.pbm` 也不会崩：界面退化成"只有品牌名"。

**数据同一份源**：HTML 里的 `@DATA-BEGIN … @DATA-END` 是唯一数据源，
`tools/biossim_gen_lua.py` 抽取 → node 求值 → 转成 Lua 表 → **整段替换** Lua 里的
`-- @@DATA-BEGIN@@ … @@DATA-END@@`。改选项/任务只改 HTML，然后：

```bash
python3 tools/biossim_gen_lua.py          # 同步数据段（幂等）
python3 tools/biossim_gen_lua.py --check  # 只校验是否同源（可挂 CI）
```

生成时会自动：① 把固件字库没有的符号换成 ASCII（`→ ← ↑ ↓ · — °`「日本語」）；
② 把 **JS 的 0 起下标 +1 成 Lua 的 1 起下标**（不转就会 `vals[0]` 恒 nil）。

### 掌机按键优化（重点）

掌机只有 **↑↓←→ + OK + BACK 六个键**，没有 F1/F8/F9/F10、没有 ±、没有 Tab，
也没有触摸屏。所以电脑版上靠功能键做的事全部降级成**每页末尾的三行普通行**：

| 电脑版 | 掌机版 |
|---|---|
| F8 检查结果 | 每页末尾 `检查结果 / 装机结论` 行 |
| F1 帮助 | 每页末尾 `说明（当前选项 / 本页）` 行 |
| F9 载入默认 | 每页末尾 `载入默认值 Load Defaults` 行 |
| F10 保存退出 | 页面里的 `Save Changes and Exit` 行 |
| `+` `-` 改值 | **OK 直接循环到下一个值** |
| Shift `+`/`-` 调引导顺序 | 编辑态里 **← 上移 / → 下移** |
| Tab / ←→ 切页签 | ← → 切一级菜单 |
| 触摸点选 | 不需要（本机无触摸屏；代码里留了 `poll()` 兜底） |

**屏上不显示按键提示条**，只有内容；要看键位进「说明」行。
打开程序时先选一次（厂商 → 装机场景），选完记进 `nvs`，**下次直接进全屏 BIOS**，
不用再选；在 BIOS 里按 BACK 回到选择屏可以换。

### 配色：白纸黑字

反射式 1bpp 屏默认白底（`app.clear()` 清白），所以掌机版是 **黑字白底 +
选中行填黑白字** —— 电脑版 HTML 预览已改成**同一极性**，截图和设备能一一对上。

- 字模：`app.texts`（ASCII 8×12）+ `app.zhs`（汉字 12×12）→ **50 列 × 25 行**
- 本机验证不需要硬件：
  ```bash
  PY=/Users/linit/.workbuddy/binaries/python/envs/default/bin/python
  $PY tools/lua_sim.py ~/Desktop/LinTOS_Lua应用/src/biossim.lua --out /tmp/bs --scale 1 \
      --steps "render;key:5;render;key:5;render;key:2;key:2;render;key:5;render"
  ```
  `key:N` = 1↑ 2↓ 3← 4→ 5OK 6BACK 7HOME
- 细则与踩坑：技能 `lintos-lua-app-pipeline`（§5.1 nvs 签名 / §9 数据生成器 / §9.1 按键优化 / §9.2 配色）

### ⚠ 顺手修掉的一个真 bug

`tools/lua_sim.py` 原来把 `app.nvs` 写成 `get_str(ns, key)` / `set_str(ns, key, v)`（多一个 ns 参数），
而**固件**（`page_lua.c`）的命名空间是写死的 `"luaapp"`，签名是 `get_str(key)` / `set_str(key, value)`。
→ 用模拟器写出来的 app 在真机上会"永远写到 arg1 那个键上"。已修正模拟器；
历史遗留 `pomodoro.lua` 是按错的签名写的（当前不发行），恢复发行时必须改成 `pom_work_min` 这种单键。
另外 **NVS 键名上限 15 字符**（ESP-IDF 限制）。

---

## 后续可做

- `POST 自检画面`（内存计数 / 初始化提示 / `Press DEL`）—— 真机装机流程的头一段，
  参考开源 `BIOSHsim` / `bios-prompt`（见 `ref/SOURCES.md` 第 6 节）
- 常驻「Item Help」说明区（BIOSHsim 的做法，比按 F1 更利于学习）
- 真机像素级参照：`sudo xcodebuild-license accept` 后可 `brew install qemu`，
  用开源 OVMF/EDK II 跑真实 UEFI Setup 截图核对
