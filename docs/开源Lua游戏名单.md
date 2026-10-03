# 开源 Lua 游戏 · 详细名单（已下载到本地）

> 位置：`~/Desktop/开源游戏参考/` · 共 **16 个** · 整理于 2026-10-03
> 目标平台：`LinTOS 0.8` — ESP32-S3 + **400×300 单色(1bpp)** + **Lua 5.5 单文件运行时**(只有 `app.*` 桥)

## 0. 怎么判断"值不值得移植"

| 判据 | 说明 |
|---|---|
| **画风分辨率** | 决定成败。≤32px 像素画 → 能用；高清/灰度 → 二值化必糊 |
| **能不能跑起来试** | 我已装好 **LÖVE 11.5**（`~/Applications/love.app`），LÖVE 系的现在就能跑 |
| **代码语言** | **纯 Lua** 最省事（对着抄逻辑）；GDScript/C# 只能当参考 |
| **授权** | 代码多为 MIT；**素材经常是 CC BY-NC-ND（不许改）** → 只能抄结构，不能抄图 |

**适配度评分**：★★★★ 直接能用 · ★★★ 逻辑/架构可抄 · ★★ 参考价值有限 · ★ 不适用

---

## 1. LÖVE 系（纯 Lua，**现在就能跑**）

跑法：`~/Applications/love.app/Contents/MacOS/love <项目目录>`

| # | 项目 | 类型 | 规模 | 授权 | 适配度 | 说明 |
|---|---|---|---|---|---|---|
| 1 | **cavern**<br>`challacade/cavern` ⭐134 | 2D 冒险解谜 | 39M / **99 个 Lua** | 代码 MIT<br>素材 CC BY-NC-ND | ★★★ | **完整成品游戏**，代码组织是范本；有网页版可直接试 |
| 2 | **Love2D-TestGame**<br>(Earth Survival) | **生存** | 968K / 12 Lua | 见仓库 | ★★★★ | **和你需求最像**：120×68 瓦片程序化世界 + 相机跟随 + 敌人 AI(快/慢/搜索/避洞) + 地形(草/水/流沙/山/洞) + 能量系统 + 鲨鱼 |
| 3 | **fun_ascii_roguelike**<br>`Saturn91` | ASCII 地牢 | 1.1M / **61 Lua** | 见仓库 | ★★★★ | **图形全是 ASCII 字符** → 转 1bpp **天然完美**、零素材成本。含：房间生成/走廊、怪物 AI、骰子战斗、10 格背包 + 装备槽、CSV 配置驱动平衡 |
| 4 | **BYTEPATH**<br>`a327ex` ⭐1.5k | 街机射击 | 140M / **776 Lua** | 代码 MIT | ★★★ | 作者是知名 LÖVE 教程作者，**整套工程 = 教学素材**；体量大，适合挑模块抄 |
| 5 | **LoveGames**<br>`KacperSynator` | 三合一 | 7.2M / 14 Lua | **MIT** | ★★★ | 含 `TopDownShooter`（俯视射击，用 anim8 动画）+ 平台跳跃 + 打靶 |
| 6 | **Love2d**<br>`EX3mKing` | 生存高分 | 424K / **3 Lua** | 见仓库 | ★★★ | **代码极少**（main.lua + conf.lua），一晚能读完；白毛武士砍无尽怪、击杀加速刷怪 |
| 7 | **MazeKnight**<br>`Saba-Kandashvili` | 迷宫探索 | 46M / 13 Lua | CC BY-NC 4.0 | ★★★ | 程序化迷宫 + 敌人 + **玩家周围径向黑暗**；光照/视野这块可参考 |
| 8 | **l0ad3d-2d**<br>`sedexdev` | 俯视射击 | 107M / 56 Lua | **MIT** | ★★ | 波次生存 + 关底 Boss + 高分榜；主角是贴图，风格偏 PS1 |
| 9 | **shadows**<br>`matiasah` | 光照引擎 | 7.0M / 22 Lua | **MIT** | ★ | 2D 阴影/光照库 —— **黑白屏没有灰度，用不上** |

## 2. TIC-80 系（**240×136 / 16 色 / Lua**，和你硬件同一档约束）

> 价值：它的限制和你的屏几乎一样（低分辨率 + 限制色板 + Lua），**精灵/瓦片尺寸就是你该照抄的规格**。
> 跑法：装 TIC-80 客户端（tic80.com），把 `.tic` 卡带拖进去。

