--@lapp v1
--@app name=迷你DayZ
--@app cat=游戏
--@app desc=废墟搜刮 + 僵尸生存: 找人烟、抢物资、别被围死
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
-- Mini DayZ · 掌机版 (LinTOS 0.8 Lua 单文件应用)
-- ----------------------------------------------------------------------------
-- 素材: ~/Desktop/minidayz-master (Godot 复刻工程, 粉丝作品) 经 tools/mdz_pack.py
--       转成 1bpp → apps/lua/minidayz/  →  设备上 /store/minidayz/
-- 取图依据 (不是猜的): 见 mdz_pack.py 顶部注释
--   item/durability_item.gd : SPRITE_HFRAMES=4 / MELEE_WEAPON_VFRAMES=17 / SPRITE_INIT_FRAME=39
--   item/melee_weapon.gd    : hframes=4 vframes=17 frame=39  ⇒ 近战图标 = 第 39 帧
--   item/ranged_weapon.gd   : 只设 hframes=4                  ⇒ 手枪图标 = 第 0 帧
--
-- 帧排列 (4 列 = 4 方向, 已逐帧看图确认):
--   索引 = 相位*4 + 方向        相位: 0 站立 / 1 走 A / 2 走 B
--   方向: 0 下(正面) 1 上(背面) 2 左 3 右
--
-- 版面 (400×300 全屏):
--   y  0..17   状态条 (血/食/水/武器/剩余僵尸)
--   y 18..297  场地 (20×15 格 @20px, 相机跟随)
--   返回键 → 背包 (再按返回关闭)
--
-- ★ 性能: 帧缓冲在 PSRAM, "画了多少像素"就是帧耗时。所以严格分层 ——
--   静态层(瓦片)只在换图/相机跨格时重建; 每帧只擦上一帧占用的格子 + 画新格。
--   脏标记是**两道独立闸门**(见 render)。
-- ============================================================================

-- ---- 动作常量: 必须带数值兜底 (真机上 app.UP 等可能读不到值 → 所有分支都不命中) ----
local A_UP    = (type(app.UP)      == "number") and app.UP      or 1
local A_DOWN  = (type(app.DOWN)    == "number") and app.DOWN    or 2
local A_LEFT  = (type(app.LEFT)    == "number") and app.LEFT    or 3
local A_RIGHT = (type(app.RIGHT)   == "number") and app.RIGHT   or 4
local A_OK    = (type(app.CONFIRM) == "number") and app.CONFIRM or 5
local A_BACK  = (type(app.BACK)    == "number") and app.BACK    or 6
local BK, WH  = 0, 1

local W, H = 400, 300

local function beep(f, ms)
    if app.tone_async then app.tone_async(f, ms) end
end
local function buzz(ms)
    if app.vibrate then app.vibrate(ms) end
end

-- ============================================================================
-- CFG
-- ============================================================================
local CFG = {}
CFG.TILE   = 20
-- ★ 世界生成规则**学自源工程 environment/ground.gd**(不是自己拍的):
--   地块 17×17 格 (BLOCK_SIZE_IN_TILE), 整图 16×16 地块 = 272×272 格; 路宽 4 格且
--   在每块内偏移 6..9; 空地 1/9 变水; 5 类地点按等级数量表落子(要求 3×3 邻域全空)。
--   掌机上按 **8×8 地块** 缩一档 (=136×136 格 = 2720×2720px), 规则完全一样。
CFG.BLOCK  = 17                                -- 每地块边长 (格) = 原版 BLOCK_SIZE_IN_TILE
CFG.BLOCKS = 8                                 -- 8×8 地块
CFG.MCOLS  = CFG.BLOCK * CFG.BLOCKS            -- 136
CFG.MROWS  = CFG.MCOLS
CFG.ROAD_W   = 4                               -- 原版 ROAD_WIDTH
CFG.ROAD_MIN = 6                               -- 原版 ROAD_MIN_IN_BLOCK; 路占 6..9
CFG.WATER_DEN = 9                              -- 原版 WATER_CHANCE_DENOMINATOR: 空地 1/9 变水
CFG.VCOLS, CFG.VROWS = 20, 15                  -- 视野 = 一屏
CFG.MW, CFG.MH = CFG.MCOLS * CFG.TILE, CFG.MROWS * CFG.TILE
CFG.HUD_H  = 20
CFG.DRAW_GAP_MS = 16

-- 地形 (顺序必须与 tools/mdz_pack.py 的 TILE_KINDS 一致)
local T_GRASS, T_DIRT, T_TREE, T_WATER, T_WALL, T_FLOOR, T_SAND, T_ROAD = 0, 1, 2, 3, 4, 5, 6, 7
-- 可走 = 不是树/水/墙
local BLOCK = { [T_TREE] = true, [T_WATER] = true, [T_WALL] = true }

-- 移动: 一格 20px, 拆 5 子步 × 4px
CFG.SUB, CFG.STEP_PX, CFG.STEP_MS = 5, 4, 46

-- 生存节奏
CFG.FOOD_MS, CFG.WATER_MS = 9000, 7000         -- 每 N 毫秒掉 1 点
CFG.STARVE_MS = 4000                           -- 饥饿/口渴见底后每 N 毫秒掉 1 血
CFG.SEARCH_MS = 700                            -- 搜刮用时
CFG.ATK_CD_MS = 380
CFG.ZOM_SIGHT, CFG.ZOM_ATK_MS, CFG.ZOM_DMG = 7, 900, 6

-- ============================================================================
-- 随机 (自实现 LCG: 只用小整数乘数 —— 固件 Lua 是 32 位整数, 大乘法会变浮点)
-- ============================================================================
local SEED = 12345
local function srnd(s) SEED = (s % 65519) + 1 end
local function rnd(n)                                   -- 0 .. n-1
    SEED = (SEED * 75 + 74) % 65519
    return SEED % n
end
local function rng(a, b) return a + rnd(b - a + 1) end

-- ============================================================================
-- RES — 素材 (map.lua + 3 张 bmp 全部进 PSRAM)
-- ============================================================================
local RES = { dir = "/store/minidayz/" }
local ITEMS = {}                                        -- ITEMS[i] = {name,icon,kind,val}
local SPR = {}                                          -- SPR[name] = 首帧号

function RES.enter()
    if RES.ready then return true end
    local f = app.fs.read(RES.dir .. "map.lua")
    if type(f) ~= "string" or #f < 40 then RES.err = "map.lua"; return false end
    local fn = load(f, "mdz_map")
    if not fn then RES.err = "map.lua 语法"; return false end
    local ok, meta = pcall(fn)
    if not ok or type(meta) ~= "table" then RES.err = "map.lua 内容"; return false end
    RES.meta = meta
    RES.dir  = meta.dir or RES.dir
    RES.SPX  = meta.spr_px or 32
    RES.SPRC = meta.spr_cols or 45
    RES.IPX  = meta.item_px or 32
    RES.TS   = meta.tile or 20
    RES.disp = meta.disp or {}
    RES.road = meta.road_disp or { m = 0, l = 0, r = 0, u = 0, d = 0 }
    for i = 1, #(meta.groups or {}) do
        local g = meta.groups[i]
        SPR[g.name] = g.first
    end
    ITEMS = meta.items or {}
    RES.tile = app.img.load(RES.dir .. "tile.bmp")
    RES.spr  = app.img.load(RES.dir .. "spr.bmp")
    RES.item = app.img.load(RES.dir .. "item.bmp")
    if not RES.tile or not RES.spr then RES.err = "bmp"; return false end
    RES.ready = true
    return true
end

