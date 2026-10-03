# QQ堂 v5.2 客户端素材 —— 来源、清单与解析状态

> 来源：`~/Desktop/34129-[2020-12-03]QQTang.v5.2/QQTang.v5.2.rar`（87MB 压缩 / **134MB 解压，2020 个文件**）
> 解压位置：`/tmp/qt`（工具：`~/tmp/7z/7zz`，bsdtar 读不了 solid RAR）
> 解析工具：`tools/qqt_img.py`

---

## 1. 里面到底有什么（这是"参考它的 UI"的真正抓手）

**没有一张常规 UI 图**：全树只有 4 个 `bmp/gif`（`res/GBK12.bmp`、`res/uiRes/avt_boy.gif`、`avt_girl.gif` 等）。
UI 全部是**自有格式 `.img`（908 个）**，其中 **505 个在 `res/uiRes/`** 下：

| 目录 | 数量 | 大致内容 |
|---|---:|---|
| `res/uiRes/icon/` | 290 | 图标（含 `npack` / `pvpfunction` / `huanying` / `eye` 等子类） |
| `res/uiRes/face/` | 173 | **界面板/按钮/表情**（`btn_*`、`biaoq_*`、`biaoqingxuandan_*` …） |
| `res/uiRes/selRoom/` | 19 | 选房界面（`room` / `mapFilter` / `recorder` / `icon` 子目录） |
| `res/uiRes/gameChat/` | 8 | 局内聊天 |
| `res/uiRes/selSect/` | 7 | 选阵营 |
| `res/uiRes/number/` | 4 | 数字字模（分数/倒计时用） |
| `res/uiRes/game/` | 3 | 局内 HUD |
| `res/uiRes/shop/` | 1 | 商店 |

其余 `ui/` 目录另有 `ui/selSect/`（14）与 `ui/pawnshop/`（1）。

**更值钱的是它还带 Python 源码/字节码**（客户端是 Python 驱动的）：

| 文件 | 用途 |
|---|---|
| `res/uiRes/uiTemplate.pyc` | **UI 模板（版式定义）** |
| `res/uiRes/uiConst.pyc` | **UI 常量（位置/尺寸大概率在这里）** |
| `res/uiRes/uiLogo/uiLogin/uiMain/uiRoom/uiSelRoom/uiSelSect/uiShop/uiGame/uiWeb.pyc` | 各界面逻辑 |
| `object/player/player.py`（**明文源码**） | 角色逻辑 |
| `object/mapElem/mapElem.py`（**明文源码**） | 地图元素/道具 |
| `object/commodityCFG.py` / `itemCFG.py` / `map/mapDesc.py` | 商品/道具/地图配置 |
| `res/uiRes/levelCFG.pyc` | 关卡配置 |

> 另有 377 个 `.map`、335 个 `.eff`（特效）、132 个 `.ini`、58 `.wav` / 31 `.ogg`（音效音乐）。

---

## 2. `.img`（`DIMG`）格式 —— ✅ **已完全破解，全量导出**

magic = `QQF\x1aDIMG`，随后 **15 个 little-endian uint32**，再是像素数据。

### 定案口径

```
每帧 = [dw × dh 个 RGB565 小端]  ++  [dw × dh 个附加字节]   ← 两块**分开存**
dw, dh = f[12], f[13]          ← ★ 帧尺寸（不是 f[6]/f[7] 的画布尺寸！）
帧数    = f[2]
之后可有一小段尾部 (实测 28/56/112/140 B)，跳过即可
```

| 下标 | 含义 |
|---|---|
| f[0] | 65536 (0x00010000) 版本标记 |
| f[1] | 位深（24 = 16 位色 + 8 位附加） |
| f[2] | 帧数 |
| f[6] / f[7] | **画布**宽 / 高（常带留白） |
| f[11] | 帧数（重复） |
| f[12] / f[13] | **帧**宽 / 帧高 ← 数据按这个切 |
| f[14] | 彩色部分行字节数 = 帧宽 × 2 |

每像素字节数：实测 **875 个 3B/px**、24 个 4B/px（BGRA8888）、7 个 2B/px（纯 RGB565，无附加字节）。

### 破它的判据链（三次踩坑，每次都留了证据）

| # | 错误假设 | 症状 | 推翻它的证据 |
|---|---|---|---|
| ① | RGB888 逐行 | 雪花噪声 | 平坦色块是 `84 00 84 00…` = **RGB565 常量 0x0084**，不是乱码 |
| ② | 3 字节交错 | **严格 1 像素棋盘** | 那是"2 字节像素被跨 3 字节读"的干涉纹；且 f[14] = 帧宽×**2** |
| ③ | 用画布尺寸 f[6]/f[7] | 470 个文件"长度对不上" | 改用**帧尺寸 f[12]/f[13]** 后，`payload = 帧数 × dw × dh × 3` 严丝合缝 |

外加一个只有肉眼能发现的坑：**第 3 个字节不是显示 alpha** —— 拿它合成到白底会把整图洗成
"水印级"浅灰（形状都在、看不清）。只在拼总览图时才会暴露；长度/CRC 自洽完全查不出来。

### 导出结果（`tools/qqt_img.py extract /tmp/qt -o ... --allframes`）

```
源文件 908 个  →  导出 1415 张 PNG（每一帧一张），仅 2 个跳过
/tmp/qqt_out/
  img/        1415 张 PNG + _manifest.tsv
  audio/        89 个 wav/ogg（本来就是标准格式）
  config/      160 个 ini/xml/txt/cfg/bmp/gif
  map_src/     716 个 .map/.py（地图数据 + 明文 Python 源码）
  effect_src/  165 个 .eff（特效，格式未解）
```

**素材内容**（见 `docs/screenshots/QQ堂-素材全量总览.png`）：
地图瓦片（草地/砖墙/水面/道路/屋顶）、**表情系统 35+ 种吉祥物表情**、道具图标
（蛋糕/剑/咖啡/时钟/太阳/道具箱…）、**装备与牌组图标**（瓶子/书/卡牌，带边框）、
UI 件（▶ 播放键 / MVP / 徽章 / 会员·普通按钮 / 翻页箭头）。

### 还剩什么

- `.eff`（165 个特效）与 `.map`（377 个地图）格式**未解** —— 需要时再单独攻。
- 2 个 `.img` 头部字段不规范（已列在 manifest 的"跳过"行）。

## 3. 与掌机版的关系

掌机版 `qqtang.lua`（`~/Desktop/LinTOS_Lua应用/src/qqtang.lua`）当前用的是 **Kenney CC0** 素材
（16×16，1:1 不缩放）。本目录素材是**商业游戏《QQ堂》的原创资源**：

- 自己机器上做**个人研究/自用移植**没问题（素材本来就在你本地）；
- 但**一旦把固件或应用分发出去**，就属于再分发他人美术资源，需要你自行判断。

技术上要接入的话，路径已经清楚：`.img` → 解码 → 1bpp 化（复用
`tools/wuxia_pack.py` 里那套 `ink_of()` 合成与 `write_bmp1()` 写真 1bpp BMP 的做法）
→ 放进 `apps/lua/qqtang/` → Lua 侧用 `app.img.*` 加载。**前提是先把第 2 节的像素排列解决。**
