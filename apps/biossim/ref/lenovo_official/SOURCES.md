# 联想官方 BIOS 模拟器 —— 菜单树提取来源与完整度说明

- 目标应用：`apps/biossim`（ESP32 掌机 BIOS 模拟器）的数据源参考
- 抓取日期：**2026-10-02**
- 抓取人/方式：自动化脚本（HTTP 直接取原始 schema + 无头 Chrome/DevTools 渲染核对）
- 站点性质：联想 BIOS Simulator Center（`download.lenovo.com/bsco/`），Vue SPA + 老版 canvas 模拟器
- ⚠ 版权：素材来自联想官方公开模拟器，仅供本地对照学习，**不要外发/随固件镜像分发**

---

## 0. 关键发现：不要逆向 bundle，去取它的**数据文件**

站点渲染时会把菜单数据**单独作为文件拉下来**，这才是权威数据源（比截图/DOM 全量、精确）：

| 模拟器类型 | 数据来自 | 格式 |
|---|---|---|
| legacy 老版（canvas，拯救者） | `legacy/schemas/<机型>.ini` | InsydeH2O 自定义 ini（`$Roots`/`$MixedInteraction`/`$Content`…） |
| 新版 Vue（graphical + text） | `public/<机型>.json`（部分带 `__` 后缀） | JSON：`{"data":[{name,type,value:[…]}]}` |
| 机型总清单 | `legacy/schemas/list.conf.txt` / JS 内联 | 文本 / JS |

- legacy 引擎 `combine.js` 是 1.4MB hex 混淆（`\xNN`），**无需逆向**：它运行时用 ajax 从
  `schemas/` 拉 `.ini`（代码里可见 `"schemas/"`、`".ini"`、`"schemas/list.conf.txt"`）。
- 新版 Vue 用 `fetch('public/${name}__.json')` 加载机型数据（可见 `assets/data/list.conf.json`）。

### 已抓到的原始文件（见 `raw_sources/`）
| 文件 | 大小 | 内容 |
|---|---|---|
| `lenovo_legion_pro_7_16irx8h_82wq.ini` | 38,898 B | 拯救者 legacy schema（InsydeH2O） |
| `lenovo_k14_g3_imu_21kl_21km.json` | 87,132 B | K14 G3 graphical 数据 |
| `lenovo_v530_15arr_10y3_10v3.json` | 133,157 B | V530 text/graphical 数据 |
| `legacy_text_v530_15arr_10y3_10v3.ini` | 89,810 B | V530 另一份 legacy 文本 schema（交叉校验用，未采用） |

---

## 1. 拯救者 Lenovo Legion Pro 7 16IRX8H (82WQ) —— legacy canvas 模拟器

- **模拟器 URL**：`https://download.lenovo.com/bsco/legacy/index.html?gfile=Lenovo%20Legion%20Pro%207%2016IRX8H%20(82WQ)`
- **实际数据文件**：`https://download.lenovo.com/bsco/legacy/schemas/Lenovo%20Legion%20Pro%207%2016IRX8H%20(82WQ).ini`
- **抓取方式**：① 直接 curl 上述 `.ini`（权威数据）→ 自写解析器解析；② 用无头 Chrome + CDP
  真实渲染，逐个页签点击/拖动滚动条**截图核对**（`screenshots/legion_*.png`）。
- **顶层菜单（5）**：`Information / Configuration / Security / Boot / Exit`
- **规模**：63 个条目，55 个候选值，**0 个子菜单**
- **完整度**：**≈100%**（渲染行层面）。已可视核对 Information / Configuration / Security(含滚动到底) / Boot 四个页签，逐行一致；Exit 为标准 6 项，数据在 schema 中明确。
- **手段**：数据解析（`.ini`）+ 截图核对（canvas 无 DOM 文本，故用截图）。
- **没拿到的部分 / 说明**：
  - legacy 模拟器的行列表**只由 schema 中 `[Setup Menu <页签>]` 的 `$MixedInteraction` 列表决定**。
    schema 里还写着一批**数据段存在但实测不渲染**的段（模板共用/条件段），已列在 JSON 的
    `non_rendered_schema_sections`（33 段），例如：
    `Security/Intel SGX`、`Security/I/O Port Access`、`Information/System Summary`、
    `Boot/Boot Mode`、`Boot/Fast Boot`、`Configuration/Hotkey Mode` 等。
    **已用模拟器实测确认这些不在界面上出现**（截图 `legion_security_bottom.png` 显示 Security 到底也只有
    Secure Boot 系列，没有 Intel SGX / I/O Port Access）。
  - 取值（如 SN/CPU/内存）是模拟器的**演示占位值**，非真机。

---

## 2. Lenovo K14 G3 IMU (21KL,21KM) —— 新版 graphical 模拟器

- **模拟器 URL**：`https://download.lenovo.com/bsco/index.html#/graphicalsimulator/Lenovo%20K14%20G3%20IMU%20(21KL,21KM)`
- **实际数据文件**：`https://download.lenovo.com/bsco/public/Lenovo%20K14%20G3%20IMU%20(21KL,21KM)__.json`
  （注意有 `__` 后缀；已用 CDP 抓网络请求确认这是页面实际拉取的文件）