-- 精灵: 32px 帧取正中 20×20 贴进 20px 的格
-- 从 32px 帧里取正中 size×size 贴出去。
-- ⚠ 图集是**网格**排布 (见 mdz_pack.py 的 SPR_COLS): 源坐标要按
--   行=idx//cols, 列=idx%cols 换算; 写成 `idx*SPX` 只有单行图集才对。
local function blit_spr_sz(first, dir, phase, sx, sy, size)
    if not RES.spr then return end
    local cols = RES.SPRC or 45
    local idx = first + (phase % 3) * 4 + (dir % 4)
    local ix, iy = idx % cols, idx // cols
    local so = (RES.SPX - size) // 2
    app.img.blit(RES.spr, ix * RES.SPX + so, iy * RES.SPX + so, sx, sy, size, size)
end
-- ⚠ 人物用 24px 而不是 20px: 源帧里的角色有 24px 高, 只取 20px 会把头和脚切掉,
--   屏幕上就成了"一小坨黑的"。反正动态层本来就标 3×3 邻格, 溢出 2px 是安全的。
local function blit_spr(first, dir, phase, sx, sy)
    blit_spr_sz(first, dir, phase, sx, sy, CFG.TILE)
end
local function blit_item(ic, sx, sy, size)
    if not RES.item then return end
    local ip = RES.IPX
    local so = (ip - size) // 2
    app.img.blit(RES.item, ic * ip + so, so, sx, sy, size, size)
end

-- 道具查表
function ITEM_OF(icon)
    for i = 1, #ITEMS do
        if ITEMS[i].icon == icon then return ITEMS[i], i end
    end
    return nil, nil
end
local function item_name_of(icon)
    local it = ITEM_OF(icon)
    return it and it.name or "?"
end

-- ============================================================================
-- WORLD — 地形生成 + 可走判定
-- ============================================================================
local T = {}                                            -- T[y*MCOLS+x] = 地形

local function tile_at(x, y)
    if x < 0 or y < 0 or x >= CFG.MCOLS or y >= CFG.MROWS then return T_WALL end
    return T[y * CFG.MCOLS + x] or T_GRASS
end
local function tile_set(x, y, v)
    if x < 0 or y < 0 or x >= CFG.MCOLS or y >= CFG.MROWS then return end
    T[y * CFG.MCOLS + x] = v
end
local function walkable(x, y)
    return not BLOCK[tile_at(x, y)]
end

-- 瓦片变体选择。
-- ⛔ 必须**确定性**: 静态层会被反复重建(相机跨格/开面板), 用随机的话同一格每次重建换个样子
--   → 满屏闪。所以用格子坐标的小整数哈希。
--   ⚠ 哈希只用 x*7 / y*13 这种小乘法: 固件 Lua 是 **32 位整数**, x*73856093 会变浮点再取模就报错。
-- 路面例外: 按上下左右**邻居接法**选竖/横/十字, 这样路才像路。
local function disp_tile(t, x, y)
    local d = RES.disp and RES.disp[t + 1]
    if not d or #d == 0 then return t end
    -- ⚠ 哈希只用 x*7 / y*13 这种小乘法: 固件 Lua 是 32 位整数, 大乘会变浮点再取模就报错
    local h3 = (x * 7 + y * 13 + (x * y) % 11) % 997 % 3

    if t == T_ROAD then
        -- 路缘线 = "这个格子在 4 格宽的路面里排第几列" (原版 ROAD_WIDTH=4, 地块内偏移 6..9)。
        -- 竖路看上下邻居、横路看左右邻居; 组内 +h3 换颗粒花纹, 避免沿路重复。
        local r = RES.road
        local up    = (tile_at(x, y - 1) == T_ROAD)
        local down  = (tile_at(x, y + 1) == T_ROAD)
        local left  = (tile_at(x - 1, y) == T_ROAD)
        local right = (tile_at(x + 1, y) == T_ROAD)
        local vert, horz = (up or down), (left or right)
        local bs = CFG.BLOCK
        if vert and not horz then
            local off = x - (x // bs) * bs - CFG.ROAD_MIN
            if off <= 0 then return r.l + h3 end
            if off >= CFG.ROAD_W - 1 then return r.r + h3 end
            return r.m + h3
        elseif horz and not vert then
            local off = y - (y // bs) * bs - CFG.ROAD_MIN
            if off <= 0 then return r.u + h3 end
            if off >= CFG.ROAD_W - 1 then return r.d + h3 end
            return r.m + h3
        end
        return r.m + h3                                  -- 路口 / 孤立路格
    end
    local n = #d
    if n == 1 then return d[1] end
    return d[h3 % n + 1]
end

local WORLD = {}
WORLD.buildings = {}                                    -- { {x0,y0,x1,y1}, ... } 供搜刮布点
WORLD.towns = {}                                        -- 地点中心 (格坐标), 供僵尸布点
WORLD.level = 1                                         -- 关卡等级(岛 1..4) → 地点数量表分支
WORLD.checkpoints = {}                                  -- 检查站 (官方: 十字路口的 90%)
WORLD.secret = nil                                      -- 秘密地点地块

-- 地块类型 (对标 ground.gd 的 BlockType 子集)
local B_EMPTY, B_VILLAGE, B_CITY, B_MILITARY, B_HOSPITAL, B_FIRE, B_SECRET = 0, 1, 2, 3, 4, 5, 6
local B_ROAD_H, B_ROAD_V, B_ROAD_X, B_WATER = 7, 8, 9, 10
local B_LOC = { [B_VILLAGE] = true, [B_CITY] = true, [B_MILITARY] = true,
                [B_HOSPITAL] = true, [B_FIRE] = true }
local B_ROADS = { [B_ROAD_H] = true, [B_ROAD_V] = true, [B_ROAD_X] = true }
local B_CONNECT = B_LOC                                  -- 能接路的只有 5 类地点 (原版 ROAD_CONNECT_TYPES)

-- 每个地块里的建筑数量 / 尺寸 (原版是模板场景里手摆的 slot, 掌机按下表近似)
local BLD_N    = { [B_VILLAGE] = 4, [B_CITY] = 7, [B_MILITARY] = 5,
                   [B_HOSPITAL] = 1, [B_FIRE] = 1, [B_SECRET] = 1 }
local BLD_SIZE = { [B_VILLAGE] = { 5, 8, 4, 6 }, [B_CITY] = { 5, 9, 4, 6 },
                   [B_MILITARY] = { 7, 12, 5, 8 }, [B_HOSPITAL] = { 10, 14, 7, 10 },
                   [B_FIRE] = { 8, 11, 6, 8 }, [B_SECRET] = { 5, 7, 4, 5 } }

-- 地点数量表 = **Bohemia 官方数字** (minidayz.com/blog/mini-dayz-guide-locations)。
-- 官方是 4 座岛, 每座岛换一张图, 越往后越冷、城市/军营越多、村子越少:
--   岛1: 20 village / 3 city / 1 fire / 1 hospital / 0 military
--   岛2: 15 village / 5 city / 2 fire / 2 hospital / 3 military
--   岛3:  6 village / 10 city/ 3 fire / 3 hospital / 6 military
--   岛4:  2 village / 11 city/ 2 fire / 4 hospital / 6 military (+ 4 gas)
-- 官方"岛"整图是 16×16 地块 = 256 块; 掌机是 8×8 = 64 块 ⇒ **按 1/4 缩**, 不足 1 的保底 1。
-- 字段序: { village, city, military, hospital, fire, secret }
local LEVEL_COUNTS = {
    { 5, 1, 0, 1, 1, 1 },      -- 岛1 (官方无军营)
    { 4, 1, 1, 1, 1, 1 },      -- 岛2
    { 2, 3, 2, 1, 1, 1 },      -- 岛3
    { 1, 3, 2, 1, 1, 1 },      -- 岛4
}

local BLK = {}                                          -- BLK[by*BLOCKS+bx] = 地块类型
local function blk_at(bx, by)
    if bx < 0 or by < 0 or bx >= CFG.BLOCKS or by >= CFG.BLOCKS then return B_EMPTY end
    return BLK[by * CFG.BLOCKS + bx]
end
local function blk_set(bx, by, v)
    if bx < 0 or by < 0 or bx >= CFG.BLOCKS or by >= CFG.BLOCKS then return end
    BLK[by * CFG.BLOCKS + bx] = v
end
-- 3×3 邻域全空才能落子 (原版 is_3x3_clear_at; 越界当空)
local function clear3(bx, by)
    for dy = -1, 1 do
        for dx = -1, 1 do
            if blk_at(bx + dx, by + dy) ~= B_EMPTY then return false end
        end
    end
    return true
end

-- 四邻偏移 (提在这里, 因为 Lua 不能直接索引表构造式)
local NBS = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } }

