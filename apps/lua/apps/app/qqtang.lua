--@lapp v1
--@app name=QQ堂冒险
--@app cat=游戏
--@app desc=泡泡炸砖块探无限地牢: 道具/背包/技能升级/越深越难
--@app version=1
--@app build=202610300638
--@app icon=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAPwAAAA/AAAA/AAAAD8AAAD8AAAAPwAAAPwAAAA/AAAA/AAAAD8AAAD8AAAAPwAAAAPwAA/AAAAAA/AAD8AAAAAD8AAPwAAAAAPwAA/AAAAAA/AAD8AAAAAD8AAPwAAAAP//////AAAA//////8AAAD//////wAAAP//////AAAA//////8AAAD//////wAAP/wP//gf/AA//A//+B/8AD/8D//4H/wAP/wP//gf/AA//A//+B/8AD/8D//4H/wP////////////////////////////////////////////////////////////////wP//////A//A//////8D/8D//////wP/wP//////A//A//////8D/8D//////wP/wPwAAAA/A//A/AAAAD8D/8D8AAAAPwP/wPwAAAA/A//A/AAAAD8D/8D8AAAAPwPwAAP/gf/AAAAAA/+B/8AAAAAD/4H/wAAAAAP/gf/AAAAAA/+B/8AAAAAD/4H/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
-- 图标自绘: blit_app_icon(x, y[, inv=true])
-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAPwAAAA/AAAA/AAAAD8AAAD8AAAAPwAAAPwAAAA/AAAA/AAAAD8AAAD8AAAAPwAAAAPwAA/AAAAAA/AAD8AAAAAD8AAPwAAAAAPwAA/AAAAAA/AAD8AAAAAD8AAPwAAAAP//////AAAA//////8AAAD//////wAAAP//////AAAA//////8AAAD//////wAAP/wP//gf/AA//A//+B/8AD/8D//4H/wAP/wP//gf/AA//A//+B/8AD/8D//4H/wP////////////////////////////////////////////////////////////////wP//////A//A//////8D/8D//////wP/wP//////A//A//////8D/8D//////wP/wPwAAAA/A//A/AAAAD8D/8D8AAAAPwP/wPwAAAA/A//A/AAAAD8D/8D8AAAAPwPwAAP/gf/AAAAAA/+B/8AAAAAD/4H/wAAAAAP/gf/AAAAAA/+B/8AAAAAD/4H/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
local function blit_app_icon(x, y, inv)
  local d = _APPICON
  local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local buf, acc, nbits = {}, 0, 0
  for i = 1, #d do
    local ch = d:sub(i, i)
    if ch == "=" then break end
    local v = B64:find(ch, 1, true)
    if v then
      acc = acc * 64 + (v - 1); nbits = nbits + 6
      if nbits >= 8 then
        nbits = nbits - 8
        buf[#buf + 1] = math.floor(acc / (2 ^ nbits)) % 256
        acc = acc % (2 ^ nbits)
      end
    end
  end
  for row = 0, 63 do
    for col = 0, 63 do
      local idx = row * 8 + (col >> 3) + 1
      local byte = buf[idx] or 0
      local on = (math.floor(byte / (2 ^ (7 - (col % 8)))) % 2) == 1
      if on ~= (inv == true) then app.px(x + col, y + row, inv and app.WHITE or app.BLACK) end
    end
  end
end
-- ── 以上为注入前奏, 以下为应用原始代码 ──
-- ============================================================================
-- qqtang.lua — 《QQ堂》冒险模式（掌机版·单机）
--
-- 复刻对象: QQ堂 v5.2 客户端的「探险模式」(PVE 闯关)
--
-- 与原厂对齐的三层结构 (v2, 2026-10-03 重做)
--   ① 冒险大厅 —— 原厂"房间界面"的掌机化:
--        · 左栏「人物展示」= 人物头像 + 名字 + 装备四槽 (头/身/翅膀/泡泡)
--        · 右栏「探险资历」= 原厂资料面板的 7 项:
--            等级 / 探险积分 / 血量 / 糖泡数量 / 糖泡恢复速度 / 糖泡威力 / 速度
--        · 底栏 = 开始冒险 / 背包 / 锻造 / 退出
--   ② 选关 —— 12 个世界 (图标取自原版 map/bigIcon) + 该世界关卡列表 + 背景预览
--   ③ 游戏内 —— **整屏 400×300 场地** (20×15 格 @20px), 顶部状态条压在首行硬墙上
--
-- 素材 (全部原版, 见 tools/qqt_pack.py)
--   bgN.bmp    原版 pveN.img 等比放大铺满 400×300
--   tile.bmp   4 格 @20px: 0砖块(可破坏) 1地板 2硬墙 3门   ← 按原版形状程序化重绘
--   face.bmp   16 个吉祥物表情 @48px   (原版 face/faces/*.img) → 人物展示
--   item.bmp   16 个道具图标 @24px     (原版 icon/item/*.img)  → 背包
--   wicon.bmp  12 个世界图标 @40×36    (原版 map/bigIcon/*.img) → 选关
--   equip.bmp  4 个装备槽图标 @24px     (原版 icon/item/*.img)  → 装备栏
--
-- 关卡布局说明（如实标注，不假装是原版）
--   原版每关砖块布局在 `pveNN_8.map` 里, 那是带增删语义的自定义序列化(含负数), 格式未破。
--   所以这里**布局程序生成**成经典炸弹人阵型(外圈硬墙 + 偶偶坐标硬块 + 软砖 + 藏道具砖);
--   背景用原版 pveN.img, 图标/表情/道具图标也是原版 —— 所以每关的味道仍是原版的。
--
-- ⛔ 三个真机专属坑 (本地 64 位 Lua 模拟器测不出来, 都踩过):
--   ① 固件 Lua 是 32 位整数: 字面量 ≥2^31 会变浮点 → 取模/位运算直接报错。
--      本文件**不用位运算、字面量全部 < 2^31**。
--   ② `local` 只对声明之后可见; 表构造里的 local 会变全局。
--   ③ 被更早定义的函数引用的变量必须在文件顶部**前置声明**。
--
-- 退出: 大厅按返回键 → 桥层统一弹「退出程序?」(脚本 return false = 未消费)。
-- ============================================================================

local A_UP    = (type(app.UP)      == "number") and app.UP      or 1
local A_DOWN  = (type(app.DOWN)    == "number") and app.DOWN    or 2
local A_LEFT  = (type(app.LEFT)    == "number") and app.LEFT    or 3
local A_RIGHT = (type(app.RIGHT)   == "number") and app.RIGHT   or 4
local A_OK    = (type(app.CONFIRM) == "number") and app.CONFIRM or 5
local A_BACK  = (type(app.BACK)    == "number") and app.BACK    or 6
local BK, WH  = 0, 1
local W, H    = 400, 300

-- 方向 = 文件级 local (写进表里会变全局 —— 英雄坛说上踩过一次)
local DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT = 0, 1, 2, 3

-- 前置声明 (Lua 的 local 只对声明之后的代码可见)
local RES, LEVEL, P, BOMB, FLAME, MON, DROP = {}, {}, {}, {}, {}, {}, {}
local EQUIP, FORGE, UI, GAME, SAVE, MAT = {}, {}, {}, {}, {}, {}
local CFG = {}
-- ⛔ msg 必须在这里前置声明: GAME.gain_exp / GAME.start_stage 定义在它**之前**,
--    若写成 `local function msg` 后置, 那两个函数会把 msg 解析成**全局** → 运行时 nil 调用报错
--    (英雄坛说上踩过一次, 表现是"炸砖掉道具的一瞬间整个应用挂掉")。
local msg
local g_seed, tick_n = 20261003, 0

-- ============================================================================
-- CFG
-- ============================================================================
-- ★★ v5: **大世界 + 视野跟随**
--   地图 = 80×60 格 @20px = 1600×1200 px (面积是"一屏"的 16 倍), 视野只有 20×15 格。
--   用户原话: "视野跟着我人物走, 地图要非常非常大, 起码是现在的十几二十倍"。
CFG.TILE = 20
CFG.MCOLS, CFG.MROWS = 72, 54                   -- 世界大小 (格) = 1440×1080px
CFG.VCOLS, CFG.VROWS = 20, 15                   -- 视野大小 (格) = 400×300
CFG.MW, CFG.MH = CFG.MCOLS * CFG.TILE, CFG.MROWS * CFG.TILE
CFG.FX, CFG.FY = 0, 0
CFG.HUD_H = CFG.TILE                            -- 顶部状态条高度
CFG.FUSE_MS, CFG.FLAME_MS = 2400, 420
CFG.STEP_SUB, CFG.STEP_PX = 4, 4
CFG.STEP_MS = { 42, 36, 31, 27, 23 }            -- 速度档 1..5 (越小越快)
CFG.BOMB_MAXL, CFG.POW_MAXL, CFG.HP_MAX = 6, 7, 6
CFG.IF_MS, CFG.STAR_MS = 1600, 8000
CFG.EXP_BASE, CFG.DRAW_GAP_MS = 12, 16
-- 瓦片索引 = tile.bmp 里的格子号 (v4: 7 格)
CFG.T_FLOOR, CFG.T_BRICK, CFG.T_WALL, CFG.T_ITEM = 0, 1, 2, 3
CFG.T_GRASS, CFG.T_WATER, CFG.T_EXIT = 4, 5, 6

-- 12 个世界 (名称/关数取自 mapDesc.py: 6,5,4,7,5,5,4,3,3,3,3,2 = 50)
CFG.WORLDS = {
    { "魔法王国1", 6 }, { "魔法王国2", 5 }, { "机械王国", 4 }, { "黑龙王国", 7 },
    { "迷之森林", 5 }, { "迷之沙漠", 5 }, { "迷之底",   4 }, { "风暴海湾", 3 },
    { "极昼平原", 3 }, { "怒风峡谷", 3 }, { "守望山麓", 3 }, { "永冬之颠", 2 },
}

-- 材料表 (炸砖/杀怪的掉落) —— 图标槽号对应 item.bmp 的格子
MAT.LIST = {
    { k = "bubble",  name = "糖泡",   ic = 0,  desc = "泡泡数 +1" },
    { k = "powder",  name = "火药",   ic = 2,  desc = "糖泡威力 +1" },
    { k = "skate",   name = "溜冰鞋", ic = 3,  desc = "移动速度 +1" },
    { k = "potion",  name = "血瓶",   ic = 7,  desc = "血量回复满" },
    { k = "star",    name = "无敌星", ic = 8,  desc = "8 秒无敌" },
    { k = "crystal", name = "水晶",   ic = 12, desc = "锻造材料" },
}

-- 视野左上角在"世界像素坐标"里的位置。**吸附到整格**: 视野只在人物跨格时移动,
-- 这样静态层(背景+瓦片)不用每帧重画 —— 这是大地图还能跑得动的关键。
CAM = {}

-- ============================================================================
-- RNG — 确定性 (同一关种子 → 同一布局, 便于复现/以后联机)
-- ============================================================================
local function h32(a, b, c)
    -- ★★ 这里有两个**设备-only**的坑, 本地模拟器(64 位 Lua)测不出来, 只在真机上炸:
    --   ① 固件的 Lua 是 **32 位整数**(LUA_32BITS=1)。字面量 `4294967296`(2^32) 放不进 int32
    --      → 被当成**浮点** → `x % 4294967296` 把 x 提升成浮点 → 后面按位运算直接报
    --      "number has no integer representation"。
    --   ② 按位运算(~)对整数类型极敏感, 一旦上游有浮点就断。
    --   所以这里**只用 < 2^31 的字面量、且完全不用位运算**。
    a, b, c = math.floor(a or 0), math.floor(b or 0), math.floor(c or 0)
    local x = (a * 1103515245 + b * 12345 + c * 1013904223) % 2147483647
    x = (x * 48271 + 7) % 2147483647
    x = (x * 16807 + 11) % 2147483647
    return x
end
local function rnd(lo, hi)
    tick_n = tick_n + 1
    return lo + (h32(tick_n, g_seed, 7919) % (hi - lo + 1))
end
local function rndpct(p)
    tick_n = tick_n + 1
    return (h32(tick_n, g_seed, 104729) % 1000) < p
end

-- ============================================================================
-- RES — 原版素材
-- ============================================================================
RES.dir = "/store/qqtang/"
RES.BG_W, RES.BG_H = 400, 300
RES.FACE_PX, RES.ITEM_PX, RES.EQUIP_PX = 48, 24, 32
RES.WICON_W, RES.WICON_H = 40, 36
RES.FACE_N, RES.ITEM_N = 16, 16

function RES.enter()
    if RES.ready then return true end
    local f = app.fs.read(RES.dir .. "map.lua")
    if type(f) ~= "string" or #f < 40 then RES.err = "map.lua"; return false end
    local fn = load(f, "qqtang_map")
    if not fn then RES.err = "map.lua 语法"; return false end
    local ok, meta = pcall(fn)
    if not ok or type(meta) ~= "table" then RES.err = "map.lua 内容"; return false end
    RES.meta = meta
    RES.dir  = meta.dir or RES.dir
    if meta.bg_w then RES.BG_W, RES.BG_H = meta.bg_w, meta.bg_h end
    if meta.face_px  then RES.FACE_PX  = meta.face_px  end
    if meta.item_px  then RES.ITEM_PX  = meta.item_px  end
    if meta.equip_px then RES.EQUIP_PX = meta.equip_px end
    if meta.wicon_w  then RES.WICON_W, RES.WICON_H = meta.wicon_w, meta.wicon_h end
    RES.tile  = app.img.load(RES.dir .. "tile.bmp")
    RES.face  = app.img.load(RES.dir .. "face.bmp")
    RES.item  = app.img.load(RES.dir .. "item.bmp")
    RES.wicon = app.img.load(RES.dir .. "wicon.bmp")
    RES.equip = app.img.load(RES.dir .. "equip.bmp")
    RES.spr   = app.img.load(RES.dir .. "spr.bmp")
    if not RES.tile then RES.err = "tile.bmp"; return false end
    RES.ready = true
    return true
end

-- 背景懒加载: 只留当前世界的, 省 PSRAM
function RES.load_bg(n)
    if RES.bg_n == n and RES.bg_h then return true end
    if RES.bg_h then app.img.free(RES.bg_h); RES.bg_h, RES.bg_n = nil, nil end
    local h = app.img.load(RES.dir .. "bg" .. n .. ".bmp")
    if not h then return false end
    RES.bg_h, RES.bg_n = h, n
    return true
end

local function blit_tile(idx, px, py)
    if not RES.tile then return end
    app.img.blit(RES.tile, idx * CFG.TILE, 0, px, py, CFG.TILE, CFG.TILE)
end
local function blit_item(ic, x, y)
    if not RES.item then return end
    app.img.blit(RES.item, ic * RES.ITEM_PX, 0, x, y, RES.ITEM_PX, RES.ITEM_PX)
end
local function blit_equip(ic, x, y, mode)
    if not RES.equip then return end
    app.img.blit(RES.equip, ic * RES.EQUIP_PX, 0, x, y, RES.EQUIP_PX, RES.EQUIP_PX, mode or 0)
end
local function blit_wicon(i, x, y)
    if not RES.wicon then return end
    app.img.blit(RES.wicon, (i - 1) * RES.WICON_W, 0, x, y, RES.WICON_W, RES.WICON_H)
end
-- mode=1 → 位1画白 (给吉祥物表情当"黑底白脸"贴纸用)
local function blit_face(i, x, y, mode)
    if not RES.face then return end
    app.img.blit(RES.face, i * RES.FACE_PX, 0, x, y, RES.FACE_PX, RES.FACE_PX, mode or 0)
end

-- ============================================================================
-- LEVEL — 关卡地形
-- ----------------------------------------------------------------------------
-- ★ v4 重做 (用户: "我需要的是有地形有路线, 不是铺满整个屏幕的方格子, 要像探险地图一样")
--   旧版是经典炸弹人阵型(外圈墙 + 偶偶坐标硬块 + 满屏软砖) —— 在 20×15 上就是一张
--   **均匀方格纸**, 既没有地形也没有路线。
--   现在:
--     ① 噪声播种 → ② 4 轮元胞自动机 → **洞穴地形**(有岩体、有洞厅、有天然通路)
--     ③ 只保留**最大连通空腔** → 保证一定走得通
--     ④ 出生点/出口取"直径两端"(BFS 最远) → 形成一条**要走的路线**
--     ⑤ 岩体**边缘风化成可炸碎石** → 探险手感: 可以沿路线走, 也可以炸开捷径找宝
--     ⑥ 灌木/水塘点缀 → 地形层次
--   通关条件也从"清光怪"改成 **走到出口** (探险, 不是清场)。
-- ============================================================================
LEVEL.world, LEVEL.stage, LEVEL.n = 1, 1, 1
LEVEL.done = {}                     -- done[世界][关] = true (已通关)
LEVEL.exit_x, LEVEL.exit_y = 1, 1
LEVEL.spawn_x, LEVEL.spawn_y = 1, 1

function LEVEL.total_stages()
    local s = 0
    for i = 1, #CFG.WORLDS do s = s + CFG.WORLDS[i][2] end
    return s
end
function LEVEL.world_stage_of(n)
    local acc = 0
    for i = 1, #CFG.WORLDS do
        local c = CFG.WORLDS[i][2]
        if n <= acc + c then return i, n - acc, c end
        acc = acc + c
    end
    local last = #CFG.WORLDS
    return last, CFG.WORLDS[last][2], CFG.WORLDS[last][2]
end
function LEVEL.at(x, y)
    if x < 0 or y < 0 or x >= CFG.MCOLS or y >= CFG.MROWS then return CFG.T_WALL end
    return LEVEL.g[y * CFG.MCOLS + x] or CFG.T_FLOOR
end
function LEVEL.set(x, y, v)
    if x < 0 or y < 0 or x >= CFG.MCOLS or y >= CFG.MROWS then return end
    LEVEL.g[y * CFG.MCOLS + x] = v
end
function LEVEL.on_exit(x, y) return LEVEL.at(x, y) == CFG.T_EXIT end
-- 不能走的: 岩壁/碎石/宝箱碎石/灌木/深坑 (出口可以走上去)
function LEVEL.solid(x, y)
    local t = LEVEL.at(x, y)
    return t ~= CFG.T_FLOOR and t ~= CFG.T_EXIT
end
-- 能挡住爆炸的: 只有岩壁和深坑
function LEVEL.blocks_flame(x, y)
    local t = LEVEL.at(x, y)
    return t == CFG.T_WALL or t == CFG.T_WATER
end

local DD = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }
local lv_scratch = {}

local function lv_gat(G, x, y)
    if x < 0 or y < 0 or x >= CFG.MCOLS or y >= CFG.MROWS then return CFG.T_WALL end
    return G[y * CFG.MCOLS + x] or CFG.T_FLOOR
end

local function lv_rock_neighbors(G, x, y)
    local c = 0
    for dy = -1, 1 do
        for dx = -1, 1 do
            if not (dx == 0 and dy == 0) then
                if lv_gat(G, x + dx, y + dy) == CFG.T_WALL then c = c + 1 end
            end
        end
    end
    return c
end

-- 从 from 出发的 BFS 距离表 (只走地板)
local function lv_bfs(G, from)
    local dist, q = {}, { from }
    dist[from] = 0
    local h = 1
    while h <= #q do
        local c = q[h]; h = h + 1
        local cx, cy = c % CFG.MCOLS, math.floor(c / CFG.MCOLS)
        for d = 1, 4 do
            local nx, ny = cx + DD[d][1], cy + DD[d][2]
            if nx >= 1 and ny >= 1 and nx <= CFG.MCOLS - 2 and ny <= CFG.MROWS - 2 then
                local ni = ny * CFG.MCOLS + nx
                if G[ni] == CFG.T_FLOOR and dist[ni] == nil then
                    dist[ni] = dist[c] + 1
                    q[#q + 1] = ni
                end
            end
        end
    end
    return dist, q
end

-- ★ 地形生成 (v5: 凿巷式)。
--   试过元胞自动机: 在 20×15 这种小网格上只会出"一大坨岩"或"一大片空",
--   既没有通道感也没有路线感。改成**随机游走凿巷**:
--     ① 整张图先全是岩体
--     ② 从左上凿一条主通道, 之后每条通道都**从已有通道上分叉** → 天然连通
--     ③ 在通道上掏几个洞厅 → 有开阔区也有窄道 (这就是"地形")
--     ④ 空腔占比不足 52% 就继续补凿 → 每关都保证有得走, 又不会是一片白纸
--     ⑤ 岩体边缘风化成可炸碎石 → 可以沿路走, 也可以炸开捷径找宝
--   G 是"往哪张表里生成" —— 选关页的缩略图也用它(生成到临时表, 不动全局)。
function LEVEL.build(n, G)
    local wi, si, cnt = LEVEL.world_stage_of(n)
    g_seed = 1000003 * n + 77
    tick_n = 0
    local N = CFG.MCOLS * CFG.MROWS

    -- ① 全部岩体
    for i = 1, N do G[i] = CFG.T_WALL end

    local open = {}
    local nfloor = 0                              -- carve 时顺手计数 (省掉每轮重扫全图)
    local cw = 2                                  -- 通道宽度 2 格 (人物 20px, 走起来不憋)
    local function carve(x, y)
        for dy = 0, cw - 1 do
            for dx = 0, cw - 1 do
                local xx, yy = x + dx, y + dy
                if xx >= 1 and yy >= 1 and xx <= CFG.MCOLS - 2 and yy <= CFG.MROWS - 2 then
                    local i = yy * CFG.MCOLS + xx
                    if G[i] ~= CFG.T_FLOOR then
                        G[i] = CFG.T_FLOOR
                        nfloor = nfloor + 1
                    end
                    if #open < 6000 then open[#open + 1] = i end
                end
            end
        end
    end
    local function clampx(v)
        if v < 1 then return 1 end
        local hi = CFG.MCOLS - 1 - cw
        if v > hi then return hi end
        return v
    end
    local function clampy(v)
        if v < 1 then return 1 end
        local hi = CFG.MROWS - 1 - cw
        if v > hi then return hi end
        return v
    end
    local function walk(x, y, steps, turnp)
        local d = DD[rnd(1, 4)]
        for _ = 1, steps do
            if rndpct(turnp) then d = DD[rnd(1, 4)] end
            x = clampx(x + d[1]); y = clampy(y + d[2])
            carve(x, y)
        end
    end

    -- ② 主通道 + 分叉 (分叉起点取"已有通道格" → 全图必然连通)
    local x, y = rnd(2, 4), rnd(2, 4)
    carve(x, y)
    local ncorr = 10 + math.floor(wi / 2)
    local clen = 40 + si * 3
    for i = 1, ncorr do
        if i > 1 then
            local c = open[rnd(1, #open)]
            x, y = c % CFG.MCOLS, math.floor(c / CFG.MCOLS)
        end
        walk(x, y, clen, 320)
    end

    -- ②b 贯穿通道: 从已有空腔出发, 沿一个主轴"偏置游走"横穿地图。
    --     ⛔ 只有随机游走的话, 通道会挤在一角 (实测左半边全是空腔、右半边一整块岩)。
    local function walk_biased(x, y, steps, bi, turnp)
        local perp = (bi <= 2) and { 3, 4 } or { 1, 2 }
        for _ = 1, steps do
            if rndpct(turnp) then                       -- 偶尔横向错一步 (避免笔直得像木板)
                local nd = DD[perp[rnd(1, 2)]]
                carve(clampx(x + nd[1]), clampy(y + nd[2]))
            end
            local d = DD[bi]
            x = clampx(x + d[1]); y = clampy(y + d[2])
            carve(x, y)
        end
    end
    for k = 1, 5 do
        local c = open[rnd(1, #open)]
        local bi = ({ 1, 3, 1, 3, 1 })[k]               -- 右/下/右/下/右
        walk_biased(c % CFG.MCOLS, math.floor(c / CFG.MCOLS),
                    math.floor(CFG.MCOLS * 1.1), bi, 260)
    end

    -- ③ 洞厅 (开阔区)
    for _ = 1, 8 + wi do
        local c = open[rnd(1, #open)]
        local cx, cy = c % CFG.MCOLS, math.floor(c / CFG.MCOLS)
        local rw, rh = rnd(3, 6), rnd(2, 4)
        for yy = cy, cy + rh - 1 do
            for xx = cx, cx + rw - 1 do
                if xx >= 1 and yy >= 1 and xx <= CFG.MCOLS - 2 and yy <= CFG.MROWS - 2 then
                    G[yy * CFG.MCOLS + xx] = CFG.T_FLOOR
                end
            end
        end
    end

    -- ④ 密度归一化: 空腔不足 52% 继续补凿 (最多 16 次, 防呆)
    local need = math.floor((CFG.MCOLS - 2) * (CFG.MROWS - 2) * 46 / 100)
    local tries = 0
    while nfloor < need and tries < 24 do
        tries = tries + 1
        local c = open[rnd(1, #open)]
        walk(c % CFG.MCOLS, math.floor(c / CFG.MCOLS), 40, 350)
    end

    -- ④b 岩石侵蚀: 空腔过大就收回来。
    --     ⛔ 只"补凿"是不够的 —— 凿巷的交叠会把大片区域连成一片空场, 地图变成"一片白纸"
    --        (用户要的是"有地形有路线")。这一步把"岩邻 ≥5 的地板格"逐年长回岩体,
    --        几轮之后空场会被收成蜿蜒的通道与洞厅, 而且形状比手工画的更有机。
    --     ⚠ 性能: 每轮要扫全图, 所以**只数 4 邻居**(8 邻居版在真机上要好几秒, 会卡在进关)。
    local maxfloor = math.floor((CFG.MCOLS - 2) * (CFG.MROWS - 2) * 55 / 100)
    for _ = 1, 6 do
        if nfloor <= maxfloor then break end
        local cand = {}
        for yy = 1, CFG.MROWS - 2 do
            local base = yy * CFG.MCOLS
            for xx = 1, CFG.MCOLS - 2 do
                local i = base + xx
                if G[i] == CFG.T_FLOOR then
                    local rn = 0
                    if G[i - 1] == CFG.T_WALL then rn = rn + 1 end
                    if G[i + 1] == CFG.T_WALL then rn = rn + 1 end
                    if G[i - CFG.MCOLS] == CFG.T_WALL then rn = rn + 1 end
                    if G[i + CFG.MCOLS] == CFG.T_WALL then rn = rn + 1 end
                    if rn >= 3 then cand[#cand + 1] = i end
                end
            end
        end
        if #cand == 0 then break end
        for k = 1, #cand do G[cand[k]] = CFG.T_WALL end
        nfloor = nfloor - #cand
    end

    -- ⑤ 一次性把"出生点可达区"算出来 (只做 **1 次 BFS**)。
    --    ⛔ 旧版是"最大连通域扫描 + 直径两端各一次 BFS" = 4 遍全图遍历,
    --       在 72×54 上真机要 ~500ms/次 —— 选关页每换一个世界就卡半秒 (实测 511ms/帧)。
    --       现在: 从出生点 BFS 一遍 → 顺便拿到"最远可达格"当出口, 顺带把不可达的地板填回岩体。
    local spawn = open[1] or (math.floor(CFG.MROWS / 2) * CFG.MCOLS + 3)
    local seen2, q = {}, { spawn }
    local dist = { [spawn] = 0 }
    seen2[spawn] = true
    local h = 1
    local far, fd = spawn, 0
    while h <= #q do
        local c = q[h]; h = h + 1
        local dv = dist[c]
        if dv > fd then far, fd = c, dv end
        local cx, cy = c % CFG.MCOLS, math.floor(c / CFG.MCOLS)
        for d = 1, 4 do
            local nx, ny = cx + DD[d][1], cy + DD[d][2]
            if nx >= 1 and ny >= 1 and nx <= CFG.MCOLS - 2 and ny <= CFG.MROWS - 2 then
                local ni = ny * CFG.MCOLS + nx
                if G[ni] == CFG.T_FLOOR and not seen2[ni] then
                    seen2[ni] = true
                    dist[ni] = dv + 1
                    q[#q + 1] = ni
                end
            end
        end
    end
    -- 不可达的地板填回岩体 (顺带保证"出口一定走得到")
    for yy = 1, CFG.MROWS - 2 do
        for xx = 1, CFG.MCOLS - 2 do
            local i = yy * CFG.MCOLS + xx
            if G[i] == CFG.T_FLOOR and not seen2[i] then G[i] = CFG.T_WALL end
        end
    end
    LEVEL.spawn_x, LEVEL.spawn_y = spawn % CFG.MCOLS, math.floor(spawn / CFG.MCOLS)
    LEVEL.exit_x,  LEVEL.exit_y  = far % CFG.MCOLS,  math.floor(far / CFG.MCOLS)

    -- ⑦ 岩体边缘风化 → 可炸碎石
    for yy = 1, CFG.MROWS - 2 do
        for xx = 1, CFG.MCOLS - 2 do
            local i = yy * CFG.MCOLS + xx
            if G[i] == CFG.T_WALL and lv_rock_neighbors(G, xx, yy) < 8 and rndpct(620) then
                G[i] = CFG.T_BRICK
            end
        end
    end

    -- ⑧ 灌木点缀 (可炸, 轻)
    for _ = 1, (4 + wi) * 6 do
        local c = open[rnd(1, #open)]
        if c and G[c] == CFG.T_FLOOR then G[c] = CFG.T_GRASS end
    end

    -- ⑨ 水塘 (不可通行) —— 先放再验连通, 断了就撤回
    local ponds = {}
    for _ = 1, 3 + wi do
        local c = open[rnd(1, #open)]
        local cx, cy = c % CFG.MCOLS, math.floor(c / CFG.MCOLS)
        if G[c] == CFG.T_FLOOR and (cx ~= LEVEL.spawn_x or cy ~= LEVEL.spawn_y)
           and (cx ~= LEVEL.exit_x or cy ~= LEVEL.exit_y) then
            G[c] = CFG.T_WATER
            ponds[#ponds + 1] = c
        end
    end
    if #ponds > 0 then
        local dist = lv_bfs(G, LEVEL.spawn_y * CFG.MCOLS + LEVEL.spawn_x)
        if dist[LEVEL.exit_y * CFG.MCOLS + LEVEL.exit_x] == nil then
            for i = 1, #ponds do G[ponds[i]] = CFG.T_FLOOR end
        end
    end

    -- ⑩ 藏道具的碎石
    for _ = 1, 60 + wi * 6 do
        local c = open[rnd(1, #open)]
        local cx, cy = c % CFG.MCOLS, math.floor(c / CFG.MCOLS)
        for d = 1, 4 do
            local nx, ny = cx + DD[d][1], cy + DD[d][2]
            if nx >= 1 and ny >= 1 and nx <= CFG.MCOLS - 2 and ny <= CFG.MROWS - 2 then
                local ni = ny * CFG.MCOLS + nx
                if G[ni] == CFG.T_BRICK then G[ni] = CFG.T_ITEM; break end
            end
        end
    end

    -- ⑪ 出口 = 那个远端地板格换成传送门
    G[LEVEL.exit_y * CFG.MCOLS + LEVEL.exit_x] = CFG.T_EXIT
    return wi, si, cnt
end

-- 生成当前关 (写全局 LEVEL.g)
function LEVEL.gen(n)
    local t0 = app.millis()
    LEVEL.g = {}
    local wi, si, cnt = LEVEL.build(n, LEVEL.g)
    if n <= 2 or (n % 10) == 0 then
        app.log(string.format("地形生成 %d 关: %d ms", n, app.millis() - t0))
    end
    LEVEL.world, LEVEL.stage, LEVEL.wcnt = wi, si, cnt
    LEVEL.n = n
    LEVEL.is_boss = (si == cnt)
    P.x, P.y, P.mx, P.my = LEVEL.spawn_x, LEVEL.spawn_y, 0, 0
    P.moving, P.step, P.step_t = false, 0, 0
    LEVEL.cleared = false
    CAM.x, CAM.y = -1, -1                 -- 强制下一帧 CAM.follow 重算
    MON.spawn_all(wi, si, LEVEL.is_boss)
end

-- ============================================================================
-- P — 玩家
-- ============================================================================
local function player_reset(full)
    if full then
        P.lv, P.exp = 1, 0
        P.hp, P.hpMax = 3, 3
        P.bombs, P.power, P.spd = 1, 1, 1
        P.crystal = 0
        P.equip = { head = 0, body = 0, wing = 0, bubble = 0 }
        P.bag = {}
    end
    P.dir, P.frame = DIR_DOWN, 0
    P.if_t, P.star_t = 0, 0
    P.dead, P.moving, P.step, P.step_t = false, false, 0, 0
    P.mx, P.my = 0, 0
end
player_reset(true)

function P.maxbomb()
    local v = P.bombs + (P.equip.bubble or 0) // 2
    if v > CFG.BOMB_MAXL then v = CFG.BOMB_MAXL end
    return v
end
function P.pow()
    local v = P.power + P.lv // 2 + (P.equip.head or 0) // 2
    if v > CFG.POW_MAXL then v = CFG.POW_MAXL end
    return v
end
function P.speedidx()
    local v = P.spd + (P.equip.wing or 0) // 3
    if v > 5 then v = 5 end
    if v < 1 then v = 1 end
    return v
end
function P.hpmax()
    local v = P.hpMax + (P.equip.body or 0)
    if v > CFG.HP_MAX then v = CFG.HP_MAX end
    return v
end

-- ============================================================================
-- BOMB / FLAME
-- ============================================================================
function BOMB.clear() BOMB.list = {} end
BOMB.clear()
function BOMB.at(x, y)
    for i = 1, #BOMB.list do
        if BOMB.list[i].x == x and BOMB.list[i].y == y then return true end
    end
    return false
end
function BOMB.place(x, y, maxn, power)
    local n = 0
    for i = 1, #BOMB.list do
        if BOMB.list[i].owner == "P" then n = n + 1 end
    end
    if n >= maxn then return false end
    BOMB.list[#BOMB.list + 1] = { x = x, y = y, t = CFG.FUSE_MS, power = power, owner = "P" }
    return true
end

function FLAME.clear() FLAME.list = {} end
FLAME.clear()
function FLAME.at(x, y)
    for i = 1, #FLAME.list do
        if FLAME.list[i].x == x and FLAME.list[i].y == y then return true end
    end
    return false
end
function FLAME.add(x, y)
    if x < 0 or y < 0 or x >= CFG.MCOLS or y >= CFG.MROWS then return end
    if LEVEL.blocks_flame(x, y) then return end
    if FLAME.at(x, y) then return end
    FLAME.list[#FLAME.list + 1] = { x = x, y = y, t = CFG.FLAME_MS }
    -- 烧软砖 / 出道具
    local t = LEVEL.at(x, y)
    if t == CFG.T_BRICK or t == CFG.T_ITEM or t == CFG.T_GRASS then
        LEVEL.set(x, y, CFG.T_FLOOR)
        if t == CFG.T_ITEM then DROP.spawn(x, y) end
        GAME.level_dirty = true
        GAME.play_full = true          -- 静态层(瓦片)变了 → 下一帧重建
    end
end
function FLAME.boom(x, y, power)
    local i = 1
    while i <= #BOMB.list do
        local b = BOMB.list[i]
        if b.x == x and b.y == y then table.remove(BOMB.list, i) else i = i + 1 end
    end
    FLAME.add(x, y)
    local dirs = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }
    for d = 1, 4 do
        for k = 1, power do
            local nx, ny = x + dirs[d][1] * k, y + dirs[d][2] * k
            if LEVEL.blocks_flame(nx, ny) then break end
            FLAME.add(nx, ny)
            if LEVEL.at(nx, ny) == CFG.T_BRICK or LEVEL.at(nx, ny) == CFG.T_ITEM then break end
        end
    end
    -- 连锁
    for i = #BOMB.list, 1, -1 do
        local b = BOMB.list[i]
        if FLAME.at(b.x, b.y) then FLAME.boom(b.x, b.y, b.power) end
    end
end

-- ============================================================================
-- DROP — 道具/材料
-- ============================================================================
function DROP.clear() DROP.list = {} end
DROP.clear()
function DROP.at(x, y)
    for i = 1, #DROP.list do
        local d = DROP.list[i]
        if d.x == x and d.y == y then return i end
    end
    return nil
end
function DROP.spawn(x, y)
    local ic = MAT.LIST[rnd(1, #MAT.LIST)]
    DROP.list[#DROP.list + 1] = { x = x, y = y, kind = ic.k, ic = ic.ic, name = ic.name }
end
function DROP.take(kind)
    P.bag[kind] = (P.bag[kind] or 0) + 1
    if kind == "bubble" and P.bombs < CFG.BOMB_MAXL then P.bombs = P.bombs + 1 end
    if kind == "powder" and P.power < CFG.POW_MAXL then P.power = P.power + 1 end
    if kind == "skate"  and P.spd < 5 then P.spd = P.spd + 1 end
    if kind == "potion" then P.hp = P.hpmax() end
    if kind == "star"   then P.star_t = CFG.STAR_MS end
    if kind == "crystal" then P.crystal = P.crystal + 1 end
    GAME.gain_exp(3)
end

-- ============================================================================
-- MON — 怪物 (照原版说明: 不用糖泡, 各带特殊技能)
-- ============================================================================
function MON.clear() MON.list, MON.fog = {}, {} end
MON.clear()
function MON.alive()
    local n = 0
    for i = 1, #MON.list do if not MON.list[i].spawn_inv then n = n + 1 end end
    return n
end
function MON.add(kind, x, y, wi)
    local hp = 2 + wi // 2
    local mon = {
        kind = kind, x = x, y = y, hp = hp, t = 0,
        spd = ({ 1700, 2100, 2000, 1500 })[kind] or 1700,
        atk = ({ 99999, 4200, 3800, 5000 })[kind] or 99999,
        atk_t = 0, exp = 4 + wi + kind * 2,
    }
    if kind == 4 then
        mon.boss, mon.hp, mon.exp = true, 12 + wi * 3, 40 + wi * 6
        mon.spd, mon.atk, mon.atk_t = 1400, 4200, 0
    end
    MON.list[#MON.list + 1] = mon
end
-- ★ v4: 怪物**必须离出生点足够远**(曼哈顿 ≥7) —— 上一版会贴着出生点刷,
--   玩家一进关就被围殴, 表现成"忽然就提示探险失败"(用户实测反馈)。
function MON.spawn_all(wi, si, boss)
    MON.clear()
    local cnt = 6 + math.floor(wi * 1.2) + si
    if cnt > 14 then cnt = 14 end
    local sx, sy = LEVEL.spawn_x, LEVEL.spawn_y
    for _ = 1, cnt do
        for _ = 1, 120 do
            local x, y = rnd(1, CFG.MCOLS - 2), rnd(1, CFG.MROWS - 2)
            if not LEVEL.solid(x, y) and (math.abs(x - sx) + math.abs(y - sy)) >= 10 then
                MON.add((rnd(1, 10) <= 7) and 1 or rnd(2, 3), x, y, wi)
                break
            end
        end
    end
    if boss then
        for _ = 1, 120 do
            local x, y = rnd(1, CFG.MCOLS - 2), rnd(1, CFG.MROWS - 2)
            if not LEVEL.solid(x, y) and (math.abs(x - sx) + math.abs(y - sy)) >= 5 then
                MON.add(4, x, y, wi); break
            end
        end
    end
end

-- ============================================================================
-- EQUIP / FORGE — 四槽 + 七彩水晶锻造
-- ============================================================================
EQUIP.SLOTS = { "head", "body", "wing", "bubble" }
EQUIP.NAME  = { head = "头", body = "身", wing = "翅膀", bubble = "泡泡" }
EQUIP.DESC  = { head = "2档+1威力", body = "1档+1血", wing = "3档+1速", bubble = "2档+1泡" }
FORGE.COST  = { 1, 2, 3, 5, 8, 12, 18 }
FORGE.COLOR = { "赤", "橙", "黄", "绿", "青", "蓝", "紫" }

function FORGE.can(slot)
    local cur = P.equip[slot] or 0
    if cur >= 7 then return false, 0 end
    local need = FORGE.COST[cur + 1]
    return P.crystal >= need, need
end
function FORGE.do_forge(slot)
    local ok, need = FORGE.can(slot)
    if not ok then return false end
    P.crystal = P.crystal - need
    P.equip[slot] = (P.equip[slot] or 0) + 1
    app.tone_async(1180, 90, 55)
    app.vibrate(40)
    return true
end

-- ============================================================================
-- SAVE — 跨关保留
-- ============================================================================
SAVE.dirty, SAVE.t = false, 0
function SAVE.mark() SAVE.dirty = true end
function SAVE.write()
    local e = P.equip or {}
    local s = table.concat({
        P.lv, P.exp, P.hpMax, P.bombs, P.power, P.spd,
        e.head or 0, e.body or 0, e.wing or 0, e.bubble or 0,
        P.crystal, GAME.best or 1,
    }, ",")
    SAVE.dirty = false
    SAVE.t = app.millis()
    app.nvs.set_str("qtadv", s)
end
function SAVE.load()
    local s = app.nvs.get_str("qtadv")
    if type(s) ~= "string" or s == "" then return false end
    local f = {}
    for w in string.gmatch(s, "[^,]*") do f[#f + 1] = w end
    if #f < 12 then return false end
    local function gi(v, d) return math.floor(tonumber(v) or d) end
    P.lv    = math.max(1, gi(f[1], 1))
    P.exp   = math.max(0, gi(f[2], 0))
    P.hpMax = math.min(CFG.HP_MAX, math.max(3, gi(f[3], 3)))
    P.bombs = math.min(CFG.BOMB_MAXL, math.max(1, gi(f[4], 1)))
    P.power = math.min(CFG.POW_MAXL, math.max(1, gi(f[5], 1)))
    P.spd   = math.min(5, math.max(1, gi(f[6], 1)))
    P.equip = { head = math.min(7, math.max(0, gi(f[7], 0))),
                body = math.min(7, math.max(0, gi(f[8], 0))),
                wing = math.min(7, math.max(0, gi(f[9], 0))),
                bubble = math.min(7, math.max(0, gi(f[10], 0))) }
    P.crystal = math.max(0, gi(f[11], 0))
    GAME.best = math.max(1, gi(f[12], 1))
    P.hp = P.hpmax()
    return true
end
function SAVE.tick()
    if SAVE.dirty and (app.millis() - SAVE.t) > 2000 then SAVE.write() end
end

-- ============================================================================
-- UI 基础件 —— 版式统一从这里出, 不在各页各画一套
-- ============================================================================
-- ⛔ 中文宽度必须按**字符**数, 不能按 `#s` (字节数)!
--    中文一个字 3 字节 → `#"魔法王国1"` = 13 (不是 5), 拿它算居中会把整行推到屏外
--    (第一版选关页的关卡名全糊在一起就是这个原因)。
local function zw(s)
    local n, i = 0, 1
    while i <= #s do
        local b = string.byte(s, i)
        if b >= 240 then i = i + 4
        elseif b >= 224 then i = i + 3
        elseif b >= 192 then i = i + 2
        else i = i + 1 end
        n = n + 1
    end
    return n
end

-- ⚠⚠ 性能铁律 (2026-10-03 真机定位): **不要在 render 里刷"白底填充"**。
--   `GAME.render()` 开头已经 `app.clear(WH)` 把整屏置白 (那是 memset, 极快), 之后任何
--   `app.fill(..., WH)` 都只是把白再写一遍 —— 但它是逐像素 `st7305_draw_pixel`,
--   帧缓冲在 **PSRAM**, 每次都是随机读改写 (~300ns)。
--   实测: 冒险大厅右栏一个 235×239 的白框 = 5.6 万次随机写 ≈ 数十毫秒;
--   配合"每帧都重绘"直接把帧率拖到个位数 —— 抓屏看到的就是"只画了一半"的画面
--   (连续两帧墨点 9103 → 21766, 一直是半成品)。**白底一律靠 clear, 不要 fill。**
local function frame(x0, y0, x1, y1)
    app.rect(x0, y0, x1, y1, BK)
    app.rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, BK)
end

-- 画在**场景之上**的面板才需要刷白底 (把底下的场地盖掉); 画在白屏上的用 frame() 就够。
local function panel_bg(x0, y0, x1, y1)
    app.fill(x0 + 2, y0 + 2, x1 - 2, y1 - 2, WH)
    app.rect(x0, y0, x1, y1, BK)
    app.rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, BK)
end

local function title_bar(s)
    app.fill(0, 0, W - 1, 23, BK)
    app.mix(8, 3, s, true)
end

-- 行: 左侧中文标签 + 右侧数值, 选中整行反白
local function stat_row(x0, y, x1, label, value, sel)
    if sel then app.fill(x0, y - 2, x1, y + 16, BK) end
    app.mix(x0 + 4, y, label, sel and true or false)
    local w = app.mixw(value)
    app.mix(x1 - 6 - w, y, value, sel and true or false)
end

-- 血条 (方块, 不走字库 —— ■/□ 不在固件字库里, 会被画成空白格)
local function hp_bar(x, y, cur, mx, inv)
    local bw, gap = 7, 2
    for i = 1, mx do
        local bx = x + (i - 1) * (bw + gap)
        if i <= cur then
            app.fill(bx, y, bx + bw - 1, y + 9, inv and WH or BK)
        else
            app.rect(bx, y, bx + bw - 1, y + 9, inv and WH or BK)
        end
    end
    return x + mx * (bw + gap)
end

-- 底部菜单条: items 数组, sel 选中项 (1 起)
local function bottom_menu(items, sel)
    app.fill(0, H - 26, W - 1, H - 1, BK)
    local n = #items
    -- 只有一项时不要把整条刷白 (看着像一条白杠) —— 居中放一个小牌子
    if n == 1 then
        local s = items[1]
        local w = app.mixw(s)
        local x = math.floor((W - w - 24) / 2)
        app.fill(x, H - 23, x + w + 23, H - 3, WH)
        app.rect(x, H - 23, x + w + 23, H - 3, BK)
        app.mix(x + 12, H - 22, s, false)
        return
    end
    local cw = math.floor(W / n)
    for i = 1, n do
        local x = (i - 1) * cw
        local s = items[i]
        local w = app.mixw(s)
        if i == sel then
            app.fill(x + 3, H - 23, x + cw - 4, H - 3, WH)
            app.mix(x + math.floor((cw - w) / 2), H - 22, s, false)
        else
            app.mix(x + math.floor((cw - w) / 2), H - 22, s, true)
        end
    end
end

-- ============================================================================
-- UI.lobby — 冒险大厅 (人物展示 + 探险资历 + 装备 + 底栏)
-- ============================================================================
GAME.menu_items = { "开始冒险", "背包", "锻造", "退出" }

function UI.lobby()
    title_bar("QQ堂 探险模式")
    local s = "Lv" .. P.lv
    app.mix(W - 12 - app.mixw(s), 3, s, true)

    -- ---- 左栏: 人物展示 (原厂"房间"里的人物展示位) ----
    local lx0, lx1 = 6, 152
    frame(lx0, 28, lx1, 128)
    app.fill(lx0 + 2, 30, lx1 - 2, 96, BK)          -- 头像黑底
    blit_face(0, lx0 + 8, 39, 1)                    -- 表情以"白脸"贴上黑底
    app.mix(lx0 + 64, 40, "毛毛", true)
    app.mix(lx0 + 64, 62, "Lv " .. P.lv, true)
    app.mix(lx0 + 64, 84, "探险家", true)
    -- 装备四槽 (32px 图标 + 1px 框 + 槽位名): 图标是原版道具图, 但 4 个都是"物件",
    -- 光看形状分不出哪个是头/身/翅膀/泡泡 → 每个槽下面必须带 12px 中文小标签。
    app.mix(lx0, 134, "装备", false)
    local es = RES.EQUIP_PX + 2
    for i = 1, 4 do
        local ex = lx0 + (i - 1) * (es + 4)
        app.rect(ex, 150, ex + es - 1, 150 + es - 1, BK)
        blit_equip(i - 1, ex + 1, 151)
        local tier = P.equip[EQUIP.SLOTS[i]] or 0
        local nm = EQUIP.NAME[EQUIP.SLOTS[i]]
        app.zhsm(ex + math.floor((es - zw(nm) * 12) / 2), 150 + es + 2, nm, false)
        if tier > 0 then
            app.fill(ex + es - 11, 150 + es - 12, ex + es - 2, 150 + es - 3, WH)
            app.rect(ex + es - 11, 150 + es - 12, ex + es - 2, 150 + es - 3, BK)
            app.textsb(ex + es - 9, 150 + es - 12, tostring(tier), false)
        else
            app.mix(ex + es - 10, 150 + es - 16, "-", false)
        end
    end
    app.mix(lx0, 206, "水晶 " .. P.crystal, false)
    app.mix(lx0, 230, "进度 " .. (GAME.best or 1) .. "/" .. LEVEL.total_stages(), false)

    -- ---- 右栏: 探险资历 (原厂资料面板的 7 项) ----
    local rx0, rx1 = 158, W - 6
    frame(rx0, 28, rx1, 268)
    app.mix(rx0 + 6, 32, "探险资历", false)
    app.hline(rx0 + 2, rx1 - 2, 50, BK)
    local rows = {
        { "等级",         tostring(P.lv) },
        { "探险积分",     tostring(P.exp) .. "/" .. (CFG.EXP_BASE + P.lv * 6) },
        { "血量",         P.hp .. "/" .. P.hpmax() },
        { "糖泡数量",     tostring(P.maxbomb()) },
        { "糖泡恢复速度", tostring(P.speedidx()) },
        { "糖泡威力",     tostring(P.pow()) },
        { "速度",         tostring(P.speedidx()) },
    }
    local y = 50
    for i = 1, #rows do
        stat_row(rx0 + 2, y, rx1 - 2, rows[i][1], rows[i][2], false)
        y = y + 23
    end
    app.hline(rx0 + 2, rx1 - 2, y + 3, BK)
    app.mix(rx0 + 6, y + 10, "装备加成  " .. EQUIP.DESC[EQUIP.SLOTS[GAME.fg_sel]], false)
    app.mix(rx0 + 6, y + 32, "当前世界 " .. CFG.WORLDS[LEVEL.world][1] ..
            "   总 " .. LEVEL.total_stages() .. " 关", false)

    bottom_menu(GAME.menu_items, GAME.menu_sel)
end

-- ============================================================================
-- UI.world — 选关 (12 世界图标网格 + 右栏背景预览/关卡)
-- ============================================================================
-- 选关页的地形缩略图: 直接把那一关生成一遍看**路线**
-- ⚠ LEVEL.build 会写 LEVEL.spawn_x/exit_x 等全局 → 先备份, 画完还原 (否则会污染当前关)
local lv_preview = {}
local mm_key = nil
local mm_ox, mm_oy, mm_ex, mm_ey = 1, 1, 1, 1
local function draw_minimap(x, y, w, h, world, stage)
    local acc = 0
    for i = 1, world - 1 do acc = acc + CFG.WORLDS[i][2] end
    if mm_key ~= world * 100 + stage then      -- 80×60 生成一次要几十 ms, 只在换关时重算
        mm_key = world * 100 + stage
        local bsx, bsy = LEVEL.spawn_x, LEVEL.spawn_y
        local bex, bey = LEVEL.exit_x, LEVEL.exit_y
        LEVEL.build(acc + stage, lv_preview)
        mm_ox, mm_oy = LEVEL.spawn_x, LEVEL.spawn_y
        mm_ex, mm_ey = LEVEL.exit_x, LEVEL.exit_y
        LEVEL.spawn_x, LEVEL.spawn_y, LEVEL.exit_x, LEVEL.exit_y = bsx, bsy, bex, bey
    end
    local sx, sy, ex, ey = mm_ox, mm_oy, mm_ex, mm_ey

    local cs = math.floor(math.min(w / CFG.MCOLS, h / CFG.MROWS))
    if cs < 2 then cs = 2 end
    local ox = x + math.floor((w - cs * CFG.MCOLS) / 2)
    local oy = y + math.floor((h - cs * CFG.MROWS) / 2)
    app.rect(ox - 1, oy - 1, ox + cs * CFG.MCOLS, oy + cs * CFG.MROWS, BK)
    for gy = 0, CFG.MROWS - 1 do
        for gx = 0, CFG.MCOLS - 1 do
            local t = lv_preview[gy * CFG.MCOLS + gx] or CFG.T_FLOOR
            local px, py = ox + gx * cs, oy + gy * cs
            -- 墨 = 地形/阻挡, 留白 = 能走的路 → 整张缩略图**一眼看出路线**
            -- (反过来把"可走"涂黑会变成一整块黑板, 试过)
            if t ~= CFG.T_FLOOR and t ~= CFG.T_GRASS then
                app.fill(px, py, px + cs - 1, py + cs - 1, BK)
            end
        end
    end
    -- 出生点 = 实心黑点; 出口 = 空心双框 (目标一眼可见)
    app.fill(ox + sx * cs, oy + sy * cs, ox + sx * cs + cs - 1, oy + sy * cs + cs - 1, BK)
    app.fill(ox + ex * cs, oy + ey * cs, ox + ex * cs + cs - 1, oy + ey * cs + cs - 1, BK)
    app.fill(ox + ex * cs + 1, oy + ey * cs + 1, ox + ex * cs + cs - 2, oy + ey * cs + cs - 2, WH)
end

local WORLD_GRID = { cols = 3, rows = 4, cw = 68, ch = 60, x0 = 4, y0 = 30 }

function UI.world()
    title_bar("选择关卡")
    local g = WORLD_GRID
    for i = 1, #CFG.WORLDS do
        local c = (i - 1) % g.cols
        local r = math.floor((i - 1) / g.cols)
        local x, y = g.x0 + c * g.cw, g.y0 + r * g.ch
        local sel = (i == GAME.world_sel)
        if sel then
            app.rect(x - 2, y - 2, x + g.cw - 4, y + g.ch - 4, BK)
            app.rect(x - 1, y - 1, x + g.cw - 5, y + g.ch - 5, BK)   -- 2px 粗框 = 选中
        else
            app.rect(x - 2, y - 2, x + g.cw - 4, y + g.ch - 4, BK)
        end
        blit_wicon(i, x + math.floor((g.cw - 4 - RES.WICON_W) / 2), y + 1)
        local nm = CFG.WORLDS[i][1]
        local nw = zw(nm) * 12
        local nx = math.floor(x + (g.cw - 4 - nw) / 2)
        -- ⛔ 不要给名字加"黑底白字": 12px 小字反白后会糊成一片 (实测很难认)
        app.zhsm(nx, y + 40, nm, false)
    end

    -- 右栏
    local rx0 = g.x0 + g.cols * g.cw + 6
    local rx1 = W - 6
    frame(rx0, 30, rx1, H - 32)
    local wi, cnt = GAME.world_sel, CFG.WORLDS[GAME.world_sel][2]
    app.mix(rx0 + 6, 34, CFG.WORLDS[wi][1], false)
    app.mix(rx1 - 8 - app.mixw("共" .. cnt .. "关"), 34, "共" .. cnt .. "关", false)
    -- ★ 这一关的**地形缩略图** (不是背景截图!): 黑色 = 能走的路线, 白色 = 岩体
    --   ⛔ 上一版直接贴一张背景图的窗口, 用户说"右侧的简单地图特别糊特别模糊" ——
    --      因为那是抖动网点, 根本不是地图。
    local pw = rx1 - rx0 - 12
    local ph = math.floor(pw * CFG.MROWS / CFG.MCOLS)
    draw_minimap(rx0 + 6, 56, pw, ph, wi, LEVEL.stage)
    -- 关卡方块
    local by = 56 + ph + 10
    local bw = 26
    for k = 1, cnt do
        local bx = rx0 + 8 + ((k - 1) % 6) * (bw + 4)
        local byy = by + math.floor((k - 1) / 6) * (bw + 4)
        local done = LEVEL.done[wi] and LEVEL.done[wi][k]
        if done then
            app.fill(bx, byy, bx + bw - 1, byy + bw - 1, BK)
            app.mix(bx + 7, byy + 5, tostring(k), true)
        else
            app.rect(bx, byy, bx + bw - 1, byy + bw - 1, BK)
            app.mix(bx + 7, byy + 5, tostring(k), false)
        end
    end
    app.mix(rx0 + 8, by + 2 * (bw + 4) + 2, "黑=岩体  白=可走", false)

    -- 底栏
    app.fill(0, H - 26, W - 1, H - 1, BK)
    local h = "确认 开始这关     返回 大厅"
    app.mix(math.floor((W - app.mixw(h)) / 2), H - 22, h, true)
end

-- ============================================================================
-- UI.bag — 背包 (材料/道具)
-- ============================================================================
function UI.bag()
    title_bar("背包 材料")
    local cw, x0, y0 = 64, 8, 40
    for i = 1, #MAT.LIST do
        local m = MAT.LIST[i]
        local x, y = x0 + (i - 1) * cw, y0
        local own = (P.bag[m.k] or 0) + ((m.k == "crystal") and P.crystal or 0)
        local sel = (i == GAME.bag_sel)
        local ip = RES.ITEM_PX
        if sel then
            app.rect(x - 2, y - 2, x + ip + 3, y + 58, BK)
            app.rect(x - 1, y - 1, x + ip + 2, y + 57, BK)
        else
            app.rect(x - 2, y - 2, x + ip + 3, y + 58, BK)
        end
        -- 图标始终画出来 (未拥有也画, 只用数量区分), 否则背包一片问号很寒酸
        blit_item(m.ic, x, y)
        if own == 0 then
            app.rect(x, y, x + ip - 1, y + ip - 1, BK)
            blit_item(m.ic, x, y)
            app.hline(x + 2, x + ip - 3, y + 11, WH)
            app.hline(x + 2, x + ip - 3, y + 12, WH)
        end
        local nw = zw(m.name) * 12
        app.zhsm(x + math.floor((ip - nw) / 2), y + ip + 4, m.name, sel)
        local cs = "x" .. own
        app.texts(x + math.floor((ip - #cs * 8) / 2), y + ip + 19, cs, sel)
    end
    -- 说明
    local m = MAT.LIST[GAME.bag_sel]
    local by = y0 + 60 + 8
    frame(8, by, W - 8, by + 62)
    local own = (P.bag[m.k] or 0) + ((m.k == "crystal") and P.crystal or 0)
    app.mix(18, by + 8, m.name .. "   拥有 " .. own, false)
    app.mix(18, by + 32, m.desc, false)
    app.mix(W - 18 - app.mixw("炸砖 / 杀怪掉落"), by + 8, "炸砖 / 杀怪掉落", false)
    bottom_menu({ "返回大厅" }, 1)
    app.mix(300, H - 22, "上下选择", true)
end

-- ============================================================================
-- UI.forge — 锻造 (四槽 + 七彩水晶)
-- ============================================================================
function UI.forge()
    title_bar("锻造   水晶 " .. P.crystal)
    local y = 42
    for i = 1, #EQUIP.SLOTS do
        local sel = (i == GAME.fg_sel)
        local k = EQUIP.SLOTS[i]
        local cur = P.equip[k] or 0
        if sel then app.fill(6, y - 6, W - 6, y + 30, BK) end
        app.mix(14, y, EQUIP.NAME[k], sel and true or false)
        local tagn = (cur > 0) and (FORGE.COLOR[cur] .. cur .. "档") or "未锻造"
        app.mix(92, y, tagn, sel and true or false)
        local cost = (cur >= 7) and "已满紫档"
                     or ("升" .. (cur + 1) .. "   需晶 " .. FORGE.COST[cur + 1])
        app.mix(214, y, cost, sel and true or false)
        -- 选中行整行反白 → 图标必须用 mode=1(位1画白) 才不会"黑墨压黑底"看不见
        blit_equip(i - 1, W - 14 - RES.EQUIP_PX, y - 8, sel and 1 or 0)
        y = y + 46
    end
    frame(8, y + 2, W - 8, y + 58)
    app.mix(18, y + 8, "装备效果   " .. EQUIP.DESC[EQUIP.SLOTS[GAME.fg_sel]], false)
    app.mix(18, y + 32, "水晶按赤橙黄绿青蓝紫 7 档逐级锻造", false)
    bottom_menu({ "返回大厅" }, 1)
    app.mix(W - 6 - app.mixw("确认 升级  上下 选择"), H - 22, "确认 升级  上下 选择", true)
end

-- ============================================================================
-- UI.play — 游戏内 (整屏场地 + 顶部状态条)
-- ============================================================================
function UI.hud()
    -- 顶部状态条压在首行硬墙上 —— 该行本来就是墙, 盖住不损失任何可玩区域
    app.fill(0, 0, W - 1, CFG.HUD_H - 1, BK)
    local x = 6
    app.mix(x, 2, "Lv" .. P.lv, true)
    x = x + app.mixw("Lv" .. P.lv) + 6
    x = hp_bar(x, 5, P.hp, P.hpmax(), true)
    x = x + 8
    app.mix(x, 2, "泡" .. P.maxbomb() .. " 威" .. P.pow(), true)
    -- 探险要"知道往哪走": 给出到出口的步数 (曼哈顿距离, 每秒刷一次就够)
    local r = "出口 " .. (math.abs(LEVEL.exit_x - P.x) + math.abs(LEVEL.exit_y - P.y))
              .. "  余怪 " .. MON.alive()
    app.mix(W - 6 - app.mixw(r), 2, r, true)
end

-- ============================================================================
-- 场地绘制 —— 大世界 + 相机 + 静态/动态分层
-- ----------------------------------------------------------------------------
-- ★★ v5 三件事叠在一起:
--   ① **大世界**: MAP 80×60 格 = 1600×1200 px (面积是"一屏"的 16 倍)。
--   ② **相机跟随**: 视野 20×15 格, 屏幕坐标一律 = `世界格*TILE - CAM`。
--      ⛔ CAM 必须**吸附到整格** —— 否则每移动 1 像素静态层就要重画一次 (会掉到个位数帧率)。
--         吸附之后"视野只在人物跨格时整格一跳", 静态层每跨一格才重建一次 (约 150ms 一次)。
--   ③ **静态/动态分层** 照旧: 静态层(全部瓦片)按需重建; 动态层每帧只擦旧格+画新格。
--
-- ⛔ 性能铁律 (真机定量): 帧缓冲在 PSRAM, 每次落笔都是随机读改写(≈100 周期),
--   **"画了多少像素"就是帧耗时**。所以: render 里不刷白底(clear 一次就够)、
--   静态层只在需要时重建、脏标记两道闸门分开写。
-- ============================================================================
local SPX = 24                                   -- spr.bmp 单帧尺寸
RES.SPX = SPX

-- ★ 瓦片变体: 同一种地形给 3~5 个外观, 按格子哈希挑。
--   为什么必须"确定性哈希"而不是随机: 静态层会被重复重建(换关/开砖/相机跨格),
--   随机的话同一格每次重建都换个样子 → 满屏闪。
--   ⚠ 哈希只用小整数运算: 32 位 Lua 里 x*73856093 这种大乘法会变浮点。
local DISP = {
    [0] = { 0, 7, 8, 9, 10 },        -- 地板: 素面 / 碎石粒 / 草簇 / 裂纹 / 石板缝
    [1] = { 1 },                      -- 碎石
    [2] = { 2, 11, 12 },              -- 岩壁: 十字 / 斜纹 / 实心核心
    [3] = { 3 },                      -- 宝箱碎石
    [4] = { 4 },                      -- 灌木
    [5] = { 5 },                      -- 深坑
    [6] = { 6 },                      -- 出口
}
local function disp_tile(t, x, y)
    local v = DISP[t]
    if not v then return t end
    local n = #v
    if n == 1 then return v[1] end
    local h = (x * 7 + y * 13 + (x * y) % 11) % 997
    return v[h % n + 1]
end

local function blit_tile(t, sx, sy)              -- t == tile.bmp 图集格号
    if not RES.tile then return end
    app.img.blit(RES.tile, t * CFG.TILE, 0, sx, sy, CFG.TILE, CFG.TILE)
end

-- 精灵: 从 24px 帧里取正中 w×w, 并把 w×w **居中**放进 20px 的格里。
--   ⛔ 怪兽必须比格子小一圈 (18px): 20px 的怪挨在一起会连成一整块黑 (实测像"一坨糊"),
--      留 1px 缝之后才看得出是"好几个怪"。
local function blit_spr(idx, gx, gy, w)
    if not RES.spr then return end
    local so = (SPX - w) // 2            -- 源帧内裁剪偏移
    local dc = (CFG.TILE - w) // 2       -- 目标格内居中偏移 (⛔ 别叫 do: 保留字)
    app.img.blit(RES.spr, idx * SPX + so, so, gx + dc, gy + dc, w, w)
end

-- 相机: 始终把人物放在视野中心 (到地图边界就贴边), 并吸附到整格
function CAM.follow()
    local tx = P.x * CFG.TILE + CFG.TILE // 2 - W // 2
    local ty = P.y * CFG.TILE + CFG.TILE // 2 - H // 2
    if tx < 0 then tx = 0 elseif tx > CFG.MW - W then tx = CFG.MW - W end
    if ty < 0 then ty = 0 elseif ty > CFG.MH - H then ty = CFG.MH - H end
    tx = (tx // CFG.TILE) * CFG.TILE
    ty = (ty // CFG.TILE) * CFG.TILE
    if tx ~= CAM.x or ty ~= CAM.y then
        CAM.x, CAM.y = tx, ty
        return true
    end
    return false
end

-- 整片可见区域 (背景 + 全部瓦片) —— 只在换关 / 开砖 / 相机跨格时重画
function GAME.draw_static()
    local c0 = CAM.x // CFG.TILE
    local r0 = CAM.y // CFG.TILE
    for r = r0, r0 + CFG.VROWS - 1 do
        for c = c0, c0 + CFG.VCOLS - 1 do
            blit_tile(disp_tile(LEVEL.at(c, r), c, r), c * CFG.TILE - CAM.x, r * CFG.TILE - CAM.y)
        end
    end
    GAME.dn_prev = 0
end

-- 把一格擦回"静态层"的样子 (参数是世界格坐标)
local function restore_cell(cx, cy)
    if cx < 0 or cy < 0 or cx >= CFG.MCOLS or cy >= CFG.MROWS then return end
    local sx, sy = cx * CFG.TILE - CAM.x, cy * CFG.TILE - CAM.y
    if sx <= -CFG.TILE or sy <= -CFG.TILE or sx >= W or sy >= H then return end
    -- ⛔ 视野首行永远被"顶部状态条"压着 (相机是整格吸附的, 所以 sy 必是 20 的倍数):
    --   擦这一行会把状态条盖花 —— 一旦真的发生, 必须让状态条下一帧重画。
    if sy < CFG.HUD_H then
        GAME.hud_key = nil
        return
    end
    blit_tile(disp_tile(LEVEL.at(cx, cy), cx, cy), sx, sy)
end

-- 标记"这一格被动态物体占用" → 下一帧要擦 (世界格坐标!)
local function dcell(x, y)
    local n = GAME.dn + 1
    if n > 2400 then return end
    GAME.dn = n
    GAME.dyn_cur[n] = y * 1000 + x
end

local function draw_field(cx, cy, ox, oy, pdir)
    -- 1) 擦掉上一帧的动态格
    local pv, pn = GAME.dyn_prev, GAME.dn_prev or 0
    for i = 1, pn do
        local c = pv[i]
        restore_cell(c % 1000, math.floor(c / 1000))
    end
    -- 2) 换表
    local tmp = GAME.dyn_cur
    GAME.dyn_cur = GAME.dyn_prev
    GAME.dyn_prev = tmp
    GAME.dn_prev = GAME.dn
    GAME.dn = 0

    local function vis(sx, sy)
        return sx > -CFG.TILE and sy > -CFG.TILE and sx < W and sy < H
    end

    -- 雾
    for i = 1, #MON.fog do
        local f = MON.fog[i]
        dcell(f.x, f.y)
        local sx, sy = f.x * CFG.TILE - CAM.x + 4, f.y * CFG.TILE - CAM.y + 4
        if vis(sx, sy) then
            for k = 0, 5 do
                app.hline(sx + k, sx + 11 - k, sy + k, BK)
                app.hline(sx + k, sx + 11 - k, sy + 11 - k, BK)
            end
        end
    end
    -- 道具
    for i = 1, #DROP.list do
        local d = DROP.list[i]
        dcell(d.x, d.y)
        local sx, sy = d.x * CFG.TILE - CAM.x, d.y * CFG.TILE - CAM.y
        if vis(sx, sy) then
            app.fill(sx + 1, sy + 1, sx + CFG.TILE - 2, sy + CFG.TILE - 2, WH)
            app.rect(sx + 1, sy + 1, sx + CFG.TILE - 2, sy + CFG.TILE - 2, BK)
            local ip = RES.ITEM_PX
            app.img.blit(RES.item, d.ic * ip + 4, 4, sx + 2, sy + 2, 16, 16)
        end
    end
    -- 火焰
    for i = 1, #FLAME.list do
        local f = FLAME.list[i]
        dcell(f.x, f.y)
        local sx, sy = f.x * CFG.TILE - CAM.x, f.y * CFG.TILE - CAM.y
        if vis(sx, sy) then
            app.fill(sx, sy, sx + CFG.TILE - 1, sy + CFG.TILE - 1, BK)
            app.fill(sx + 8, sy + 3, sx + 11, sy + CFG.TILE - 4, WH)
            app.fill(sx + 3, sy + 8, sx + CFG.TILE - 4, sy + 11, WH)
            app.fill(sx + 6, sy + 6, sx + CFG.TILE - 7, sy + CFG.TILE - 7, WH)
        end
    end
    -- 怪物 (spr 8..11)
    for i = 1, #MON.list do
        local m = MON.list[i]
        dcell(m.x, m.y)
        local sx, sy = m.x * CFG.TILE - CAM.x, m.y * CFG.TILE - CAM.y
        if vis(sx, sy) then
            local k = m.kind
            if k < 1 or k > 4 then k = 1 end
            blit_spr(8 + k - 1, sx, sy, CFG.TILE - 4)   -- 16px: 相邻怪之间留 4px 缝, 不会连成一片黑
        end
    end
    -- 人物 (spr 0..7, 索引 = 方向*2 + 走路帧)
    if not P.dead then
        -- ⛔ 标记的必须是**格坐标**; 24px 精灵会压到 3×3 邻格, 全都要标,
        --   否则移动时邻格擦不干净 → 拖影 (上一版就是这里错的, 用户实测"好几个重影")
        dcell(cx, cy)
        dcell(cx - 1, cy - 1); dcell(cx, cy - 1); dcell(cx + 1, cy - 1)
        dcell(cx - 1, cy);                       dcell(cx + 1, cy)
        dcell(cx - 1, cy + 1); dcell(cx, cy + 1); dcell(cx + 1, cy + 1)
        local gx = cx * CFG.TILE - CAM.x
        local gy = cy * CFG.TILE - CAM.y
        local sx, sy = gx + ox - 2, gy + oy - 2
        local blink = (P.if_t > 0) and (math.floor(app.millis() / 100) % 2 == 0)
        if not blink and vis(sx, sy) then
            local fr = (P.frame or 0) % 2
            blit_spr(pdir * 2 + fr, sx, sy, SPX)
        end
    end
    -- 泡泡画在人物**之后** (原版泡泡会盖住人)
    for i = 1, #BOMB.list do
        local b = BOMB.list[i]
        dcell(b.x, b.y)
        local sx, sy = b.x * CFG.TILE - CAM.x, b.y * CFG.TILE - CAM.y
        if vis(sx, sy) then
            local r = 8
            if b.t < 700 and (math.floor(b.t / 120) % 2 == 0) then r = 5 end
            app.fill(sx + 10 - r, sy + 10 - r, sx + 9 + r, sy + 9 + r, BK)
            app.fill(sx + 6, sy + 6, sx + CFG.TILE - 7, sy + CFG.TILE - 7, WH)
            app.px(sx + 8, sy + 8, BK)
        end
    end
end

-- 游戏内顶部横条菜单 (返回键呼出)
function UI.playmenu(sel)
    local items = GAME.play_items
    local pad, aw = 5, 13
    local widths, total = {}, pad * 2
    for i = 1, #items do
        widths[i] = aw + app.mixw(items[i]) + 4
        total = total + widths[i]
    end
    if total > W then total = W end
    local x0 = math.floor((W - total) / 2)
    app.fill(x0, 0, x0 + total, 21, WH)
    app.rect(x0, 0, x0 + total, 21, BK)
    local x = x0 + pad
    for i = 1, #items do
        if i == sel then app.fill(x + 1, 4, x + 10, 17, BK) end
        app.mix(x + aw, 2, items[i], false)
        x = x + widths[i]
        if x > x0 + total - 4 then break end
    end
end

-- 结算
local function settle_panel()
    local wn = CFG.WORLDS[LEVEL.world][1]
    local pw, ph = 340, 190
    local px, py = math.floor((W - pw) / 2), math.floor((H - ph) / 2)
    panel_bg(px, py, px + pw - 1, py + ph - 1)
    app.fill(px + 2, py + 2, px + pw - 3, py + 26, BK)
    app.mix(px + 8, py + 5, "本关结算", true)
    local rows = {
        "通关 " .. wn .. " 第" .. LEVEL.stage .. "关",
        "Lv" .. P.lv .. "   水晶 " .. P.crystal,
        "血量 " .. P.hp .. "/" .. P.hpmax() .. "   余怪 " .. MON.alive(),
        "泡泡 " .. P.maxbomb() .. "   威力 " .. P.pow() .. "   速度 " .. P.speedidx(),
    }
    local y = py + 40
    for i = 1, #rows do app.mix(px + 16, y, rows[i], false); y = y + 28 end
    app.fill(px + pw - 62, py + 40, px + pw - 14, py + 88, BK)
    blit_face(0, px + pw - 62, py + 40, 1)
    app.mix(px + 16, py + ph - 26, "确认=下一关   返回=大厅", false)
end

local function over_panel()
    local pw, ph = 320, 170
    local px, py = math.floor((W - pw) / 2), math.floor((H - ph) / 2)
    panel_bg(px, py, px + pw - 1, py + ph - 1)
    app.fill(px + 2, py + 2, px + pw - 3, py + 26, BK)
    app.mix(px + 8, py + 5, "探险失败", true)
    local rows = {
        "你被击倒了",
        "Lv" .. P.lv .. "   水晶 " .. P.crystal,
        "最高到 第" .. (GAME.best or 1) .. " 关",
    }
    local y = py + 40
    for i = 1, #rows do app.mix(px + 16, y, rows[i], false); y = y + 30 end
    app.mix(px + 16, py + ph - 26, "确认=重来本关   返回=大厅", false)
end

-- ============================================================================
-- GAME — 状态机
-- play / menu / lobby / world / bag / forge / settle / over
-- ============================================================================
GAME.state = "lobby"
GAME.dirty, GAME.last_draw = true, 0
-- 增量重绘的状态 (见"场地绘制 静态层/动态层分离"那节):
GAME.page_dirty     = true   -- 菜单页需要重画
GAME.play_full      = true   -- 场地需要整屏重建 (进关/开砖/状态切换)
GAME.last_state     = nil    -- 用来捕捉状态切换
GAME.hud_key        = nil    -- 顶部状态条缓存键 (数值没变就不重画)
GAME.msg_box        = nil    -- 当前消息条占据的矩形 (消失时要擦掉)
GAME.dn, GAME.dn_prev = 0, 0 -- 动态格计数
GAME.dyn_prev, GAME.dyn_cur = {}, {}
GAME.menu_sel, GAME.bag_sel, GAME.fg_sel, GAME.world_sel = 1, 1, 1, 1
GAME.play_items = { "继续", "背包", "锻造", "重开", "退出" }
GAME.play_sel = 1
GAME.best, GAME.msg, GAME.msg_t = 1, nil, 0
GAME.level_dirty = true

function GAME.gain_exp(v)
    P.exp = P.exp + v
    local need = CFG.EXP_BASE + P.lv * 6
    while P.exp >= need do
        P.exp = P.exp - need
        P.lv = P.lv + 1
        if P.lv % 2 == 0 and P.power < CFG.POW_MAXL then P.power = P.power + 1 end
        if P.hpMax < CFG.HP_MAX then P.hpMax = P.hpMax + 1 end
        P.hp = P.hpmax()
        msg("升级 Lv" .. P.lv)
        app.tone_async(1000, 90, 55)
        need = CFG.EXP_BASE + P.lv * 6
    end
    SAVE.mark()
end

function GAME.start_stage(n)
    LEVEL.gen(n)
    BOMB.clear()
    FLAME.clear()
    DROP.clear()
    RES.load_bg(LEVEL.world)
    P.if_t, P.star_t = 1200, 0
    P.moving, P.step, P.step_t, P.mx, P.my = false, 0, 0, 0, 0
    P.dead = false
    if P.hp <= 0 then P.hp = P.hpmax() end
    if n > (GAME.best or 1) then GAME.best = n end
    GAME.state = "play"
    GAME.dirty, GAME.level_dirty = true, true
    GAME.play_full = true
    local wn = CFG.WORLDS[LEVEL.world][1]
    msg(wn .. " 第" .. LEVEL.stage .. "关")
end

local function try_move(dir)
    P.dir = dir
    local dx = (dir == DIR_RIGHT) and 1 or ((dir == DIR_LEFT) and -1 or 0)
    local dy = (dir == DIR_DOWN) and 1 or ((dir == DIR_UP) and -1 or 0)
    local nx, ny = P.x + dx, P.y + dy
    P.moving, P.step, P.step_t = true, 0, 0
    if LEVEL.solid(nx, ny) or BOMB.at(nx, ny) then
        P.tx, P.ty = P.x, P.y                     -- 撞墙/泡泡: 原地转身
    else
        P.tx, P.ty = nx, ny
    end
end

local function hurt_player(ms)
    if P.if_t > 0 or P.star_t > 0 or P.dead then return end
    P.hp = P.hp - 1
    P.if_t = ms or CFG.IF_MS
    app.vibrate(45)
    app.tone_async(200, 90, 55)
    if P.hp <= 0 then
        P.hp = 0
        P.dead = true
        GAME.state = "over"
        GAME.dirty = true
        SAVE.write()
    end
end

function GAME.step(dt)
    local moved = false
    if P.if_t > 0 then P.if_t = P.if_t - dt end
    if P.star_t > 0 then P.star_t = P.star_t - dt end

    -- 玩家移动
    if P.moving then
        local inplace = (P.x == P.tx and P.y == P.ty)
        local sms = CFG.STEP_MS[P.speedidx()]
        P.step_t = P.step_t + dt
        while P.step_t >= sms and P.moving do
            P.step_t = P.step_t - sms
            P.step = P.step + 1
            local seq = { 1, 2, 3, 0 }
            P.frame = seq[P.step] or 0
            if not inplace then
                local dx = (P.dir == DIR_RIGHT) and 1 or ((P.dir == DIR_LEFT) and -1 or 0)
                local dy = (P.dir == DIR_DOWN) and 1 or ((P.dir == DIR_UP) and -1 or 0)
                P.mx = P.mx + dx * CFG.STEP_PX
                P.my = P.my + dy * CFG.STEP_PX
            end
            if P.step >= CFG.STEP_SUB then
                P.moving, P.step = false, 0
                P.x, P.y = P.tx, P.ty
                P.mx, P.my = 0, 0
                moved = true
            end
        end
    elseif not P.dead then
        local dir = nil
        if app.key_down(A_LEFT) then dir = DIR_LEFT
        elseif app.key_down(A_RIGHT) then dir = DIR_RIGHT
        elseif app.key_down(A_UP) then dir = DIR_UP
        elseif app.key_down(A_DOWN) then dir = DIR_DOWN end
        if dir then try_move(dir); moved = true end
    end

    -- 吃道具
    local di = DROP.at(P.x, P.y)
    if di then
        local d = DROP.list[di]
        DROP.take(d.kind)
        msg("吃到 " .. d.name)
        app.tone_async(880, 60, 40)
        table.remove(DROP.list, di)
        SAVE.mark()
    end

    -- 引信
    for i = #BOMB.list, 1, -1 do
        local b = BOMB.list[i]
        b.t = b.t - dt
        if b.t <= 0 then FLAME.boom(b.x, b.y, b.power) end
    end

    -- 火焰: 烧怪 + 烧玩家
    for i = #FLAME.list, 1, -1 do
        local f = FLAME.list[i]
        f.t = f.t - dt
        for j = #MON.list, 1, -1 do
            local m = MON.list[j]
            if m.x == f.x and m.y == f.y then
                m.hp = m.hp - 2
                if m.hp <= 0 then
                    GAME.gain_exp(m.exp)
                    msg("击败" .. (m.boss and "BOSS" or "怪") .. " +" .. m.exp)
                    table.remove(MON.list, j)
                end
            end
        end
        if P.x == f.x and P.y == f.y then hurt_player() end
        if f.t <= 0 then table.remove(FLAME.list, i) end
    end

    -- 怪物
    for i = #MON.list, 1, -1 do
        local m = MON.list[i]
        m.t = m.t + dt
        if m.t >= m.spd then
            m.t = 0
            local dirs = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }
            local chase = (m.kind == 1) or (m.boss and rndpct(700)) or (m.kind == 3 and rndpct(400))
            local dx, dy = 0, 0
            if chase then
                if math.abs(P.x - m.x) >= math.abs(P.y - m.y) then
                    dx = (P.x > m.x) and 1 or -1
                else
                    dy = (P.y > m.y) and 1 or -1
                end
            else
                local d = dirs[rnd(1, 4)]
                dx, dy = d[1], d[2]
            end
            local nx, ny = m.x + dx, m.y + dy
            if not LEVEL.solid(nx, ny) and not BOMB.at(nx, ny) then
                m.x, m.y = nx, ny
            elseif not chase then
                local d = dirs[rnd(1, 4)]
                nx, ny = m.x + d[1], m.y + d[2]
                if not LEVEL.solid(nx, ny) and not BOMB.at(nx, ny) then m.x, m.y = nx, ny end
            end
        end
        -- 元素师: 火球 → 落点毒雾
        if m.kind == 2 then
            m.atk_t = m.atk_t + dt
            if m.atk_t >= m.atk then
                m.atk_t = 0
                if not LEVEL.solid(P.x, P.y) then
                    MON.fog[#MON.fog + 1] = { x = P.x, y = P.y, t = 2200 }
                    msg("毒雾")
                end
            end
        end
        -- 引爆者: 引爆玩家一个泡泡
        if m.kind == 3 then
            m.atk_t = m.atk_t + dt
            if m.atk_t >= m.atk and #BOMB.list > 0 then
                m.atk_t = 0
                local b = BOMB.list[rnd(1, #BOMB.list)]
                if b then FLAME.boom(b.x, b.y, b.power) end
            end
        end
        -- BOSS: 召唤
        if m.boss then
            m.atk_t = m.atk_t + dt
            if m.atk_t >= m.atk and #MON.list < 14 then
                m.atk_t = 0
                for _ = 1, 20 do
                    local x, y = rnd(2, CFG.MCOLS - 2), rnd(1, CFG.MROWS - 2)
                    if not LEVEL.solid(x, y) and (math.abs(x - P.x) + math.abs(y - P.y)) > 3 then
                        MON.add(1, x, y, LEVEL.world)
                        break
                    end
                end
                msg("BOSS 召唤")
            end
        end
        if m.x == P.x and m.y == P.y then hurt_player() end
    end
    -- 毒雾
    for i = #MON.fog, 1, -1 do
        local f = MON.fog[i]
        f.t = f.t - dt
        if f.x == P.x and f.y == P.y then hurt_player(CFG.IF_MS + 400) end
        if f.t <= 0 then table.remove(MON.fog, i) end
    end

    -- 通关: **走到出口** (探险玩法 —— 不是"清光怪", 怪可以绕开/炸开)
    if GAME.state == "play" and LEVEL.on_exit(P.x, P.y) and not LEVEL.cleared then
        LEVEL.cleared = true
        LEVEL.done[LEVEL.world] = LEVEL.done[LEVEL.world] or {}
        LEVEL.done[LEVEL.world][LEVEL.stage] = true
        GAME.state = "settle"
        app.tone_async(1200, 120, 60)
        SAVE.write()
    end

    SAVE.tick()
    if moved or P.moving then GAME.dirty = true end
end

-- ============================================================================
-- 渲染
-- ============================================================================
-- HUD 只在数值变化时重画 (它下面那行硬墙是静态的, 画一次就够)
local function hud_cached()
    local key = P.hp .. "|" .. P.hpmax() .. "|" .. P.lv .. "|" ..
                P.maxbomb() .. "|" .. P.pow() .. "|" .. MON.alive() .. "|" ..
                (app.millis() // 500)
    if GAME.hud_key == key then return end
    GAME.hud_key = key
    UI.hud()
end

-- 把一块矩形区域擦回静态层 (给"消息条"这种浮层用)
local function restore_rect(x0, y0, x1, y1)
    local c0 = math.max(0, math.floor(x0 / CFG.TILE))
    local c1 = math.min(CFG.MCOLS - 1, math.floor(x1 / CFG.TILE))
    local r0 = math.max(0, math.floor(y0 / CFG.TILE))
    local r1 = math.min(CFG.MROWS - 1, math.floor(y1 / CFG.TILE))
    for r = r0, r1 do
        for c = c0, c1 do restore_cell(c, r) end
    end
end

function GAME.render()
    local now = app.millis()
    -- ★★ 这里是**整帧率杀手**, 从旧版继承过来的写法害了很久:
    --     `if not dirty and (now - last) < GAP then return end`
    --     只有"既不脏、又刚画完"才跳过 ⇒ **只要过了 16ms 就无条件重绘**。
    --   桥层每帧都会 needs_redraw, 于是 render 每帧全量重画一次; 真机上一帧要几十毫秒,
    --   结果: 帧率个位数 + 抓屏永远抓到"画到一半"的画面 (一度被误判成"右栏没画出来")。
    --   正确语义 = 两道闸门**分开**: 不脏就跳过; 脏但离上次太近也跳过。
    if not GAME.dirty then return end
    if (now - GAME.last_draw) < CFG.DRAW_GAP_MS then return end
    GAME.last_draw = now
    GAME.dirty = false

    local st = GAME.state
    if GAME.last_state ~= st then          -- 状态切换 = 整屏重建 (菜单开合/进出场地)
        GAME.last_state = st
        GAME.page_dirty = true
        GAME.play_full = true
    end
    local inplay = (st == "play" or st == "menu" or st == "settle" or st == "over")

    -- ---------- 菜单页: 只在进页/按键时重画一次 ----------
    if not inplay then
        if GAME.page_dirty then
            GAME.page_dirty = false
            GAME.play_full = true          -- 下次进场地整屏重建
            app.clear(WH)
            if st == "lobby" then UI.lobby()
            elseif st == "world" then UI.world()
            elseif st == "bag" then UI.bag()
            else UI.forge() end
        end
        return
    end

    -- ---------- 游戏内 ----------
    -- 相机跟随 (整格吸附): 只在**跨格**时返回 true → 静态层才需要重建
    if CAM.follow() then GAME.play_full = true end
    if GAME.play_full then
        GAME.play_full = false
        GAME.hud_key = nil
        GAME.msg_box = nil
        app.clear(WH)
        GAME.draw_static()                 -- 背景 + 全部瓦片 (29ms, 只在换关/开砖时发生)
        GAME.dn, GAME.dn_prev = 0, 0
    elseif GAME.msg_box then               -- 消息条消失 → 把那块擦回静态层
        restore_rect(GAME.msg_box[1], GAME.msg_box[2], GAME.msg_box[3], GAME.msg_box[4])
        GAME.msg_box = nil
    end

    draw_field(P.x, P.y, P.mx, P.my, P.dir)     -- 擦旧格 + 画新格 (~6ms)
    hud_cached()

    if st == "menu" then
        UI.playmenu(GAME.play_sel)
        -- 个人菜单摘要 (原版探险资历里最关键的三项)
        local x = 8
        app.fill(0, H - 26, W - 1, H - 1, BK)
        app.mix(x, H - 22, "血 " .. P.hp .. "/" .. P.hpmax(), true)
        x = x + 100
        app.mix(x, H - 22, "泡 " .. P.maxbomb(), true)
        x = x + 70
        app.mix(x, H - 22, "威 " .. P.pow(), true)
        x = x + 70
        app.mix(x, H - 22, "晶 " .. P.crystal, true)
    elseif st == "settle" then
        settle_panel()
    elseif st == "over" then
        over_panel()
    end

    if GAME.msg and (app.millis() - GAME.msg_t) <= 2400 then
        local w = app.mixw(GAME.msg)
        local mx = math.floor((W - w - 10) / 2)
        local my = H - 48
        app.fill(mx, my, mx + w + 9, my + 19, WH)
        app.rect(mx, my, mx + w + 9, my + 19, BK)
        app.mix(mx + 5, my + 1, GAME.msg, false)
        GAME.msg_box = { mx, my, mx + w + 9, my + 19 }
    end
end

msg = function(s)
    GAME.msg, GAME.msg_t = s, app.millis()
    GAME.dirty = true
end
-- ============================================================================
-- 契约函数 (必须全局)
-- ============================================================================
function on_enter()
    if not RES.enter() then
        msg("素材缺失 " .. (RES.err or "?"))
    end
    player_reset(false)
    SAVE.load()
    RES.load_bg(1)
    GAME.state = "lobby"
    GAME.menu_sel, GAME.bag_sel, GAME.fg_sel, GAME.world_sel = 1, 1, 1, LEVEL.world
    GAME.dirty, GAME.page_dirty, GAME.play_full = true, true, true
    GAME.last_state, GAME.hud_key, GAME.msg_box = nil, nil, nil
    GAME.dn, GAME.dn_prev = 0, 0
    app.log(string.format("QT2 tile=%s face=%s item=%s wicon=%s equip=%s bg=%s",
                          tostring(RES.tile), tostring(RES.face), tostring(RES.item),
                          tostring(RES.wicon), tostring(RES.equip), tostring(RES.bg_h)))
end

function on_exit()
    if SAVE.dirty then SAVE.write() end
    app.img.freeall()
end

function render() GAME.render() end

local function enter_stage(world, stage)
    local acc = 0
    for i = 1, world - 1 do acc = acc + CFG.WORLDS[i][2] end
    GAME.start_stage(acc + stage)
end

function action(a)
    GAME.dirty = true
    GAME.page_dirty = true        -- 菜单页(大厅/背包/锻造/选关)按键后重画一次
    if type(a) ~= "number" or a < 1 or a > 7 then return true end

    -- ============ 大厅 ============
    if GAME.state == "lobby" then
        local n = #GAME.menu_items
        if a == A_LEFT or a == A_UP then GAME.menu_sel = (GAME.menu_sel - 2) % n + 1
        elseif a == A_RIGHT or a == A_DOWN then GAME.menu_sel = GAME.menu_sel % n + 1
        elseif a == A_BACK then
            return false                   -- ★ 未消费 → 框架弹「退出程序?」
        elseif a == A_OK then
            local s = GAME.menu_sel
            if s == 1 then GAME.state = "world"; GAME.world_sel = LEVEL.world
            elseif s == 2 then GAME.state = "bag"; GAME.bag_sel = 1
            elseif s == 3 then GAME.state = "forge"; GAME.fg_sel = 1
            else app.exit() end
        end
        return true
    end

    -- ============ 选关 ============
    if GAME.state == "world" then
        local g = WORLD_GRID
        local i = GAME.world_sel
        if a == A_LEFT then i = i - 1
        elseif a == A_RIGHT then i = i + 1
        elseif a == A_UP then i = i - g.cols
        elseif a == A_DOWN then i = i + g.cols end
        if i < 1 then i = i + #CFG.WORLDS end
        if i > #CFG.WORLDS then i = i - #CFG.WORLDS end
        GAME.world_sel = i
        RES.load_bg(i)
        if a == A_BACK then GAME.state = "lobby"
        elseif a == A_OK then enter_stage(i, 1) end
        return true
    end

    -- ============ 背包 ============
    if GAME.state == "bag" then
        local n = #MAT.LIST
        if a == A_LEFT or a == A_UP then GAME.bag_sel = (GAME.bag_sel - 2) % n + 1
        elseif a == A_RIGHT or a == A_DOWN then GAME.bag_sel = GAME.bag_sel % n + 1
        elseif a == A_BACK or a == A_OK then GAME.state = "lobby" end
        return true
    end

    -- ============ 锻造 ============
    if GAME.state == "forge" then
        if a == A_UP then GAME.fg_sel = math.max(1, GAME.fg_sel - 1)
        elseif a == A_DOWN then GAME.fg_sel = math.min(#EQUIP.SLOTS, GAME.fg_sel + 1)
        elseif a == A_BACK then GAME.state = "lobby"
        elseif a == A_OK then
            local slot = EQUIP.SLOTS[GAME.fg_sel]
            if FORGE.do_forge(slot) then
                msg("锻造 " .. EQUIP.NAME[slot] .. " " .. (P.equip[slot] or 0) .. "档")
                SAVE.mark()
            else
                local _, need = FORGE.can(slot)
                if need == 0 then msg("已满紫档") else msg("水晶不足 需" .. need) end
                app.tone_async(240, 60, 40)
            end
        end
        return true
    end

    -- ============ 游戏内菜单 ============
    if GAME.state == "menu" then
        local n = #GAME.play_items
        if a == A_LEFT or a == A_UP then GAME.play_sel = (GAME.play_sel - 2) % n + 1
        elseif a == A_RIGHT or a == A_DOWN then GAME.play_sel = GAME.play_sel % n + 1
        elseif a == A_BACK then GAME.state = "play"
        elseif a == A_OK then
            local s = GAME.play_sel
            if s == 1 then GAME.state = "play"
            elseif s == 2 then GAME.state = "bag"; GAME.bag_sel = 1
            elseif s == 3 then GAME.state = "forge"; GAME.fg_sel = 1
            elseif s == 4 then GAME.start_stage(LEVEL.n)
            else app.exit() end
        end
        return true
    end

    -- ============ 结算 / 阵亡 ============
    if GAME.state == "settle" then
        if a == A_OK then GAME.start_stage(LEVEL.n + 1)
        elseif a == A_BACK then GAME.state = "lobby" end
        return true
    end
    if GAME.state == "over" then
        if a == A_OK then
            P.hp = P.hpmax()
            GAME.start_stage(LEVEL.n)
        elseif a == A_BACK then GAME.state = "lobby" end
        return true
    end

    -- ============ play ============
    if a == A_OK then
        if not P.dead and not P.moving and not BOMB.at(P.x, P.y) then
            if BOMB.place(P.x, P.y, P.maxbomb(), P.pow()) then
                app.vibrate(18)
                app.tone_async(660, 50, 35)
            end
        end
    elseif a == A_BACK then
        GAME.state = "menu"
        GAME.play_sel = 1
    elseif a == A_LEFT or a == A_RIGHT or a == A_UP or a == A_DOWN then
        if not P.moving and not P.dead then
            try_move(a == A_LEFT and DIR_LEFT or a == A_RIGHT and DIR_RIGHT
                     or a == A_UP and DIR_UP or DIR_DOWN)
        end
    end
    return true
end

local last_t = 0
function poll()
    local now = app.millis()
    local dt = now - last_t
    last_t = now
    if dt <= 0 or dt > 120 then dt = 16 end
    if GAME.state == "play" then
        GAME.step(dt)
        GAME.dirty = true        -- 场地里怪物/引信/火焰一直在动, 逐帧重绘
    end
    -- ★ 静态页 (大厅/选关/背包/锻造/结算) **不要**周期置 dirty:
    --   它们只在按键时有变化, 而按键在 action() 里已经置了 dirty。
    --   旧版无条件 `每 16 帧置 dirty` + 桥层每帧 needs_redraw → 静态页也在满速重画,
    --   白白吃掉整个 CPU (真机表现为"画面更新迟缓、抓屏抓到半成品")。
end