| # | 项目 | 类型 | 文件 | 适配度 | 说明 |
|---|---|---|---|---|---|
| 10 | **dungeon-scape-lua**<br>`luangrezende` | 地牢爬行 | 11 Lua | ★★★★ | **240×136**、模块化架构（core/entity/system/screen）、实体化设计、瓦片碰撞、精灵动画、状态机。**有浏览器版可直接跑** |
| 11 | **LiDAR-Dungeon**<br>`wojciech-graj` | 子弹地狱 Roguelike | 单文件 Lua | ★★★★ | 7 天做出来的成品；**"只能看见雷达扫过的地方"** —— 这个机制和黑白屏气质极搭 |
| 12 | **GrowQuickly**<br>`cxong` | 农场街机 | 1 Lua, **MIT** | ★★★ | 浇水/收果/赶乌鸦；有网页版 `tic80.com/play?cart=964` |

## 3. 非 Lua 引擎（只当**玩法/架构参考**，不能直接抄代码）

| # | 项目 | 引擎 | 说明 |
|---|---|---|---|
| 13 | **godot-roguelike-example**<br>`statico` | Godot 4 / GDScript | 代码 MIT，**有网页试玩**；BSP 地牢 + FOV 迷雾 + 背包装备 + **怪物行为树** + D20 战斗 + 数据驱动(CSV) |
| 14 | **westbehave**<br>`patunki` | Godot Mono / **C#** | **功能最接近 Mini DayZ**：背包/道具/合成/农业/饥渴/FSM 敌人/昼夜 |
| 15 | **Left2Dead**<br>`KaykeJordan` | Godot 4 / GDScript | 俯视僵尸射击：360° 移动/瞄准/射击/路障（僵尸会砸路障） |
| 16 | **Minecraft-Roguelike**<br>`PejperO` | **Python + PygameZero** | **最容易试**：`pip install pgzero` → `pgzrun main.py`；网格移动 + 追踪 AI + 血/无敌帧 + 计分，MIT |

---

## 4. 我建议你先看这 4 个（按"能不能直接变成你的游戏"排）

| 优先 | 项目 | 理由 |
|---|---|---|
| **1** | **`Love2D-TestGame`** (Earth Survival) | **玩法骨架和你现在的 Mini DayZ 一一对应**（程序化世界/相机/敌人 AI/地形危害）。它的**逻辑**直接搬，只把绘制换成 `app.*` |
| **2** | **`fun_ascii_roguelike`** | **ASCII 画面 = 零素材成本**，黑白屏上就是原生效果；背包装备/骰子战斗/配置驱动平衡都是现成设计 |
| **3** | **`dungeon-scape-lua`** (TIC-80) | 分辨率 240×136 与你的屏同级 → **它的精灵尺寸就是标准答案**；代码模块化，好读 |
| **4** | **`cavern`** | 完整成品，看它怎么组织一个"真正的游戏"（99 个 Lua 文件的分层） |

## 5. 关于"60px → 32px"（你的要求）

已记下：**瓦片 20px 太小 → 改成 32px**（原版 60px 缩到 32px，只缩到 0.53 倍，之前是 0.33 倍）。
影响面（未改，等你定）：
- 视野从 20×15 格变成 **12×9 格**
- 地面/建筑素材按 **0.53** 缩放（比现在的 0.33 保真得多）
- 精灵尺寸要重新定：玩家原版 32px → 在 32px 瓦片下占 **1 格**（原版是 0.53 格）

## 6. 现在就能跑的（LÖVE 已装好）

```bash
LOVE=~/Applications/love.app/Contents/MacOS/love
$LOVE ~/Desktop/开源游戏参考/Love2D-TestGame     # 生存: 程序化世界 + 敌人AI
$LOVE ~/Desktop/开源游戏参考/fun_ascii_roguelike # ASCII 地牢
$LOVE ~/Desktop/开源游戏参考/Love2d              # 武士生存(代码最短)
$LOVE ~/Desktop/开源游戏参考/LoveGames           # 三合一(含俯视射击)
$LOVE ~/Desktop/开源游戏参考/MazeKnight          # 迷宫 + 径向黑暗
```

> TIC-80 三个（dungeon-scape / LiDAR-Dungeon / GrowQuickly）需要 TIC-80 客户端；
> 也可以直接在线玩：`dungeon-scape` 有 `*.html` 浏览器版，`GrowQuickly` 在 tic80.com/play?cart=964。