local function gen_world()
    srnd(app.millis() + 7)
    -- ① 全图 EMPTY
    BLK = {}
    for i = 0, CFG.BLOCKS * CFG.BLOCKS - 1 do BLK[i] = B_EMPTY end
    -- ⛔ 别写 GAME.level: `local GAME` 声明在本段**之后**, 这里会退化成全局查找 →
    --    "attempt to index a nil value (global 'GAME')"。等级放在 WORLD 上。
    local cnt = LEVEL_COUNTS[WORLD.level or 1] or LEVEL_COUNTS[1]

    -- ② 地点落子 (顺序同原版: village → military → city → hospital → fire)
    local function place(bt, n)
        local placed, tries = 0, 0
        local maxt = n * 500 + 1000
        while placed < n and tries < maxt do
            tries = tries + 1
            local bx, by = rng(0, CFG.BLOCKS - 1), rng(0, CFG.BLOCKS - 1)
            if clear3(bx, by) then blk_set(bx, by, bt); placed = placed + 1 end
        end
    end
    place(B_VILLAGE, cnt[1])
    place(B_MILITARY, cnt[3])
    place(B_CITY, cnt[2])
    place(B_HOSPITAL, cnt[4])
    place(B_FIRE, cnt[5])

    -- ③ 道路: **逐行连前两个可连地点** + **逐列连前两个** (原版 generate_roads)
    local function first_two_row(by)
        local a, b = -1, -1
        for bx = 0, CFG.BLOCKS - 1 do
            if B_CONNECT[blk_at(bx, by)] then
                if a < 0 then a = bx elseif b < 0 then b = bx; break end
            end
        end
        return a, b
    end
    local function first_two_col(bx)
        local a, b = -1, -1
        for by = 0, CFG.BLOCKS - 1 do
            if B_CONNECT[blk_at(bx, by)] then
                if a < 0 then a = by elseif b < 0 then b = by; break end
            end
        end
        return a, b
    end
    local function hline_blocks(by, x0, x1)
        for bx = x0 + 1, x1 - 1 do
            if blk_at(bx, by) == B_EMPTY then blk_set(bx, by, B_ROAD_H) end
        end
    end
    local function vline_blocks(bx, y0, y1)
        for by = y0 + 1, y1 - 1 do
            local t = blk_at(bx, by)
            if t == B_ROAD_H then blk_set(bx, by, B_ROAD_X)
            elseif t == B_EMPTY then blk_set(bx, by, B_ROAD_V) end
        end
    end
    for by = 0, CFG.BLOCKS - 1 do
        local a, b = first_two_row(by)
        if a >= 0 and b >= 0 then hline_blocks(by, a, b) end
    end
    for bx = 0, CFG.BLOCKS - 1 do
        local a, b = first_two_col(bx)
        if a >= 0 and b >= 0 then vline_blocks(bx, a, b) end
    end

    -- ④ 水: 道路生成后**剩余空地每格 1/9** 变 WATER (原版 generate_water_blocks)
    for by = 0, CFG.BLOCKS - 1 do
        for bx = 0, CFG.BLOCKS - 1 do
            if blk_at(bx, by) == B_EMPTY and rng(1, CFG.WATER_DEN) == 1 then
                blk_set(bx, by, B_WATER)
            end
        end
    end

    -- ⑤ 秘密地点: 3×3 全空的格随机挑一个 (原版 generate_secret_place)
    local cand = {}
    for by = 1, CFG.BLOCKS - 2 do
        for bx = 1, CFG.BLOCKS - 2 do
            if clear3(bx, by) then cand[#cand + 1] = by * 100 + bx end
        end
    end
    if #cand > 0 then
        local p = cand[rng(1, #cand)]
        local sbx, sby = p % 100, p // 100
        blk_set(sbx, sby, B_SECRET)
        WORLD.secret = { bx = sbx, by = sby }
        -- 官方原文: 秘密地点"从不与任何地点/道路/地图边缘相邻, 总被**湖泊或森林**包围",
        --   且不显示在小地图上。clear3 已保证 3×3 全空, 这里把外圈补成森林/水。
        for k = 1, #NBS do
            local nb = NBS[k]
            local nx, ny = sbx + nb[1], sby + nb[2]
            if blk_at(nx, ny) == B_EMPTY then
                blk_set(nx, ny, (rng(1, 100) <= 70) and B_EMPTY or B_WATER)
            end
        end
    end

    -- ⑥ 地块 → 瓦片
    T = {}
    for i = 0, CFG.MCOLS * CFG.MROWS - 1 do T[i] = T_GRASS end
    WORLD.buildings, WORLD.towns = {}, {}
    local BS = CFG.BLOCK

    for by = 0, CFG.BLOCKS - 1 do
        for bx = 0, CFG.BLOCKS - 1 do
            local bt = blk_at(bx, by)
            local ox, oy = bx * BS, by * BS
            if B_ROADS[bt] then
                -- 路: 原版路宽 4, 在地块内偏移 6..9
                local r0, r1 = oy + CFG.ROAD_MIN, oy + CFG.ROAD_MIN + CFG.ROAD_W - 1
                local c0, c1 = ox + CFG.ROAD_MIN, ox + CFG.ROAD_MIN + CFG.ROAD_W - 1
                if bt == B_ROAD_H or bt == B_ROAD_X then
                    for y = r0, r1 do
                        for x = ox, ox + BS - 1 do tile_set(x, y, T_ROAD) end
                    end
                end
                if bt == B_ROAD_V or bt == B_ROAD_X then
                    for x = c0, c1 do
                        for y = oy, oy + BS - 1 do tile_set(x, y, T_ROAD) end
                    end
                end
            elseif bt == B_WATER then
                -- 水塘: 地块内一个椭圆 (整块 17×17 全水太生硬)
                local cx, cy = ox + BS // 2, oy + BS // 2
                local rx, ry = rng(5, 7), rng(5, 7)
                for dy = -ry - 1, ry + 1 do
                    for dx = -rx - 1, rx + 1 do
                        local q = (dx * dx) * 100 // (rx * rx + 1) + (dy * dy) * 100 // (ry * ry + 1)
                        if q <= 100 then tile_set(cx + dx, cy + dy, T_WATER)
                        elseif q <= 175 then tile_set(cx + dx, cy + dy, T_SAND) end
                    end
                end
            elseif B_LOC[bt] then
                WORLD.towns[#WORLD.towns + 1] = { bx = bx, by = by, t = bt }
                -- 村里的中央小路: 接到相邻的路地块
                local mid = ox + BS // 2
                -- ⛔ 别写 `{ {..},{..} }[k]` —— Lua **不允许直接索引表构造式**(语法错误),
                --   要么加括号 (({...})[k]), 要么像这样提到循环外。
                for k = 1, #NBS do
                    local nb = NBS[k]
                    local n = blk_at(bx + nb[1], by + nb[2])
                    if B_ROADS[n] or n == B_EMPTY then
                        if nb[1] ~= 0 then
                            local yy = mid
                            for x = 0, BS - 1 do
                                if tile_at(ox + x, yy) == T_GRASS then tile_set(ox + x, yy, T_DIRT) end
                            end
                        else
                            local xx = mid
                            for y = 0, BS - 1 do
                                if tile_at(xx, oy + y) == T_GRASS then tile_set(xx, oy + y, T_DIRT) end
                            end
                        end
                    end
                end
                -- 建筑: 按类别数量, 在地块内留 1 格边距随便试
                local n2 = BLD_N[bt] or 3
                local sz = BLD_SIZE[bt] or { 5, 8, 4, 6 }
                local made = 0
                local guard = 0
                while made < n2 and guard < 120 do
                    guard = guard + 1
                    local bw, bh = rng(sz[1], sz[2]), rng(sz[3], sz[4])
                    local bx0 = ox + rng(1, BS - bw - 2)
                    local by0 = oy + rng(1, BS - bh - 2)
                    local bx1, by1 = bx0 + bw, by0 + bh
                    local ok = true
                    for y = by0 - 1, by1 + 1 do
                        for x = bx0 - 1, bx1 + 1 do
                            local t = tile_at(x, y)
                            if t == T_ROAD or t == T_WATER or t == T_WALL or t == T_SAND then ok = false end
                        end
                    end
                    for _, b in ipairs(WORLD.buildings) do
                        if bx0 <= b.x1 + 1 and bx1 >= b.x0 - 1 and by0 <= b.y1 + 1 and by1 >= b.y0 - 1 then ok = false end
                    end
                    if ok then
                        for y = by0, by1 do
                            for x = bx0, bx1 do
                                local edge = (x == bx0 or x == bx1 or y == by0 or y == by1)
                                tile_set(x, y, edge and T_WALL or T_FLOOR)
                            end
                        end
                        -- 门: 朝地块中心那面开 1 个
                        local cx0, cy0 = ox + BS // 2, oy + BS // 2
                        if math.abs(cx0 - (bx0 + bx1) // 2) > math.abs(cy0 - (by0 + by1) // 2) then
                            local dy = (cy0 > (by0 + by1) // 2) and by1 or by0
                            tile_set((bx0 + bx1) // 2, dy, T_FLOOR)
                        else
                            local dx = (cx0 > (bx0 + bx1) // 2) and bx1 or bx0
                            tile_set(dx, (by0 + by1) // 2, T_FLOOR)
                        end
                        WORLD.buildings[#WORLD.buildings + 1] = { x0 = bx0, y0 = by0, x1 = bx1, y1 = by1 }
                        made = made + 1
                    end
                end
            end
        end
    end

    -- ⑥b 检查站: 官方原文「地图上的十字路口 **90%** 都是检查站, 必有军人僵尸」。
    --     它不显示在小地图上, 是"路过就被围"的地方 —— 所以布点必须真的在十字路口。
    WORLD.checkpoints = {}
    for by = 0, CFG.BLOCKS - 1 do
        for bx = 0, CFG.BLOCKS - 1 do
            if blk_at(bx, by) == B_ROAD_X and rng(1, 10) <= 9 then
                WORLD.checkpoints[#WORLD.checkpoints + 1] = {
                    x = bx * CFG.BLOCK + CFG.ROAD_MIN + 1,
                    y = by * CFG.BLOCK + CFG.ROAD_MIN + 1,
                }
            end
        end
    end

    -- ⑦ 出生点: **落在村口**。
    --   原版一开局就在镇子边上(不是荒地中间) —— 2026-10-03 实测: 落在荒地中心时
    --   首屏全是草+路, 用户第一反应就是"没有僵尸也没有房子"。
    --   做法: 选离地图中心最近的 5 类地点, 再从它**外围** 6..26 格找一格能站的。
    local mx, my = CFG.MCOLS // 2, CFG.MROWS // 2
    local best, bestd = nil, 100000
    for i = 1, #WORLD.towns do
        local tw = WORLD.towns[i]
        local cx = tw.bx * CFG.BLOCK + CFG.BLOCK // 2
        local cy = tw.by * CFG.BLOCK + CFG.BLOCK // 2
        local d = math.abs(cx - my) + math.abs(cy - mx)
        if d < bestd then best, bestd = { cx, cy }, d end
    end
    local sx, sy = mx, my
    -- ⛔ 只判"这一格能站"是不够的: 实测会把玩家塞进**两栋房子之间的墙缝**,
    --   左右都是墙 → 按左右键一动不动, 看起来像"卡死"。
    --   所以还要"四邻至少 2 格能走"。并且按 路 > 土路 > 草地 的顺序挑(路一定走得通)。
    local function try_spawn(kind, rmin, rmax, cx, cy)
        for r = rmin, rmax do
            for dy = -r, r do
                for dx = -r, r do
                    if math.abs(dx) == r or math.abs(dy) == r then
                        local x, y = cx + dx, cy + dy
                        if tile_at(x, y) == kind then
                            local open = 0
                            for k = 1, #NBS do
                                if walkable(x + NBS[k][1], y + NBS[k][2]) then open = open + 1 end
                            end
                            if open >= 2 then return x, y end
                        end
                    end
                end
            end
        end
        return nil
    end
    if best then
        local x, y = try_spawn(T_ROAD, 5, 30, best[1], best[2])
        if not x then x, y = try_spawn(T_DIRT, 5, 30, best[1], best[2]) end
        if not x then x, y = try_spawn(T_GRASS, 5, 30, best[1], best[2]) end
        if x then sx, sy = x, y end
    end
    WORLD.spawn_x, WORLD.spawn_y = sx, sy
end

-- ============================================================================
-- 实体
-- ============================================================================
local P = {}                                            -- 玩家
local ZOM = { list = {} }                               -- 僵尸
local LOOT = { list = {}, at = {} }                     -- 可搜刮容器

local BAG = { order = {}, count = {} }                  -- 背包: icon -> 数量

function BAG.add(icon, n)
    if not BAG.count[icon] then
        BAG.count[icon] = 0
        BAG.order[#BAG.order + 1] = icon
    end
    BAG.count[icon] = BAG.count[icon] + (n or 1)
end
function BAG.take(icon, n)
    local c = BAG.count[icon]
    if not c or c < (n or 1) then return false end
    BAG.count[icon] = c - (n or 1)
    if BAG.count[icon] <= 0 then
        BAG.count[icon] = nil
        for i = 1, #BAG.order do
            if BAG.order[i] == icon then table.remove(BAG.order, i); break end
        end
    end
    return true
end
function BAG.count_of(icon) return BAG.count[icon] or 0 end
function BAG.clear() BAG.order, BAG.count = {}, {} end

local function player_reset()
    P.x, P.y = WORLD.spawn_x, WORLD.spawn_y
    P.ddx, P.ddy, P.sub = 0, 0, 0
    P.dir, P.phase, P.anim = 0, 0, 0
    P.hp, P.hp_max = 100, 100
    P.food, P.water = 100, 100
    P.wpn = nil                                         -- 当前武器 icon
    P.atk_cd, P.atk_anim = 0, 0
    P.search_t, P.search_x, P.search_y = 0, -1, -1
    P.hurt_t = 0
    BAG.clear()
end

-- ============================================================================
-- 僵尸
-- ============================================================================
local ZKIND = { "z_normal", "z_fast", "z_army" }

-- 僵尸分布: **贴着地点走** —— 城镇/军营/医院周围密, 野外稀疏。
-- 源工程(minidayz-master)的僵尸系统**尚未实现**(只有美术, 没有任何 enemy/zombie 脚本),
-- 所以这里按原版 Mini DayZ 的思路定规则:
--   ① 每个地点按类别布 k 只, 撒在该地块中心半径内 (保证"进屋就得打");
--   ② 野外再补少量散兵, 保证走路时也有威胁。
-- ⛔ 必须离出生点 ≥ 8 格, 否则一出生就被围 (QQ堂踩过"忽然探险失败")。
local TOWN_Z = { [B_VILLAGE] = 2, [B_CITY] = 4, [B_MILITARY] = 5,
                 [B_HOSPITAL] = 3, [B_FIRE] = 2, [B_SECRET] = 2 }
local WILD_Z = 12

local function spawn_one(zx, zy, force)
    if not walkable(zx, zy) then return false end
    local d = math.abs(zx - WORLD.spawn_x) + math.abs(zy - WORLD.spawn_y)
    if d < 8 then return false end
    local k = force or ZKIND[rng(1, #ZKIND)]
    ZOM.list[#ZOM.list + 1] = {
        x = zx, y = zy, ddx = 0, ddy = 0, sub = 0,
        dir = rng(0, 3), phase = 0, anim = 0,
        hp = (k == "z_army") and 26 or 18,
        kind = k, cd = 0, wait = rng(0, 900),
    }
    return true
end

function ZOM.spawn(n)
    ZOM.list = {}
    -- ① 地点周围
    for i = 1, #WORLD.towns do
        local tw = WORLD.towns[i]
        local k = TOWN_Z[tw.t] or 2
        local cx = tw.bx * CFG.BLOCK + CFG.BLOCK // 2
        local cy = tw.by * CFG.BLOCK + CFG.BLOCK // 2
        local put, guard = 0, 0
        while put < k and guard < k * 60 do
            guard = guard + 1
            local x = cx + rng(-9, 9)
            local y = cy + rng(-9, 9)
            if spawn_one(x, y) then put = put + 1 end
        end
    end
    -- ①b 检查站: 官方"必有军人僵尸" → 每站 4 只 z_army
    for i = 1, #WORLD.checkpoints do
        local cp = WORLD.checkpoints[i]
        local put, guard = 0, 0
        while put < 4 and guard < 240 do
            guard = guard + 1
            if spawn_one(cp.x + rng(-4, 4), cp.y + rng(-4, 4), "z_army") then put = put + 1 end
        end
    end
    -- ② 野外
    local put, guard = 0, 0
    while put < WILD_Z and guard < WILD_Z * 120 do
        guard = guard + 1
        local x, y = rng(2, CFG.MCOLS - 3), rng(2, CFG.MROWS - 3)
        if spawn_one(x, y) then put = put + 1 end
    end
    -- ③ 不够就再补 (n 是上限期望, 不强制)
    if n and #ZOM.list < n then
        local g2 = 0
        while #ZOM.list < n and g2 < n * 80 do
            g2 = g2 + 1
            spawn_one(rng(2, CFG.MCOLS - 3), rng(2, CFG.MROWS - 3))
        end
    end
end

-- ============================================================================
-- 容器 / 搜刮
-- ============================================================================
local function gen_loot()
    LOOT.list, LOOT.at = {}, {}
    for _, b in ipairs(WORLD.buildings) do
        for y = b.y0 + 1, b.y1 - 1 do
            for x = b.x0 + 1, b.x1 - 1 do
                -- 箱子密度: 42% 太挤(满屋子小方框), 18% 才有"废墟里零星几个"的感觉
                if tile_at(x, y) == T_FLOOR and rnd(100) < 18 then
                    local t = { x = x, y = y, n = rng(1, 2), taken = false }
                    LOOT.list[#LOOT.list + 1] = t
                    LOOT.at[y * CFG.MCOLS + x] = t
                end
            end
        end
    end
end
local function loot_at(x, y) return LOOT.at[y * CFG.MCOLS + x] end

-- 战利品表: 按类别加权 (食物/饮水/药 保底, 武器稀有)
local function roll_loot()
    local r = rnd(100)
    local want
    if r < 34 then want = "food"
    elseif r < 60 then want = "drink"
    elseif r < 80 then want = "med"
    elseif r < 92 then want = "tool"
    else want = "weapon" end
    local pool = {}
    for i = 1, #ITEMS do
        local kd = ITEMS[i].kind
        if want == "weapon" then
            if kd == "melee" or kd == "gun" then pool[#pool + 1] = ITEMS[i] end
        elseif kd == want then
            pool[#pool + 1] = ITEMS[i]
        end
    end
    if #pool == 0 then pool = ITEMS end
    return pool[rng(1, #pool)].icon
end

-- ============================================================================
-- 相机 (整格吸附)
-- ============================================================================
local CAM = { x = 0, y = 0 }
local function cam_follow()
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

-- ============================================================================
-- GAME 状态
-- ============================================================================
local GAME = {
    state = "play",            -- play / bag / dead
    dirty = true, last_draw = -9999,
    page_dirty = true, play_full = true,
    last_state = nil, hud_key = nil,
    msg = nil, msg_t = 0,
    bag_sel = 1,
    last_t = 0, food_t = 0, water_t = 0, starve_t = 0,
    kills = 0, elapsed = 0,
    dyn_cur = {}, dyn_prev = {}, dn = 0, dn_prev = 0,
}

local function msg(s)
    GAME.msg, GAME.msg_t = s, app.millis()
    GAME.dirty = true
end

-- ============================================================================
-- 场地绘制: 静态层 / 动态层
-- ============================================================================
local function blit_tile(t, sx, sy)
    if not RES.tile then return end
    app.img.blit(RES.tile, t * CFG.TILE, 0, sx, sy, CFG.TILE, CFG.TILE)
end

function GAME.draw_static()
    local c0, r0 = CAM.x // CFG.TILE, CAM.y // CFG.TILE
    for r = r0, r0 + CFG.VROWS - 1 do
        for c = c0, c0 + CFG.VCOLS - 1 do
            blit_tile(disp_tile(tile_at(c, r), c, r), c * CFG.TILE - CAM.x, r * CFG.TILE - CAM.y)
        end
    end
    GAME.dn_prev = 0
end

-- 把一格擦回静态层 (参数是**世界格坐标**)
local function restore_cell(cx, cy)
    if cx < 0 or cy < 0 or cx >= CFG.MCOLS or cy >= CFG.MROWS then return end
    local sx, sy = cx * CFG.TILE - CAM.x, cy * CFG.TILE - CAM.y
    if sx <= -CFG.TILE or sy <= -CFG.TILE or sx >= W or sy >= H then return end
    -- 首行被状态条压着: 擦它会把状态条盖花 → 作废 HUD 缓存让下一帧重画
    if sy < CFG.HUD_H then GAME.hud_key = nil; return end
    blit_tile(disp_tile(tile_at(cx, cy), cx, cy), sx, sy)
end

local function dcell(x, y)
    local n = GAME.dn + 1
    if n > 3000 then return end
    GAME.dn = n
    GAME.dyn_cur[n] = y * 1000 + x
end
-- 精灵屏坐标带格内偏移 → 会压到 3×3 邻格, 全都要标 (否则移动拖影)
local function dcell9(x, y)
    dcell(x - 1, y - 1); dcell(x, y - 1); dcell(x + 1, y - 1)
    dcell(x - 1, y);     dcell(x, y);     dcell(x + 1, y)
    dcell(x - 1, y + 1); dcell(x, y + 1); dcell(x + 1, y + 1)
end

local function draw_dynamic()
    -- ① 擦掉上一帧的动态格
    local pv, pn = GAME.dyn_prev, GAME.dn_prev or 0
    for i = 1, pn do
        local c = pv[i]
        restore_cell(c % 1000, c // 1000)
    end
    local tmp = GAME.dyn_cur
    GAME.dyn_cur, GAME.dyn_prev = GAME.dyn_prev, GAME.dyn_cur
    GAME.dn_prev, GAME.dn = GAME.dn, 0

    local function vis(sx, sy)
        return sx > -CFG.TILE and sy > -CFG.TILE and sx < W and sy < H
    end

    -- ② 可搜刮容器: 一个白底黑框的小箱子
    for i = 1, #LOOT.list do
        local t = LOOT.list[i]
        if not t.taken then
            dcell(t.x, t.y)
            local sx, sy = t.x * CFG.TILE - CAM.x, t.y * CFG.TILE - CAM.y
            if vis(sx, sy) then
                app.fill(sx + 3, sy + 5, sx + CFG.TILE - 4, sy + CFG.TILE - 5, WH)
                app.rect(sx + 3, sy + 5, sx + CFG.TILE - 4, sy + CFG.TILE - 5, BK)
                app.hline(sx + 4, sx + CFG.TILE - 5, sy + 9, BK)
                app.px(sx + CFG.TILE // 2, sy + 7, BK)
            end
        else
            dcell(t.x, t.y)                             -- 搜过也要擦干净
        end
    end

    -- ③ 僵尸
    for i = 1, #ZOM.list do
        local z = ZOM.list[i]
        local zx = z.x + (z.ddx or 0) * (z.sub or 0) * CFG.STEP_PX / CFG.TILE
        local zy = z.y + (z.ddy or 0) * (z.sub or 0) * CFG.STEP_PX / CFG.TILE
        local sx = math.floor(zx * CFG.TILE) - CAM.x
        local sy = math.floor(zy * CFG.TILE) - CAM.y
        if sx > -CFG.TILE * 2 and sy > -CFG.TILE * 2 and sx < W + CFG.TILE and sy < H + CFG.TILE then
            dcell9(z.x, z.y)
            blit_spr(SPR[z.kind] or 36, z.dir, z.phase, sx, sy)
        else
            dcell9(z.x, z.y)
        end
    end

    -- ④ 玩家 (最后画, 压在最上层)
    if not P.dead then
        dcell9(P.x, P.y)
        local gx = P.x * CFG.TILE + P.ddx * P.sub * CFG.STEP_PX - CAM.x
        local gy = P.y * CFG.TILE + P.ddy * P.sub * CFG.STEP_PX - CAM.y
        local blink = (P.hurt_t > 0) and (math.floor(app.millis() / 100) % 2 == 0)
        if not blink and vis(gx - 2, gy - 2) then
            blit_spr_sz(SPR.player or 0, P.dir, P.phase, gx - 2, gy - 2, CFG.TILE + 4)
        end
        -- 挥击特效: 朝向前方画一小段线
        if P.atk_anim > 0 and vis(gx, gy) then
            local cx2, cy2 = gx + CFG.TILE // 2, gy + CFG.TILE // 2
            local dx = (P.dir == 2 and -1) or (P.dir == 3 and 1) or 0
            local dy = (P.dir == 0 and 1) or (P.dir == 1 and -1) or 0
            app.hline(cx2 + dx * 4, cx2 + dx * 12, cy2 + dy * 4, BK)
            app.px(cx2 + dx * 14, cy2 + dy * 12, BK)
            app.px(cx2 + dx * 12, cy2 + dy * 14, BK)
        end
    end
end

-- ============================================================================
-- UI: 状态条 / 背包 / 死亡
-- ============================================================================
local function bar(x, y, w, cur, mx, inv)
    local h = 6
    app.fill(x, y, x + w, y + h, inv and BK or WH)
    app.rect(x, y, x + w, y + h, BK)
    if cur > 0 then
        local fw = (w - 2) * cur // mx
        if fw > 0 then
            app.fill(x + 1, y + 1, x + fw, y + h - 1, BK)
        end
    end
end

local function hud_cached()
    local wn = P.wpn and item_name_of(P.wpn) or "空手"
    -- ⛔ key 必须带上"会变的东西": 漏了秒数 → 计时器永远停在 0s (实测)。
    --   按秒取整, 不要用毫秒 (否则每帧都重画状态条, 白白烧帧)。
    local key = P.hp .. "|" .. P.food .. "|" .. P.water .. "|" .. wn .. "|" ..
                #ZOM.list .. "|" .. (GAME.elapsed // 1000)
    if GAME.hud_key == key then return end
    GAME.hud_key = key

    app.fill(0, 0, W - 1, CFG.HUD_H - 1, WH)
    app.hline(0, W - 1, CFG.HUD_H - 1, BK)

    bar(3, 6, 52, P.hp, P.hp_max, false)
    app.mix(58, 3, "食" .. P.food, false)
    app.mix(104, 3, "水" .. P.water, false)
    app.mix(152, 3, wn, false)
    app.mix(250, 3, "尸" .. #ZOM.list, false)
    app.mix(310, 3, "杀" .. GAME.kills, false)
    app.mix(348, 3, (GAME.elapsed // 1000) .. "s", false)
end

local function draw_bag()
    app.fill(30, 26, W - 31, H - 27, WH)
    app.rect(30, 26, W - 31, H - 27, BK)
    app.rect(31, 27, W - 32, H - 28, BK)
    app.mix(38, 32, "背包", false)
    app.hline(36, W - 37, 50, BK)

    local n = #BAG.order
    if n == 0 then
        app.mix(150, 140, "空空如也", false)
    else
        if GAME.bag_sel > n then GAME.bag_sel = n end
        if GAME.bag_sel < 1 then GAME.bag_sel = 1 end
        local y = 56
        local last = math.min(n, 9)
        local first = 1
        if GAME.bag_sel > last then first = GAME.bag_sel - last + 1 end
        for i = first, math.min(n, first + last - 1) do
            local ic = BAG.order[i]
            local sel = (i == GAME.bag_sel)
            local y0 = y - 2
            if sel then
                app.fill(38, y0, W - 39, y0 + 20, BK)
            end
            blit_item(ic, 40, y0, 20)
            local nm = item_name_of(ic)
            app.mix(66, y0 + 2, nm .. " x" .. BAG.count_of(ic), sel)
            local it = ITEM_OF(ic)
            if it and P.wpn == ic then
                app.mix(W - 90, y0 + 2, "[装备]", sel)
            end
            y = y + 22
        end
    end
    app.mix(40, H - 46, "上/下=选  确认=使用/装备  返回=关", false)
end

local function draw_dead()
    app.fill(60, 90, W - 61, H - 91, WH)
    app.rect(60, 90, W - 61, H - 91, BK)
    app.rect(61, 91, W - 62, H - 92, BK)
    app.mix(150, 106, "你死了", false)
    app.hline(68, W - 69, 130, BK)
    app.mix(100, 142, "存活 " .. (GAME.elapsed // 1000) .. " 秒", false)
    app.mix(100, 166, "击杀 " .. GAME.kills .. " 只", false)
    app.mix(100, 190, "搜刮 " .. (GAME.looted_get or 0), false)
    app.mix(96, 226, "确认=重来   返回=退出", false)
end

-- ============================================================================
-- 逻辑
-- ============================================================================
local function solid_for_zombie(x, y)
    if not walkable(x, y) then return true end
    return false
end

local function zombie_step(z, dt)
    -- 移动插值
    if z.ddx ~= 0 or z.ddy ~= 0 then
        z.anim = z.anim + dt
        while z.anim >= CFG.STEP_MS do
            z.anim = z.anim - CFG.STEP_MS
            z.sub = z.sub + 1
            if z.sub >= CFG.SUB then
                z.x, z.y = z.x + z.ddx, z.y + z.ddy
                z.sub, z.ddx, z.ddy = 0, 0, 0
                break
            end
        end
        return
    end
    z.phase = 0
    z.cd = z.cd - dt
    if z.cd > 0 then return end

    local dx, dy = P.x - z.x, P.y - z.y
    local dist = math.abs(dx) + math.abs(dy)
    local tx, ty = z.x, z.y
    if dist <= CFG.ZOM_SIGHT and not P.dead then
        -- 追: 优先走差距大的那条轴
        if math.abs(dx) > math.abs(dy) then
            tx = z.x + (dx > 0 and 1 or -1)
        else
            ty = z.y + (dy > 0 and 1 or -1)
        end
        if not walkable(tx, ty) then                  -- 被挡就换另一轴
            tx, ty = z.x, z.y
            if math.abs(dx) > math.abs(dy) then
                ty = z.y + (dy > 0 and 1 or -1)
            else
                tx = z.x + (dx > 0 and 1 or -1)
            end
        end
    else
        -- 徘徊: 一半时间发呆, 一半时间沿一个方向走几步
        if rnd(100) < 45 then z.cd = rng(400, 1400); return end
        local d = rnd(4)
        tx = z.x + ((d == 2 and -1) or (d == 3 and 1) or 0)
        ty = z.y + ((d == 0 and 1) or (d == 1 and -1) or 0)
    end
    if walkable(tx, ty) then
        z.ddx, z.ddy = tx - z.x, ty - z.y
        if math.abs(z.ddx) > math.abs(z.ddy) then z.dir = (z.ddx > 0) and 3 or 2
        else z.dir = (z.ddy > 0) and 0 or 1 end
        z.sub, z.anim, z.phase = 0, 0, 1
        z.cd = rng(260, 620)
    else
        z.cd = rng(200, 500)
    end
    -- 近身攻击
    if dist <= 1 and not P.dead then
        if z.wait <= 0 then
            P.hp = P.hp - CFG.ZOM_DMG
            P.hurt_t = 300
            z.wait = CFG.ZOM_ATK_MS
            buzz(40)
            beep(180, 60)
            GAME.dirty = true
            if P.hp <= 0 then
                P.hp = 0
                P.dead = true
                GAME.state = "dead"
                GAME.page_dirty = true
            end
        end
    end
    z.wait = math.max(0, z.wait - dt)
end

local function start_move(ddx, ddy)
    local nx, ny = P.x + ddx, P.y + ddy
    if walkable(nx, ny) then
        P.ddx, P.ddy, P.sub = ddx, ddy, 0
        if ddx ~= 0 then P.dir = (ddx > 0) and 3 or 2
        else P.dir = (ddy > 0) and 0 or 1 end
        return true
    end
    -- 撞墙也要转身 (手感: 朝墙走要能看到正面朝墙)
    if ddx ~= 0 then P.dir = (ddx > 0) and 3 or 2
    else P.dir = (ddy > 0) and 0 or 1 end
    return false
end

local function player_attack()
    if P.atk_cd > 0 then return end
    P.atk_cd = CFG.ATK_CD_MS
    P.atk_anim = 160
    local dmg = 2                                       -- 空手
    if P.wpn then
        local it = ITEM_OF(P.wpn)
        if it then dmg = it.val or 2 end
    end
    local tx = P.x + ((P.dir == 2 and -1) or (P.dir == 3 and 1) or 0)
    local ty = P.y + ((P.dir == 0 and 1) or (P.dir == 1 and -1) or 0)
    local hit = false
    for i = #ZOM.list, 1, -1 do
        local z = ZOM.list[i]
        if (z.x == tx and z.y == ty) or (z.x == P.x and z.y == P.y) then
            z.hp = z.hp - dmg
            hit = true
            beep(520, 40)
            if z.hp <= 0 then
                table.remove(ZOM.list, i)
                GAME.kills = GAME.kills + 1
                beep(880, 60)
            end
        end
    end
    if not hit then beep(300, 30) end
    GAME.dirty = true
end

local function player_search()
    local t = loot_at(P.x, P.y)
    if not t or t.taken then
        player_attack()                                 -- 脚下没箱子 → 挥击
        return
    end
    if P.search_x ~= P.x or P.search_y ~= P.y then
        P.search_x, P.search_y, P.search_t = P.x, P.y, 0
    end
end

local function finish_search()
    local t = loot_at(P.x, P.y)
    if not t or t.taken then return end
    local got = {}
    for _ = 1, t.n do
        local ic = roll_loot()
        BAG.add(ic, 1)
        got[#got + 1] = item_name_of(ic)
    end
    t.taken = true
    GAME.looted_get = (GAME.looted_get or 0) + 1
    P.search_t, P.search_x, P.search_y = 0, -1, -1
    beep(740, 50)
    msg("搜到 " .. table.concat(got, " "))
    GAME.dirty = true
end

local function use_item(icon)
    local it = ITEM_OF(icon)
    if not it then return end
    if it.kind == "food" then
        if P.food >= 100 then msg("不饿"); return end
        P.food = math.min(100, P.food + it.val); BAG.take(icon, 1); beep(660, 50)
    elseif it.kind == "drink" then
        if P.water >= 100 then msg("不渴"); return end
        P.water = math.min(100, P.water + it.val); BAG.take(icon, 1); beep(700, 50)
    elseif it.kind == "med" then
        if P.hp >= P.hp_max then msg("血是满的"); return end
        P.hp = math.min(P.hp_max, P.hp + it.val); BAG.take(icon, 1); beep(600, 50)
    elseif it.kind == "melee" or it.kind == "gun" then
        P.wpn = icon
        msg("装备 " .. it.name)
    else
        msg(it.name .. " 暂时用不上")
    end
    GAME.dirty = true
end

local function step(dt)
    -- 玩家移动
    if P.ddx ~= 0 or P.ddy ~= 0 then
        P.anim = P.anim + dt
        GAME.dirty = true
        while P.anim >= CFG.STEP_MS do
            P.anim = P.anim - CFG.STEP_MS
            P.sub = P.sub + 1
            if P.sub >= CFG.SUB then
                P.x, P.y = P.x + P.ddx, P.y + P.ddy
                P.sub, P.ddx, P.ddy = 0, 0, 0
                -- 走到新格: 清掉搜刮进度
                if P.search_x ~= P.x or P.search_y ~= P.y then
                    P.search_t, P.search_x, P.search_y = 0, -1, -1
                end
                break
            end
        end
    else
        -- ⛔ 只用 app.key_down (保持态)。别混用 key_down + keys_mask:
        --   两条通道叠起来会互相覆盖, 表现为"斜向乱走"。
        local ddx, ddy = 0, 0
        if app.key_down and app.key_down(A_UP) then ddy = -1
        elseif app.key_down and app.key_down(A_DOWN) then ddy = 1
        elseif app.key_down and app.key_down(A_LEFT) then ddx = -1
        elseif app.key_down and app.key_down(A_RIGHT) then ddx = 1 end
        if ddx ~= 0 or ddy ~= 0 then start_move(ddx, ddy); GAME.dirty = true end
    end

    -- 搜刮进度
    if P.search_t > 0 and P.search_x == P.x and P.search_y == P.y then
        P.search_t = P.search_t + dt
        GAME.dirty = true
        if P.search_t >= CFG.SEARCH_MS then finish_search() end
    end

    -- 冷却
    if P.atk_cd > 0 then P.atk_cd = math.max(0, P.atk_cd - dt) end
    if P.atk_anim > 0 then P.atk_anim = math.max(0, P.atk_anim - dt); GAME.dirty = true end
    if P.hurt_t > 0 then P.hurt_t = math.max(0, P.hurt_t - dt) end

    -- 行走动画相位
    if P.sub > 0 then P.phase = (P.sub < 3) and 1 or 2 else P.phase = 0 end

    -- 僵尸
    local any_moving = false
    for i = 1, #ZOM.list do
        zombie_step(ZOM.list[i], dt)
        if ZOM.list[i].ddx ~= 0 or ZOM.list[i].ddy ~= 0 then any_moving = true end
    end
    if any_moving or #ZOM.list > 0 then
        -- 僵尸闲着也判脏 → 但它们不动时不必每帧重画, 用较粗的节拍兜底
        ZOM.tick = (ZOM.tick or 0) + dt
        if ZOM.tick >= 120 then ZOM.tick = 0; GAME.dirty = true end
    end

    -- 生存消耗
    if not P.dead then
        GAME.elapsed = GAME.elapsed + dt
        GAME.food_t = GAME.food_t + dt
        GAME.water_t = GAME.water_t + dt
        if GAME.food_t >= CFG.FOOD_MS then
            GAME.food_t = 0
            P.food = math.max(0, P.food - 1)
            GAME.dirty = true
        end
        if GAME.water_t >= CFG.WATER_MS then
            GAME.water_t = 0
            P.water = math.max(0, P.water - 1)
            GAME.dirty = true
        end
        if P.food <= 0 or P.water <= 0 then
            GAME.starve_t = GAME.starve_t + dt
            if GAME.starve_t >= CFG.STARVE_MS then
                GAME.starve_t = 0
                P.hp = P.hp - 1
                GAME.dirty = true
                if P.hp <= 0 then
                    P.hp = 0; P.dead = true
                    GAME.state = "dead"; GAME.page_dirty = true
                end
            end
        else
            GAME.starve_t = 0
        end
    end
end

-- ============================================================================
-- 渲染
-- ============================================================================
function render()
    local now = app.millis()
    -- ★★ 两道独立闸门 (旧写法 `if not dirty and (now-last) < GAP` 是错的: 过了 GAP 就无条件重绘)
    if not GAME.dirty then return end
    if (now - GAME.last_draw) < CFG.DRAW_GAP_MS then return end
    GAME.last_draw = now
    GAME.dirty = false

    local st = GAME.state
    if GAME.last_state ~= st then
        GAME.last_state = st
        GAME.page_dirty = true
    end

    if st == "dead" then
        if GAME.page_dirty then
            GAME.page_dirty = false
            app.clear(WH)
            draw_dead()
        end
        return
    end

    if st == "bag" then
        if GAME.page_dirty then
            GAME.page_dirty = false
            cam_follow()
            app.clear(WH)
            GAME.draw_static()
            draw_dynamic()
            GAME.dn, GAME.dn_prev = 0, 0
        end
        hud_cached()
        draw_bag()
        return
    end

    -- play

    if cam_follow() then GAME.play_full = true end
    if GAME.play_full then
        GAME.play_full = false
        GAME.hud_key = nil
        app.clear(WH)
        GAME.draw_static()
        GAME.dn, GAME.dn_prev = 0, 0
    end
    draw_dynamic()
    hud_cached()

    -- 搜刮进度条
    if P.search_t > 0 and P.search_x == P.x and P.search_y == P.y then
        local bx, by = 120, H - 60
        app.fill(bx, by, bx + 160, by + 18, WH)
        app.rect(bx, by, bx + 160, by + 18, BK)
        local fw = 156 * P.search_t // CFG.SEARCH_MS
        if fw > 0 then app.fill(bx + 2, by + 2, bx + 1 + fw, by + 16, BK) end
        app.mix(bx + 44, by + 1, "搜刮中", true)
    end

    -- 消息条
    if GAME.msg and (app.millis() - GAME.msg_t) <= 2000 then
        local w = app.mixw(GAME.msg)
        local mx = math.floor((W - w - 10) / 2)
        local my = H - 36
        app.fill(mx, my, mx + w + 9, my + 19, WH)
        app.rect(mx, my, mx + w + 9, my + 19, BK)
        app.mix(mx + 5, my + 1, GAME.msg, false)
    end
end

-- ============================================================================
-- 契约函数 (必须全局)
-- ============================================================================
function poll()
    local now = app.millis()
    local dt = now - (GAME.last_t or now)
    if dt < 0 then dt = 0 elseif dt > 200 then dt = 200 end
    GAME.last_t = now
    if GAME.state == "play" then
        step(dt)
    else
        if GAME.pending_restart then
            GAME.pending_restart = false
        end
    end
end

local function new_game()
    gen_world()
    gen_loot()
    player_reset()
    ZOM.spawn(34)
    CAM.x, CAM.y = -1, -1
    GAME.kills, GAME.elapsed, GAME.looted_get = 0, 0, 0
    GAME.food_t, GAME.water_t, GAME.starve_t = 0, 0, 0
    GAME.msg, GAME.bag_sel = nil, 1
    P.dead = false
    GAME.state = "play"
    GAME.dirty, GAME.page_dirty, GAME.play_full = true, true, true
    GAME.last_state, GAME.hud_key = nil, nil
    GAME.dn, GAME.dn_prev = 0, 0
    GAME.last_t = app.millis()
    GAME.search_hint = true
end

function on_enter()
    if not RES.enter() then
        msg("素材缺失: " .. (RES.err or "?"))
    end
    new_game()
    msg("找箱子 按确认搜刮, 挥击打僵尸")
    app.log("MDZ ready spr=" .. tostring(RES.spr) .. " spr_player=" .. tostring(SPR.player))
end

function on_exit()
    app.img.freeall()
    RES.ready = false
end

function action(a)
    if type(a) ~= "number" or a < 1 or a > 7 then return true end
    GAME.dirty = true

    -- 死亡面板
    if GAME.state == "dead" then
        if a == A_OK then new_game()
        elseif a == A_BACK then app.exit() end
        return true
    end

    -- 背包
    if GAME.state == "bag" then
        local n = #BAG.order
        if a == A_UP then
            GAME.bag_sel = math.max(1, GAME.bag_sel - 1)
        elseif a == A_DOWN then
            GAME.bag_sel = math.min(math.max(1, n), GAME.bag_sel + 1)
        elseif a == A_OK then
            if n > 0 then use_item(BAG.order[GAME.bag_sel]) end
        elseif a == A_BACK then
            GAME.state = "play"
            GAME.page_dirty, GAME.play_full = true, true
        end
        return true
    end

    -- 游玩
    if a == A_BACK then
        GAME.state = "bag"
        GAME.bag_sel = 1
        GAME.page_dirty = true
    elseif a == A_OK then
        local t = loot_at(P.x, P.y)
        if t and not t.taken then
            player_search()
        else
            player_attack()
        end
    end
    return true                                       -- 恒 true: 应用自己拥有 BACK 语义
end