- **抓取方式**：① curl 原始 `.json`（权威）→ 解析；② 无头 Chrome 渲染，核对 Main 页与 Config 页（截图 `screenshots/k14_*.png`）。
- **顶层菜单（6）**：`Main / Config / Date/Time / Security / Startup / Restart`
- **规模**：20 个子菜单，101 个条目，119 个候选值
- **完整度**：**≈100%**。Main 页 17 项逐行与渲染一致；Config 页 8 个子菜单（Network/USB/Keyboard/Display/Power/Storage/CPU/Intel(R) ISM）渲染一致。
- **手段**：数据解析（`.json`）+ 浏览器渲染核对（DOM/Vue，可读 `innerText`）。
- **保留信息**：条目的 `hidden`（隐藏）/`disabled`（禁用）/`tips`（帮助文本）/`selected`（当前值）均已保留。
- **没拿到的部分**：无结构性缺失。UI 的配色/像素级版式不在此文件（本文件只做菜单数据）。

---

## 3. Lenovo V530-15ARR (10Y3,10V3) —— 新版 **text** 模拟器（最像老 BIOS）

- **模拟器 URL**：`https://download.lenovo.com/bsco/index.html#/textsimulator/Lenovo%20V530-15ARR%20(10Y3,10V3)`
- **实际数据文件**：`https://download.lenovo.com/bsco/public/Lenovo%20V530-15ARR%20(10Y3,10V3).json`
  （**无** `__` 后缀；CDP 网络抓包确认 text 模式与 graphical 模式**共用同一个 json**）
- **抓取方式**：① curl 原始 `.json`（权威）→ 解析；② 无头 Chrome 渲染 text 界面核对（截图 `screenshots/v530_text_main.png`）。
- **顶层菜单（7）**：`Main / Devices / Advanced / Power / Security / Startup / Exit`
  （蓝底页签栏，灰底蓝字——经典文本 BIOS 观感）
- **规模**：24 个子菜单，202 个条目，187 个候选值
- **完整度**：**≈100%**。Main 页与 Startup 页结构与渲染一致。
- **手段**：数据解析（`.json`）+ 浏览器渲染核对。
- **说明**：
  - 另有一份 legacy 文本 schema（`raw_sources/legacy_text_v530_15arr_10y3_10v3.ini`，
    格式 `tags=Main,Devices,…`）内容与 json 高度一致，作为**交叉校验**保留，**未采用为主源**。
  - 启动顺序子菜单（Primary/Automatic/Error/Manufacturing Boot Sequence）中的无序行，
    在 json 里是 `name` 为空、`value` 为设备名的行，已还原为设备条目。
  - 只读信息行带 `disabled:true`，已在 JSON 里标记 `disabled`。

---

## 4. 统一输出格式

```
{
  "model": "…", "source_url": "用户给的模拟器 URL", "retrieved": "2026-10-02",
  "data_source_file": "…",              // 实际拉取的数据文件（新增，便于复现）
  "non_rendered_schema_sections": […],  // 仅拯救者：schema 里存在但不渲染的段
  "menus": [
    { "name": "Config",
      "items": [
        { "name":"…", "type":"list|info|action|submenu|datetime|separator|entry|bootorder",
          "values":["…"], "current":"…", "tips":"…", "hidden":true, "disabled":true,
          "sub": { "name":"…", "items":[ … ] },
          "dialog": { "title":"…", "buttons":["Yes","No"] },
          "entries": ["…"] } ] } ]
}
```

`type` 取值含义：`list`=可选项、`info`=只读信息行、`action`=弹窗/回车动作、`submenu`=子菜单、
`datetime`=日期时间、`separator`=分隔线、`entry`=无名文本行/启动项、`bootorder`=启动顺序。

对应的 `<model>.md` 是同样内容的**缩进可读版**，可直接看结构。

---

## 5. 复现方法

```bash
# 1) 取机型数据（示例：拯救者）
curl -s "https://download.lenovo.com/bsco/legacy/schemas/Lenovo%20Legion%20Pro%207%2016IRX8H%20(82WQ).ini" -o legion.ini
#    新版：
curl -s "https://download.lenovo.com/bsco/public/Lenovo%20K14%20G3%20IMU%20(21KL,21KM)__.json" -o k14.json

# 2) 渲染核对（无头 Chrome + CDP）——脚本见会话记录，思路：
#    启动 Chrome --headless=new --remote-debugging-port=…，Page.navigate → 点击/拖动 → Page.captureScreenshot
```

> 注：legacy canvas 模拟器**不能靠 DOM 读文字**，只能用：① 直接解析 `.ini`（本仓库采用），
> 或 ② 截图人工核对。新版 Vue 模拟器是 DOM 渲染，可读 `innerText`。

---

## 6. 完整度总表

| 机型 | 模拟器 | 顶层菜单 | 子菜单 | 条目 | 候选值 | 完整度(估) | 核对方式 |
|---|---|---|---:|---:|---:|---|---|
| Legion Pro 7 16IRX8H (82WQ) | legacy(canvas) | 5 | 0 | 63 | 55 | ≈100% | ini 解析 + 4 页签截图 |
| K14 G3 IMU (21KL,21KM) | graphical | 6 | 20 | 101 | 119 | ≈100% | json 解析 + 2 页渲染核对 |
| V530-15ARR (10Y3,10V3) | text | 7 | 24 | 202 | 187 | ≈100% | json 解析 + 2 页渲染核对 |

**未拿到的部分**：三台的**菜单数据结构**均无缺失。未包含的是：UI 像素级版式/配色、
以及（仅拯救者）schema 中**确实不渲染**的 33 个模板/条件段。没有任何菜单项是编造的——
每一行都来自上述原始文件。
