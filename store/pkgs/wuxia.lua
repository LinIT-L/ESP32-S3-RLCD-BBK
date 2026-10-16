--@lapp v1
--@app name=英雄坛说
--@app cat=游戏
--@app desc=原版地图整图 + 原版人物 + 63 张场景: 拜师/行囊/回合战斗/走江湖
--@app version=1.0
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
-- wuxia.lua — 《英雄坛说》掌机版（单机武侠 MUD）
--
-- 复刻对象: gmud.online 的《英雄坛说》网页版
--   · 画面  —— **原版整图直接搬**: 每张地图就是站点上的 map/map<N>.bmp
--               （w*32 × h*32 的 1bpp 位图, 相机取 400×300 窗口, app.img 一次 blit 完）
--   · 人物  —— **原版精灵图集**: img/res/resmap<ID>.bmp ∩ ¬img/mask/resmapmask<ID>.bmp
--               （格号 = 素材 id, 32×32; 玩家四向三帧 32×48）
--   · 界面  —— **原版菜单素材**: ▶光标 / ⊙单选 / 实心方块 / 盘眼软键条 / 气血·内力图标 / 头像
--   · 世界  —— **原版数据**: 63 张地图的 t 值碰撞、152 扇门、出生点 (maps.json 原值)
--   · 内容  —— **原版数据**: 210 物品 / 211 功夫 / 453 NPC / 12 门派 (npc.json / kf.json / items.json)
--
-- 与原版不一致的地方 (如实标注, 不假装是原版):
--   NPC 的**地图与坐标**原站没有 (maps.json 的 npcs 只有 bmp/npcId, 位置由服务端 WS 下发,
--   客户端代码里那个变量从未被使用)。单机版用确定性哈希把 181 个 NPC 摊到各图可行走格上,
--   见 tools/wuxia_world_gen.py 的说明。
--
-- 版面 (400×300 全屏, 系统 16px 字体):
--   y   0.. 17   状态栏 (门派·等级 | 气血 | 内力 | 银两)
--   y  18..243   场景区 (原版地图整图窗口, 32px 瓦片)
--   y 244..299   文本区 (日志 2 行 + 盘眼软键提示条)
--
-- 模块 (单文件内局部表, 只允许向下依赖, 禁止成环):  RES / WORLD / P / NPC /
--   LOG / BATTLE / INV / SKILL / DLG / SAVE /NET / DATA / UI / GAME
-- 计划书: docs/武侠MUD网络版-计划书.md
-- ============================================================================

local A_UP    = (type(app.UP)      == "number") and app.UP      or 1
local A_DOWN  = (type(app.DOWN)    == "number") and app.DOWN    or 2
local A_LEFT  = (type(app.LEFT)    == "number") and app.LEFT    or 3
local A_RIGHT = (type(app.RIGHT)   == "number") and app.RIGHT   or 4
local A_OK    = (type(app.CONFIRM) == "number") and app.CONFIRM or 5
local A_BACK  = (type(app.BACK)    == "number") and app.BACK    or 6
local BK, WH  = 0, 1

-- 前置声明 (Lua 的 local 只对声明之后的代码可见 → 模块表**和**任何会被前面的
-- 函数体引用的局部变量, 都必须写在这里。漏一个就会变成"函数里引用的是全局 nil",
-- 表现为运行期 attempt to index a nil value —— 例如 P 若声明在 blit_player 之后,
-- blit_player 里的 P 就是全局 nil。)
local tick_n    = 0
local RES, WORLD, NPC, WDATA = {}, {}, {}, nil    -- WDATA = world.lua 读出来**数据**; WORLD = 模块
local P = {}                                      -- 玩家状态 (真正初始化在 player_init)
local SAVE, NET, BATTLE, DLG, INV, SKILL, GAME, UI, LOG = {}, {}, {}, {}, {}, {}, {}, {}, {}

-- ============================================================================
-- CFG — 版面与尺寸 (全屏 400×300, 瓦片 32px = 原版)
-- ⚠ 方向常量必须是**文件级 local**: 早先把它们写在 CFG 表构造里会变成全局变量,
--   于是方向常量就成了 nil → 移动判定恒不成立(怎么按都不动) + 画人物时 nil 运算报错。
-- ============================================================================
-- ⚠ 方向编号必须与原站一致: 原站 startMove 里 `dy = dir === 0 ? -1 : dir === 1 ? 1 : 0`,
--   dx = `dir === 3 ? 1 : dir === 2 ? -1 : 0` → **0=上 1=下 2=左 3=右**。
--   我一开始写成 0=下 1=上, 结果"按上脸朝下、按下脸朝上" (用户 2026-10-03 反馈)。
local DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT = 0, 1, 2, 3

local CFG = {
    CELL       = 16,                 -- 16px 字体格 (app.mix)
    TILE       = 32,                 -- 原版瓦片 32px
    STEP_SUB   = 4,                  -- 一格走 4 个子步 (原版 STEPS)
    STEP_PX    = 8,                  -- 每子步 8px (原版 PX_STEP)
    STEP_MS    = 40,                 -- 每子步 40ms → 一格 160ms (原版 STEP_MS)
    DRAW_GAP_MS = 16,
    SAVE_GAP   = 3.0,
    LOG_COLS   = 22,                 -- 右栏日志的列数 (汉字算 2 列)
}

-- ============================================================================
-- 原版菜单素材 (由 tools/wuxia_art_gen.py 注入; 不要手改这一段)
-- ============================================================================
-- @@WUXIA-ART-BEGIN@@
-- 《英雄坛说》原版素材直搬 (来自 gmud.online/img/**/*.bmp + 内联 HUD 位图)。
-- 由 tools/wuxia_art_gen.py 生成, **不要手改**; 素材改了重跑生成器。
-- 格式: ART.<名> = {w, h, "hex"}; hex 为行优先 MSB, 1=黑 0=透明 → app.bmp(x,y,w,h,d)。
local ART = {
  arrow     = {  6,  9, "c0e0f0f8fcf8f0e0c0" },
  arrowdual = { 15,  9, "028006c00ee03ef8fefe3ef80ee006c00280" },
  ball      = {  6,  6, "3048b4b44830" },
  box       = {  6,  6, "fcfcfcfcfcfc" },
  box1      = {  6,  6, "fc84848484fc" },
  check     = {  8,  6, "fdfefcfcfcfc" },
  check1    = {  8,  6, "fd86c4ac94fc" },
  digit     = { 47,  5, "3622a22223fea7baaefaabbeb622223a237eb6fbbababafe3623a23a23fe" },
  expball   = { 16, 16, "ffeff7cff3aff560c67deffbf7f707fbbffdcff8f7fbeffddf4d8096fdbafffc" },
  fface     = { 32, 32, "0003e000000ff800003ffe00007fef0000f9778000f7bb8001ee7dc001faebe001dbbfc001d57be001f4fdc001e8774001e87d4001f85bc001ec2fc001de3f40016a2bc001ec1ac00170054001d007c003388e60040e38100209c82001d80dc003f007e004388e1007ff7ff00ff777f80df9cfd807fffff007b6b7f0071363d0" },
  fhead     = { 32, 32, "0003e000000ff800003ffe00007fff0000ffff8000ffff8001ffffc001ffffe001ffffc001ffffe001ffffc001ffffc001ffffc001ffffc001ffffc001ffffc001ffffc001ffffc001ffffc001ffffc003ffffe007fffff003ffffe001ffffc003ffffe007fffff007fffff00ffffff80ffffff807fffff007fffff007fffff0" },
  finger    = { 14, 13, "018002400c80311c43e4800401fc008003808080438021001f00" },
  fp        = { 16,  8, "fff08610bed0bed08e10bef0bef0fff0" },
  fport     = { 32, 32, "0003e000000ff800003ffe00007fef0000f9778000f7bb8001ee7dc001faebe001dbbfc001d57be001f4fdc001e8774001e87d4001f85bc001ec2fc001de3f40016a2bc001ec1ac00170054001d007c003388e60040e38100209c82001d80dc003f007e004388e1007ff7ff00ff777f80df9cfd807fffff007b6b7f0071363d0" },
  hp        = { 16,  8, "fff0b610b6d0b6d08610b6f0b6f0fff0" },
  icon      = {  6,  6, "fcfcfcfcfcfc" },
  icon1     = {  6,  6, "fc84848484fc" },
  ink       = {  6,  6, "fcf0f0f0c0c0" },
  ink2      = {  3,  3, "e0e0c0" },
  mface     = { 32, 32, "00000000001f7e0000ffff0007ffffe01f9fe7e003ffcff807cfb1f01fe7e0f01fcac1f03f96f7f007b4fbf00ffefff00dfffff009ffffe001ffbfe001ff9fe000ffffe000728be000761ba003e619b806b003bc08d8c6fc09de1efc13d3f2fa17d8071a0bbc0c340c7bf0cc0ff8c3fc19ff0fe611fc36e211f0fee21fc3edfe" },
  mhead     = { 32, 32, "00000000001f7e0000ffff0007ffffe01fffffe003fffff807fffff01ffffff01ffffff03ffffff007fffff00ffffff00dfffff009ffffe001ffffe001ffffe000ffffe0007fffe0007fffe003fffff807fffffc0ffffffc0ffffffc1ffffffe1ffffffe0ffffffc0ffffffc0ffffffc1ffffffe1ffffffe1ffffffe1ffffffe" },
  mport     = { 32, 32, "00000000001f7e0000ffff0007ffffe01f9fe7e003ffcff807cfb1f01fe7e0f01fcac1f03f96f7f007b4fbf00ffefff00dfffff009ffffe001ffbfe001ff9fe000ffffe000728be000761ba003e619b806b003bc08d8c6fc09de1efc13d3f2fa17d8071a0bbc0c340c7bf0cc0ff8c3fc19ff0fe611fc36e211f0fee21fc3edfe" },
  pagedown  = {  8,  4, "80c1e3f7" },
  panyan    = { 64, 16, "000000000000000001800ff0008001000240081000c003000420081000a00500081008107f9009fe10080810400810022004081040042002781e0810400240020810781e4002400208102004400420020810100840081002081008107f9009fe0810042000a005000810024000c003000ff00180008001000000000000000000" },
  panyanf   = { 64, 16, "000000000000000001800ff0008001000240081000c003000420081000a00500081008107f9009fe10080810400810022004081040042002781e0810400240020810781e4002400208102004400420020810100840081002081008107f9009fe0810042000a005000810024000c003000ff00180008001000000000000000000" },
  radio     = {  8,  8, "3c4299bdbd99423c" },
  radio1    = {  8,  8, "3c4281818181423c" },
  vs        = { 24,  8, "f381c06103206103003201803200c01c00601c02600801c0" },
}
-- @@WUXIA-ART-END@@

-- 贴一张原版素材 (ART.<名> = {w,h,"hex"}; 1=黑 0=透明)
local function art(name, x, y)
    local a = ART[name]
    if a then app.bmp(x, y, a[1], a[2], a[3]) end
end

-- ============================================================================
-- RES — 原版图集 (app.img 载入 PSRAM; 地图整图 + 精灵图集 + 玩家图集)
-- ============================================================================
local SPW, SPH = 32, 32        -- 精灵格
local PLW, PLH = 32, 48        -- 玩家格
local SPCOLS   = 32            -- 精灵图集列数 (格号 = 素材 id)
local PLCOLS   = 8             -- 玩家图集列数

-- 地图元信息 + 世界数据 (由 tools/wuxia_pack.py / wuxia_world_gen.py 生成)
local function load_mod(path)
    local ok, txt = pcall(function() return app.fs.read(path) end)
    if not ok or type(txt) ~= "string" or #txt < 40 then return nil end
    local f = load(txt, path)
    if not f then return nil end
    local ok2, d = pcall(f)
    return (ok2 and type(d) == "table") and d or nil
end

local MAPDIR = "/store/wuxia/"
local META = nil               -- wuxia/map.lua (由 pack 生成, 含 dir/尺寸/地图名)

function RES.load_map(id)
    if RES.cur_map == id and RES.map then return true end
    if RES.map then app.img.free(RES.map); RES.map = nil end
    RES.cur_map = nil
    local p = MAPDIR .. "map" .. id .. ".bmp"
    local h = app.img.load(p, false)   -- 非 opaque: 只写墨点; 白底交给每帧 app.clear(WH)
                                           -- (省掉整屏"刷白"那一遍批量写 → 单帧绘制时间近乎减半)
    if not h then return false end
    RES.map = h
    RES.cur_map = id
    return true
end

function RES.enter()
    if RES.ready then return true end
    META  = load_mod(MAPDIR .. "map.lua")
    WDATA = load_mod(MAPDIR .. "world.lua")
    if not META or not WDATA then
        -- 报清楚**缺哪一个** —— 上一次只报"素材缺失", 排查时还得自己猜是哪个文件
        RES.err = (not META and "map.lua") or "world.lua"
        return false
    end
    MAPDIR = META.dir or MAPDIR
    WORLD.doors = WDATA.doors          -- 门表接线 (WORLD.door_at 要用)
    RES.spr = app.img.load(MAPDIR .. "spr.bmp")
    RES.pl  = app.img.load(MAPDIR .. "player.bmp")
    RES.sprm = app.img.load(MAPDIR .. "sprm.bmp")     -- 精灵底版 (mask): 刷白用
    RES.plm  = app.img.load(MAPDIR .. "playerm.bmp")  -- 玩家底版
    if not RES.spr then RES.err = "spr.bmp" ; return false end
    if not RES.pl  then RES.err = "player.bmp"; return false end
    RES.ready = true
    return true
end

-- ---- 贴图助手 ----
-- 精灵一律"先按底版(mask)刷白、再压墨点" —— 原站 makeNpcSprite / makePlayer 在 mask 内
-- **黑白都画**, 所以人物是实心的; 只画黑点会透出背后的地图 (用户反馈"背景会透过人物")。
local function blit_spr(id, x, y)
    if not RES.spr or id < 0 then return end
    local ox, oy = (id % SPCOLS) * SPW, math.floor(id / SPCOLS) * SPH
    if RES.sprm then app.img.blit(RES.sprm, ox, oy, x, y, SPW, SPH, 1) end
    app.img.blit(RES.spr, ox, oy, x, y, SPW, SPH, 0)
end
local function blit_player(dir, frame, x, y)
    if not RES.pl then return end
    local c = (P.gender or 0) * 12 + dir * 3 + frame
    local ox, oy = (c % PLCOLS) * PLW, math.floor(c / PLCOLS) * PLH
    if RES.plm then app.img.blit(RES.plm, ox, oy, x, y, PLW, PLH, 1) end
    app.img.blit(RES.pl, ox, oy, x, y, PLW, PLH, 0)
end

-- ============================================================================
-- P — 玩家 (所有字段必须有初值; 少一个就会在 %d 上炸)
-- ============================================================================
-- P 已在文件顶部前置声明 (blit_player 等更早定义的函数要引用它)
P.gender = 0
P.map = 1
P.x, P.y = 4, 4                 -- 当前地图与格坐标
P.tx, P.ty, P.px, P.py = nil, nil, 0, CFG.TILE
P.dir, P.frame = DIR_DOWN, 0
P.moving, P.step, P.step_t = false, 0, 0
P.lv, P.exp, P.hpMax, P.hp, P.mpMax, P.mp = 1, 0, 30, 30, 10, 10
P.silver, P.points = 20, 0
P.attr = { str = 3, spd = 2, wit = 2 }
P.equip = { weapon = nil, armor = nil }
P.bag = {}
P.arts, P.art_lv = { "basic" }, { basic = 1 }
P.sect, P.sect_name = nil, nil
local function player_init()
    P.gender = 0
    P.map = (WDATA and WDATA.start and WDATA.start.map) or 1
    P.x   = (WDATA and WDATA.start and WDATA.start.x) or 4
    P.y   = (WDATA and WDATA.start and WDATA.start.y) or 4
    P.tx, P.ty, P.px, P.py = nil, nil, 0, CFG.TILE
    P.dir, P.frame, P.moving, P.step, P.step_t = DIR_DOWN, 0, false, 0, 0
    P.lv, P.exp, P.hpMax, P.hp, P.mpMax, P.mp = 1, 0, 30, 30, 10, 10
    P.silver, P.points, P.sect = 20, 0, nil
    P.attr = { str = 3, spd = 2, wit = 2 }
    P.equip = { weapon = nil, armor = nil }
    P.bag = {}
    P.arts, P.art_lv = { "basic" }, { basic = 1 }
end

-- ============================================================================
-- WORLD — 原版地图/碰撞/门 + NPC   (数据在 WDATA, 见 RES.enter)
-- ============================================================================
local cur = nil                  -- 当前地图的展开表 { w, h, t{}, d{}, npcs{} }

local function world_maps() return WDATA and WDATA.maps or {} end

function NPC.place_for(mapid)
    local out = {}
    if not WDATA or not WDATA.npcs then return out end
    for i = 1, #WDATA.npcs do
        local n = WDATA.npcs[i]
        if n.map == mapid then out[#out + 1] = n end
    end
    return out
end

-- 展开当前地图: t 值 RLE → 数组 (碰撞); d "格下标:门下标," → 表 (门)
function WORLD.enter(mapid, x, y)
    local mp = world_maps()[mapid]
    if not mp then return false end
    local t, k = {}, 0
    for pair in string.gmatch(mp.t, "[^,]+") do
        local v, n = string.match(pair, "(%d+):(%d+)")
        if v then
            v, n = tonumber(v), tonumber(n)
            for _ = 1, n do t[k] = v; k = k + 1 end
        end
    end
    local d = {}
    if mp.d then
        for pair in string.gmatch(mp.d, "[^,]+") do
            local i, di = string.match(pair, "(%d+):(%d+)")
            if i then d[tonumber(i)] = tonumber(di) end
        end
    end
    local ok = RES.load_map(mapid)
    cur = { id = mapid, w = mp.w, h = mp.h, t = t, d = d, npcs = NPC.place_for(mapid) }
    P.map, P.x, P.y = mapid, x, y
    P.tx, P.ty, P.px, P.py = nil, nil, 0, CFG.TILE
    P.moving, P.step, P.step_t = false, 0, 0
    if META and META.maps and META.maps[mapid] then
        cur.name = META.maps[mapid].name
    end
    return ok
end

-- 原站 isBlocked: 越界 / t∈{1,3,4} 挡路。
-- ★ 与站点的一处**故意差异**: 原站在 NPC 格上也挡路; 这里**不挡** ——
--   单机版 NPC 位置是我自定的 (原站坐标在服务端), 挡路会把路切死, 用户实测就是
--   "走到某个点就过不去"。改成可穿过: 交谈改为"面朝的那一格有人就交谈"。
function WORLD.blocked(gx, gy)
    if not cur then return true end
    if gx < 0 or gy < 0 or gx >= cur.w or gy >= cur.h then return true end
    local t = cur.t[gy * cur.w + gx] or 0
    return (t == 1 or t == 3 or t == 4)
end
function WORLD.tile(gx, gy)
    if not cur then return 0 end
    return cur.t[gy * cur.w + gx] or 0
end
-- 踩到门格 (t==2) → 回传 {toMap, x, y}
function WORLD.door_at(gx, gy)
    if not cur then return nil end
    local i = gy * cur.w + gx
    local di = cur.d[ i ]
    if di and WORLD.doors and WORLD.doors[di + 1] then return WORLD.doors[di + 1] end
    return nil
end
function WORLD.npc_at(gx, gy)
    if not cur then return nil end
    for i = 1, #cur.npcs do
        local n = cur.npcs[i]
        if n.x == gx and n.y == gy then return n end
    end
    return nil
end

-- ============================================================================
-- LOG — 文本区日志 (环形缓冲, 新行在顶)
-- ============================================================================
LOG.buf = {}
function LOG.push(s)
    s = tostring(s)
    local out, cols, i = {}, 0, 1
    while i <= #s and cols < (CFG.LOG_COLS or 24) do
        local b = s:byte(i)
        local n = 1
        if b >= 0xF0 then n = 4 elseif b >= 0xE0 then n = 3 elseif b >= 0xC0 then n = 2 end
        out[#out + 1] = s:sub(i, i + n - 1)
        cols = cols + (n > 1 and 2 or 1)
        i = i + n
    end
    local line = table.concat(out)
    if i <= #s then line = line .. ".." end
    table.insert(LOG.buf, 1, line)
    while #LOG.buf > 2 do table.remove(LOG.buf) end   -- 只看"当下", 不留一堆历史
    LOG.t0 = app.millis()       -- 右栏浮层的显示起点 (到点自动隐去)
    GAME.dirty = true
end

-- ============================================================================
-- DATA — 原站内容数据 (物品/功夫/NPC), 从 /store/wuxia/data.lua 读
-- ============================================================================
local DATA = { item = {}, art = {}, npc = {} }
local GD = nil
local function gdata()
    if GD ~= nil then return GD end
    GD = false
    local d = load_mod("/store/wuxia/data.lua")
    if d then GD = d end
    return GD
end

-- 内置兜底 (数据文件缺失也能玩)
DATA.item = {
    basic = nil,
    med1 = { name = "金创药", kind = "med", price = 20, heal = 15 },
    med2 = { name = "回气丹", kind = "med", price = 30, mp = 12 },
    wpn1 = { name = "铁剑", kind = "weapon", price = 80, atk = 6 },
    arm1 = { name = "布衣", kind = "armor", price = 50, def = 4 },
    book1 = { name = "罗汉拳谱", kind = "book", price = 120, learn = "luohan" },
}
DATA.art = {
    basic = { name = "基本拳脚", dmg = 3, cost = 0, hit = 0, maxlv = 5, weapon = false },
    luohan = { name = "罗汉拳", dmg = 7, cost = 3, hit = 5, maxlv = 6, weapon = false },
    fengmo = { name = "风魔剑法", dmg = 11, cost = 5, hit = 8, maxlv = 6, weapon = true },
}
DATA.sect = {
    shaolin = { name = "少林", hp = 8, str = 1, arts = { "luohan" } },
    huashan = { name = "华山", hp = 5, str = 2, arts = { "fengmo" } },
}
DATA.sect_order = { "shaolin", "huashan" }

local function ensure_data()
    local d = gdata()
    if not d then return end
    -- 物品: 把原站字段折成游戏要用的 {name, kind, price, heal/mp/atk/def/learn}
    if d.ITEM then
        for k, v in pairs(d.ITEM) do
            local id = "i" .. tostring(k)
            local kind = (v.kind == "食物" and "med") or (v.kind == "丹药" and "med")
                      or (v.kind == "武器" and "weapon") or (v.kind == "防具" and "armor")
                      or ((v.kind == "秘籍" or v.kind == "书") and "book") or "misc"
            DATA.item[id] = { name = v.name, kind = kind, price = tonumber(v.price) or 0,
                              heal = tonumber(v.exp) or 0, desc = v.desc or "" }
        end
    end
    if d.KF then
        for k, v in pairs(d.KF) do
            if v.name and v.name ~= "" and v.name ~= "NONE" then
                DATA.art["k" .. tostring(k)] = {
                    name = v.name, dmg = 4 + math.floor((tonumber(v.level) or 1) * 2),
                    cost = 2 + math.floor((tonumber(v.level) or 1)), hit = 5,
                    maxlv = 6, weapon = (v.weapon == 1 or v.weapon == true),
                }
            end
        end
    end
end

-- ============================================================================
-- BATTLE / INV / SKILL / DLG / SAVE — 与上一版一致 (数值公式见计划书 §2.9)
-- ============================================================================
BATTLE.active = false
local function pick_mob()
    local d = gdata()
    if not d or not d.NPC then return nil end
    local keys, n = {}, 0
    for k in pairs(d.NPC) do n = n + 1; keys[n] = k end
    if n == 0 then return nil end
    for _ = 1, 12 do
        local v = d.NPC[keys[((tick_n * 7919 + n) % n) + 1]]
        if v and (v.maxhp or 0) > 0 and v.name and v.name ~= "" and v.name ~= "NONE" then
            return { name = v.name, hp = v.maxhp, maxhp = v.maxhp,
                     dmg = (v.dmg or 0) + 2, def = v.def or 0,
                     exp = v.exp or 6, silver = math.max(1, math.floor((v.exp or 6) / 2)) }
        end
    end
    return nil
end
function BATTLE.start(mob)
    BATTLE.active, BATTLE.mob, BATTLE.round, BATTLE.sel = true, mob, 1, 1
    BATTLE.acts = { "普通攻击", "使用武功", "服丹药", "逃跑" }
    LOG.push("遭遇 " .. mob.name .. "（气血 " .. mob.hp .. "）")
    app.tone_async(420, 80, 45)
    GAME.dirty = true
end
local function pstats()
    local s = { atk = 3 + math.floor(P.lv * 1.2), def = 1 + math.floor(P.lv * 0.6),
                spd = 5 + P.attr.spd * 2, hit = 75, str = 3 + P.attr.str }
    local w = P.equip.weapon and DATA.item[P.equip.weapon]
    local a = P.equip.armor and DATA.item[P.equip.armor]
    if w then s.atk = s.atk + (w.atk or 0) end
    if a then s.def = s.def + (a.def or 0) end
    return s
end
local function strike(atk, def, dmgbase, hitb)
    local hit = 0.75 + ((atk.spd or 5) - (def.spd or 5)) * 0.02 + (hitb or 0) / 100
    if hit < 0.25 then hit = 0.25 elseif hit > 0.97 then hit = 0.97 end
    if math.random() > hit then return 0 end
    local d = dmgbase + math.floor((atk.str or 3) / 3) - math.floor((def.def or 1) / 2)
    if d < 1 then d = 1 end
    return d
end
function BATTLE.player_action(i)
    local st, m = pstats(), BATTLE.mob
    local md = { atk = 2 + P.lv, def = 1 + math.floor(P.lv / 3), spd = 5, str = 3 + math.floor(P.lv / 2) }
    if i == 1 then
        local d = strike(st, md, 3, 0)
        if d == 0 then LOG.push("你的普通攻击落空了。")
        else m.hp = m.hp - d; LOG.push("你普通攻击造成 " .. d .. " 点伤害。") end
    elseif i == 2 then
        local aid, best = nil, 0
        for _, id in ipairs(P.arts) do
            local a = DATA.art[id]
            if a and a.dmg * (P.art_lv[id] or 1) > best then aid, best = id, a.dmg * (P.art_lv[id] or 1) end
        end
        local a = aid and DATA.art[aid]
        if not a then LOG.push("你还没学会招式。")
        elseif P.mp < a.cost then LOG.push("内力不足（需 " .. a.cost .. "）")
        else
            P.mp = P.mp - a.cost
            local lv = P.art_lv[aid] or 1
            local d = strike(st, md, a.dmg + (lv - 1) * 2, a.hit)
            if d == 0 then LOG.push(a.name .. " 落空！")
            else m.hp = m.hp - d; LOG.push(a.name .. " (Lv" .. lv .. ") 造成 " .. d .. " 点伤害。") end
        end
    elseif i == 3 then
        if INV.use_first_med() then LOG.push("你服下丹药，气息一畅。")
        else LOG.push("身上没有丹药。") end
    else
        if math.random() < 0.45 + P.attr.spd * 0.05 then
            LOG.push("你脱出战圈。"); BATTLE.active = false
            app.tone_async(300, 70, 40); return
        end
        LOG.push("逃跑失败！")
    end
    if m.hp <= 0 then
        LOG.push("你击败了 " .. m.name .. "！")
        P.exp = P.exp + m.exp; P.silver = P.silver + m.silver
        LOG.push("获得经验 " .. m.exp .. "，银子 " .. m.silver)
        BATTLE.active = false; GAME.gain_exp(); SAVE.mark()
        app.tone_async(900, 80, 50); return
    end
    local d2 = strike(md, st, 2 + math.floor(P.lv / 2), 0)
    if d2 == 0 then LOG.push(m.name .. " 扑了个空。")
    else
        P.hp = P.hp - d2
        LOG.push(m.name .. " 反击，你受 " .. d2 .. " 点伤。")
        app.tone_async(220, 70, 50)
        if P.hp <= 0 then
            P.hp = 0; LOG.push("你被击倒……醒来已回到原处。")
            BATTLE.active = false; GAME.respawn(); return
        end
    end
    BATTLE.round = BATTLE.round + 1
    GAME.dirty = true
end

function INV.add(id, n)
    n = n or 1
    local d = DATA.item[id]
    if not d then return false end
    if d.kind == "med" or d.kind == "book" then
        for i = 1, #P.bag do if P.bag[i].id == id then P.bag[i].n = P.bag[i].n + n; return true end end
    end
    if #P.bag >= 20 then LOG.push("行囊已满。") return false end
    P.bag[#P.bag + 1] = { id = id, n = n }
    SAVE.mark(); return true
end
function INV.del(id, n)
    n = n or 1
    for i = 1, #P.bag do
        if P.bag[i].id == id then
            P.bag[i].n = P.bag[i].n - n
            if P.bag[i].n <= 0 then table.remove(P.bag, i) end
            SAVE.mark(); return true
        end
    end
    return false
end
function INV.use_first_med()
    for i, it in ipairs(P.bag) do
        local d = DATA.item[it.id]
        if d and d.kind == "med" then
            if d.heal then P.hp = math.min(P.hpMax, P.hp + d.heal) end
            if d.mp then P.mp = math.min(P.mpMax, P.mp + d.mp) end
            INV.del(it.id, 1); return true
        end
    end
    return false
end
function INV.use(id)
    local d = DATA.item[id]
    if not d then return end
    if d.kind == "med" then
        if d.heal then P.hp = math.min(P.hpMax, P.hp + d.heal) end
        if d.mp then P.mp = math.min(P.mpMax, P.mp + d.mp) end
        LOG.push("服下 " .. d.name)
        INV.del(id, 1); app.tone_async(700, 70, 40)
    elseif d.kind == "weapon" then
        P.equip.weapon = id; LOG.push("装备武器：" .. d.name); app.tone_async(600, 70, 40)
    elseif d.kind == "armor" then
        P.equip.armor = id; LOG.push("穿上防具：" .. d.name); app.tone_async(600, 70, 40)
    elseif d.kind == "book" then
        local learn = d.learn or ("k" .. tostring(math.random(1, 20)))
        if not DATA.art[learn] then LOG.push("这门武功你参不透。") return end
        if P.art_lv[learn] then LOG.push("已学过 " .. DATA.art[learn].name)
        else
            P.arts[#P.arts + 1] = learn; P.art_lv[learn] = 1
            LOG.push("习得 " .. DATA.art[learn].name .. "！")
            app.tone_async(1000, 90, 50); INV.del(id, 1)
        end
    else LOG.push("这个用不上。") end
    SAVE.mark(); GAME.dirty = true
end
function SKILL.up(artid)
    local a = DATA.art[artid]
    if not a then return end
    local lv = P.art_lv[artid] or 0
    if lv >= a.maxlv then LOG.push(a.name .. " 已至化境。") return end
    local cost = 2 + lv
    if P.points < cost then LOG.push("修为不足（需 " .. cost .. " 点）") return end
    P.points = P.points - cost
    P.art_lv[artid] = lv + 1
    LOG.push(a.name .. " 提升到 Lv" .. (lv + 1) .. "！")
    app.tone_async(1100, 80, 50); SAVE.mark(); GAME.dirty = true
end

-- 与 NPC 交谈: 姓名/门派/气血/背景描述**都取自原站 npc.json** (真数据, 不是我编的)。
--   ⚠ 踩过的坑: D.NPC 是**数字键**表 ([153] = 丁香姨), 早先按 tostring 查 → 永远查不到,
--     于是所有 NPC 都退化成 "路人<编号>" 显示出来。数字/字符串两种键都试一次, 不再退化。
DLG.on = false
local function npc_info(id)
    local d = gdata()
    if not d or not d.NPC then return nil end
    return d.NPC[id] or d.NPC[tostring(id)]
end
function DLG.open(npc)
    DLG.npc = npc
    DLG.sel = 1
    local info = npc_info(npc.npc)
    DLG.name = (info and info.name) or ("江湖人" .. npc.npc)
    DLG.pai = (info and info.pai) or "0"
    DLG.info = info
    DLG.opts = {
        { "交谈" },
        { "请教" },
        { "切磋" },
        { "离去" },
    }
    DLG.on = true
    if info then
        local hp = tonumber(info.maxhp) or 0
        DLG.text = "气血 " .. hp .. (tonumber(info.pai) and tonumber(info.pai) > 0
                   and ("   门派 " .. info.pai) or "")
    else
        DLG.text = "（此人来历不详）"
    end
    GAME.dirty = true
end
function DLG.choose()
    local o = DLG.opts[DLG.sel]
    if not o then return end
    local info = DLG.info
    if DLG.sel == 1 then
        -- 原站每个 NPC 都有一句"message"背景描述, 直接用它 (最忠于原作)
        if info and info.msg and info.msg ~= "" and info.msg ~= "NONE!!" then
            LOG.push(info.msg)
        else
            LOG.push(DLG.name .. "：" .. "江湖路远，后会有期。")
        end
        app.tone_async(760, 60, 35)
    elseif DLG.sel == 2 then
        LOG.push(DLG.name .. "：" .. "武学之道，先练桩马，再谈招式。")
    elseif DLG.sel == 3 then
        -- 切磋: 人还是原站那个人 (名字/精灵/门派), 但**体力按玩家等级缩放** ——
        -- 原站高手动辄 400+ 体力, 1 级玩家 (30 体力) 上去就是送死, 不适合单机前期。
        local base = (info and tonumber(info.maxhp) or 0)
        local hp = 24 + P.lv * 22
        if base > 0 and base < hp then hp = base end
        local m = { name = DLG.name, hp = hp, maxhp = hp,
                    dmg = 3 + P.lv, def = 1,
                    exp = 8 + P.lv * 3, silver = 4 + P.lv * 2 }
        DLG.on = false
        BATTLE.start(m)
        return
    else
        DLG.on = false
    end
    GAME.dirty = true
end

local save_dirty, save_t = false, 0
function SAVE.mark() save_dirty = true end
function SAVE.write()
    local bag = {}
    for i = 1, #P.bag do bag[#bag + 1] = P.bag[i].id .. ":" .. P.bag[i].n end
    local arts = {}
    for i = 1, #P.arts do arts[#arts + 1] = P.arts[i] .. ":" .. (P.art_lv[P.arts[i]] or 0) end
    local s = table.concat({
        P.lv, P.exp, P.hpMax, P.mpMax, P.silver, P.points,
        P.attr.str, P.attr.spd, P.attr.wit,
        P.sect or "-", P.equip.weapon or "-", P.equip.armor or "-",
        table.concat(arts, "|"), table.concat(bag, "|"),
        P.map, P.x, P.y, P.gender,
    }, ",")
    save_dirty = false; save_t = app.millis()
    if not app.nvs.set_str("wxsave", s) then LOG.push("存档失败") end
end
function SAVE.load()
    local s = app.nvs.get_str("wxsave")
    if type(s) ~= "string" or s == "" then return false end
    local f = {}
    for w in string.gmatch(s, "[^,]*") do f[#f + 1] = w end
    if #f < 13 then return false end
    P.lv      = tonumber(f[1]) or 1
    P.exp     = tonumber(f[2]) or 0
    P.hpMax   = tonumber(f[3]) or 30
    P.mpMax   = tonumber(f[4]) or 10
    P.silver  = tonumber(f[5]) or 20
    P.points  = tonumber(f[6]) or 0
    P.attr    = { str = tonumber(f[7]) or 3, spd = tonumber(f[8]) or 2, wit = tonumber(f[9]) or 2 }
    P.sect    = (f[10] ~= "-") and f[10] or nil
    P.equip   = { weapon = (f[11] ~= "-") and f[11] or nil, armor = (f[12] ~= "-") and f[12] or nil }
    P.arts, P.art_lv = { "basic" }, { basic = 1 }
    if f[13] and f[13] ~= "" then
        for pair in string.gmatch(f[13], "[^|]+") do
            local id, lv = string.match(pair, "([^:]+):(%d+)")
            if id and DATA.art[id] then
                P.arts[#P.arts + 1] = id; P.art_lv[id] = tonumber(lv) or 1
            end
        end
    end
    P.bag = {}
    if f[14] and f[14] ~= "" then
        for pair in string.gmatch(f[14], "[^|]+") do
            local id, n = string.match(pair, "([^:]+):(%d+)")
            if id and DATA.item[id] then P.bag[#P.bag + 1] = { id = id, n = tonumber(n) or 1 } end
        end
    end
    if #f >= 17 then P.map = tonumber(f[15]) or P.map; P.x = tonumber(f[16]) or P.x; P.y = tonumber(f[17]) or P.y end
    if #f >= 18 then P.gender = tonumber(f[18]) or 0 end
    P.hp, P.mp = P.hpMax, P.mpMax
    return true
end
function SAVE.tick()
    if save_dirty and (app.millis() - save_t) > CFG.SAVE_GAP * 1000 then SAVE.write() end
end
function NET.emit(kind, payload) GAME.on_event(kind, payload) end

-- ============================================================================
-- UI — 原版风格界面
--   版面(linit 2026-10-03 定): **地图铺满整屏**, 不加任何横贯的上下文字条;
--   状态与日志改为**左右两列的浮层条** (每条自带白底, 压在地图上但只有字那么宽),
--   中间区域始终是完整地图 —— 这样"上下方不写说明"。
--   面板(菜单/行囊/武功/对话)仍居中, 一律"白底 + 1px 黑框"(原站 renderPlayerMenuCanvas 同一套)。
-- ============================================================================
local W, H = 400, 300
local function gt(x, y, s, inv) app.mix(x, y, s, inv) end

-- 一条"浮层条": 白底 + 1px 黑框, 把文字包住 (压在地图上仍清晰)
local function chip(x, y, s, inv)
    local w = app.mixw(s)
    app.fill(x, y, x + w + 3, y + 17, WH)
    app.rect(x, y, x + w + 3, y + 17, BK)
    app.mix(x + 2, y + 1, s, inv)
    return w
end

-- 左上角常驻: 当前地图名 (linit: 走路时始终能看到自己在哪)
local function hud_mapname()
    local m = META and META.maps and META.maps[P.map]
    local s = (m and m.name) or ("地图" .. P.map)
    chip(2, 2, s, false)
end

-- 左栏状态 (门派/气血/内力/银两) —— **只在按返回键弹出菜单时才显示**, 不再常驻。
local function hud_left()
    local y = 28
    local sn = P.sect_name or (P.sect and DATA.sect[P.sect] and DATA.sect[P.sect].name) or "无门无派"
    chip(2, y, sn .. " Lv" .. P.lv, false); y = y + 20
    local function row(icon, val)
        local w = app.mixw(val)
        app.fill(2, y, 2 + 18 + w + 3, y + 17, WH)
        app.rect(2, y, 2 + 18 + w + 3, y + 17, BK)
        art(icon, 4, y + 5)
        app.mix(23, y + 1, val, false)
        y = y + 20
    end
    row("hp", string.format("%d/%d", math.max(0, P.hp), P.hpMax))
    row("fp", string.format("%d/%d", P.mp, P.mpMax))
    chip(2, y, "银" .. P.silver, false)
end

-- 右栏: 日志 (右对齐浮层条)。**默认不显示**, 只有"最近有话说"的几秒内才出现 ——
--       常驻会挡地图 (linit: 提示别一直显示, 需要时才弹)。
--       顶部横条菜单打开时整体下移, 避免压在一起。
local LOG_SHOW_MS = 4200
local function hud_right()
    if not LOG.t0 or (app.millis() - LOG.t0) > LOG_SHOW_MS then return end
    local y0 = (GAME.state == "menu") and 26 or 3
    for i = 1, math.min(2, #LOG.buf) do
        local s = LOG.buf[i]
        if s then
            local w = app.mixw(s)
            local x = W - 4 - w - 3
            local y = y0 + (i - 1) * 20
            app.fill(x, y, W - 3, y + 17, WH)
            app.rect(x, y, W - 3, y + 17, BK)
            app.mix(x + 2, y + 1, s, false)
        end
    end
end

-- 通用列表面板 (原站菜单同一套做法: 白底 + 1px 黑框 + 选中行前面贴 ▶)
local function panel(title, rows, sel, first, total, footer)
    local n = #rows
    local lh = 20
    local pw = 300
    local ph = 30 + n * lh + 22
    if ph > 250 then ph = 250 end
    local px = math.floor((W - pw) / 2)
    local py = math.floor((H - ph) / 2) - 8
    if py < 6 then py = 6 end
    app.fill(px, py, px + pw - 1, py + ph - 1, WH)
    app.rect(px, py, px + pw - 1, py + ph - 1, BK)
    app.hline(px, px + pw - 1, py + 24, BK)
    gt(px + 8, py + 4, title, false)
    first = first or 1
    if total and total > n then
        art("pagedown", px + pw - 16, py + 6)
    end
    for i = 1, n do
        local ry = py + 28 + (i - 1) * lh
        if ry + lh > py + ph - 4 then break end
        local is_sel = (i == sel)
        -- 选中: 原版做法 —— 整行反白, 行首贴 ▶ 光标
        if is_sel then app.fill(px + 2, ry - 1, px + pw - 3, ry + 15, BK) end
        art("arrow", px + 4, ry + 3)
        gt(px + 16, ry, rows[i], is_sel)
    end
    gt(px + 8, py + ph - 18, footer or "确认=选择  返回=上一级", false)
end

function UI.bag()
    hud_left()
    local rows = {}
    for i = 1, math.max(1, #P.bag) do
        local it = P.bag[i]
        if it then
            local d = DATA.item[it.id]
            local eq = (P.equip.weapon == it.id or P.equip.armor == it.id) and "[装]" or "   "
            rows[i] = string.format("%s%s x%d", eq, d and d.name or "?", it.n)
        else rows[i] = "-" end
    end
    panel("行囊 " .. #P.bag .. "/20", rows, GAME.bag_sel, 1, nil, "确认=使用/装备  返回=关闭")
end
function UI.skill()
    local rows = {}
    for i, id in ipairs(P.arts) do
        local a = DATA.art[id]
        if a then
            rows[i] = string.format("%s Lv%d/%d  伤%d 内%d", a.name, P.art_lv[id] or 1,
                                    a.maxlv, a.dmg, a.cost)
        end
    end
    if #rows == 0 then rows[1] = "尚未习武" end
    panel("武功  修为点 " .. P.points, rows, GAME.sk_sel, 1, nil, "确认=升级  返回=关闭")
end
-- 长列表页 (集市/秘录): 分页浏览
function UI.list_page(title, all, sel)
    local VIS = 7
    local first = 1
    if #all > VIS then
        first = sel - math.floor(VIS / 2)
        if first < 1 then first = 1 end
        if first > #all - VIS + 1 then first = math.max(1, #all - VIS + 1) end
    end
    local rows = {}
    for i = first, math.min(#all, first + VIS - 1) do
        local e = all[i]
        rows[#rows + 1] = string.format("%s%s", (e.price and (e.price .. "文  ") or ""), e.name)
    end
    panel(string.format("%s  %d/%d", title, sel, #all), rows, sel - first + 1, first, #all,
          "确认=查看  返回=关闭")
end
-- ★ 顶部横条菜单 (复刻原站 renderPlayerMenuCanvas):
--   网站那个"玩家菜单"就是在 overlay 位置画一条横向菜单 —— 白底 + 1px 黑框,
--   各项横向排列、选中项前面贴一个 ▶, 而不是居中的竖排列表框。这里**居中 + 贴顶**。
--   linit 2026-10-03: "间距太大, 而且没有靠上, 我需要置顶" → 行高压到 20、贴 y=0、
--   项内边距收紧 (光标位 13 + 文字 + 4), 一整条正好 300px 上下。
function UI.topmenu(items, sel)
    local pad, aw = 5, 13              -- 左右内边距 / 光标位宽 (arrow 素材 6×9)
    local widths, total = {}, pad * 2
    for i = 1, #items do
        widths[i] = aw + app.mixw(items[i]) + 4
        total = total + widths[i]
    end
    if total > W then total = W end
    local x0 = math.floor((W - total) / 2)
    local y, hh = 0, 20                -- ★ 贴顶
    app.fill(x0, y, x0 + total, y + hh - 1, WH)
    app.rect(x0, y, x0 + total, y + hh - 1, BK)
    local x = x0 + pad
    for i = 1, #items do
        if i == sel then art("arrow", x + 1, y + 6) end   -- 6×9 光标垂直居中
        app.mix(x + aw, y + 2, items[i], false)
        x = x + widths[i]
        if x > x0 + total - 4 then break end
    end
end

-- 顶部菜单项 (返回键弹出)
GAME.menu_items = { "继续", "行囊", "武功", "休息", "打坐", "退出" }
function UI.battle()
    local m = BATTLE.mob
    local rows = BATTLE.acts
    panel(string.format("对阵  %s   气血 %d/%d", m.name, math.max(0, m.hp), m.maxhp or m.hp),
          rows, BATTLE.sel, 1, nil, "确认=出手  返回=关闭")
end
-- 对话框 (底部覆盖, 原版框线)
function UI.dialog()
    local ph = 92
    local py = H - ph
    app.fill(0, py, W - 1, H - 1, WH)
    app.rect(4, py + 2, W - 5, H - 3, BK)
    -- 头像 (原版 32×32 线稿, 男/女)
    art(P.gender == 0 and "mport" or "fport", 12, py + 8)
    gt(52, py + 6, DLG.name or "？", false)
    gt(52, py + 24, DLG.text or "", false)
    for i = 1, #DLG.opts do
        local y = py + 44 + (i - 1) * 17
        if y + 16 > H - 2 then break end
        local s = (i == DLG.sel)
        if s then app.fill(50, y - 2, W - 12, y + 14, BK) end
        art("arrow", 50, y + 3)
        gt(62, y, DLG.opts[i][1], s)
    end
end

-- ============================================================================
-- 进入选择 + 联网登录 (linit 2026-10-03: 进入时给"离线 / 联网"两个选项,
--   联网要能弹键盘输账号密码)
--   本板**没有触摸芯片**, 所以键盘靠方向键移动一个光标格, 确认键敲下去 —— 与
--   设置页的 WiFi 键盘同一套交互。光标在屏幕坐标上走, 用 app.kbd.hit() 反查键位,
--   这样不必在脚本里重抄一遍固件的键位表 (抄了必然对不上)。
-- ============================================================================
local ENT = { "单机离线", "联网登录" }
GAME.entry_sel = 1
GAME.lg_field = 1          -- 1=账号 2=密码
GAME.lg_user, GAME.lg_pass, GAME.lg_msg = "", "", ""
GAME.kbx, GAME.kby, GAME.kb_idx = 4, 0, -1

local KB_ROWS = 4
local function kb_setup()
    app.kbd.use("rows", KB_ROWS)
    GAME.kb_y = H - KB_ROWS * app.kbd.ROW
    GAME.kbx = 4
    GAME.kby = GAME.kb_y + math.floor(app.kbd.ROW / 2)
    GAME.kb_idx = app.kbd.hit(GAME.kbx, GAME.kby)
end

local function kb_move(dx, dy)
    local nx = GAME.kbx + dx * 48
    local ny = GAME.kby + dy * app.kbd.ROW
    local ok = app.kbd.hit(nx, ny) >= 0
    if not ok then                       -- 走到行外 → 换行/回绕
        if dy == 0 then
            nx = (dx > 0) and 4 or (W - 6)
            ny = GAME.kby
        else
            ny = (dy > 0) and (GAME.kb_y + math.floor(app.kbd.ROW / 2))
                           or (H - math.floor(app.kbd.ROW / 2))
            nx = 4
        end
        ok = app.kbd.hit(nx, ny) >= 0
    end
    if ok then
        GAME.kbx, GAME.kby = nx, ny
        GAME.kb_idx = app.kbd.hit(nx, ny)
    end
end

local function kb_tap()
    local i = GAME.kb_idx
    if i < 0 then return end
    local act = app.kbd.press(i)
    local ch = app.kbd.char(i)
    local function cur_field()
        return (GAME.lg_field == 1) and "user" or "pass"
    end
    if act == app.kbd.CHAR and ch then
        if cur_field() == "user" then
            if #GAME.lg_user < 24 then GAME.lg_user = GAME.lg_user .. ch end
        else
            if #GAME.lg_pass < 24 then GAME.lg_pass = GAME.lg_pass .. ch end
        end
    elseif act == app.kbd.BKSP then
        if cur_field() == "user" then GAME.lg_user = GAME.lg_user:sub(1, -2)
        else GAME.lg_pass = GAME.lg_pass:sub(1, -2) end
    elseif act == app.kbd.ENTER or act == app.kbd.TAB then
        if GAME.lg_field == 1 then GAME.lg_field = 2
        else GAME.login_submit = true end
    elseif act == app.kbd.ESC then
        GAME.login_cancel = true
    end
end

-- ============================================================================
-- 进入界面: 只留"标题 + 两张原版主角头像 + 菜单 + 操作提示"。
--   说明性文字(原版地图/原版人物/场景数)一律不显示 —— 让画面自己说话。
--   头像用原版 32×32 素材 (ART.mface 男 / ART.fface 女), 与游戏内同源。
-- ============================================================================
function UI.entry()
    app.clear(WH)

    -- 标题区: 男女主角头像夹住 24px 中文标题
    art("mface", 100, 22)
    app.zh24(152, 26, "英雄坛说", false)
    art("fface", 268, 22)

    -- 分隔线: 左右两段 + 中间一颗方块 (原版 ART.icon)
    app.hline(64, 193, 70, BK)
    app.hline(207, 336, 70, BK)
    art("icon", 197, 67)

    -- 菜单: 反白=选中, 框外左侧双箭头指示
    local y = 130
    for i = 1, #ENT do
        local s = (i == GAME.entry_sel)
        local w = app.mixw(ENT[i])
        local bw = w + 40
        local bx0 = (400 - bw) / 2
        local bx1 = bx0 + bw
        if s then
            app.fill(bx0, y - 4, bx1, y + 19, BK)
            app.mix(bx0 + 20, y, ENT[i], true)
            art("arrowdual", bx0 - 22, y + 3)
        else
            app.rect(bx0, y - 4, bx1, y + 19, BK)
            app.mix(bx0 + 20, y, ENT[i], false)
        end
        y = y + 40
    end

    -- 底部操作提示 (居中)
    local hint = "方向键选择   确认=进入   返回=退出"
    app.mix((400 - app.mixw(hint)) / 2, 250, hint, false)
end

function UI.login()
    app.clear(WH)
    app.mix(6, 4, "登录 gmud.online", false)
    local function field(y, label, val, is_cur, mask)
        local shown = mask and string.rep("*", #val) or val
        local s = label .. " " .. shown
        local w = math.max(250, app.mixw(s) + 14)
        if 6 + w > W - 6 then w = W - 12 end
        app.fill(6, y, 6 + w, y + 17, WH)
        app.rect(6, y, 6 + w, y + 17, BK)
        app.mix(9, y + 1, s, false)
        if is_cur then app.hline(9, 6 + w - 3, y + 18, BK) end
    end
    field(26, "账号", GAME.lg_user, GAME.lg_field == 1, false)
    field(48, "密码", GAME.lg_pass, GAME.lg_field == 2, true)
    if GAME.lg_msg ~= "" then app.mix(6, 72, GAME.lg_msg, false) end
    app.mix(6, 90, "方向键移光标  确认=敲键  返回=回上一步", false)
    app.kbd.draw(GAME.kb_idx)          -- 光标格会以按下态高亮
end

-- 登录提交: 现阶段做"账号记在本地 + 探一次服务器是否可达";
-- WebSocket 登录通道还没进固件 (见 MEMORY.md §11 / 本文件头的联网说明)。
local function login_submit()
    if GAME.lg_user == "" then
        GAME.lg_msg = "请先输入账号"
        return
    end
    if not app.net.is_connected() then
        GAME.lg_msg = "未联网：请先在设置里连 WiFi"
        return
    end
    GAME.lg_msg = "正在检查服务器…"
    GAME.dirty = true
    app.nvs.set_str("wx_user", GAME.lg_user)     -- ★ 只记账号, 密码不落盘
    local body = app.net.http_get("https://www.gmud.online/")
    if type(body) == "string" and #body > 0 then
        GAME.lg_msg = "服务器可达(账号已记住);WS 登录通道待接入"
    else
        GAME.lg_msg = "服务器不可达：检查 WiFi / 稍后再试"
    end
    app.redraw()
end

-- 正式开局 (离线/联网共用; online 只是记个标记, 联网权威模型待接入)
local function start_game(online)
    GAME.online = online and true or false
    if WDATA and WDATA.start then
        player_init()
        SAVE.load()
        if not P.sect then
            P.sect = "shaolin"
            local s = DATA.sect[P.sect]
            if s then
                P.hpMax = P.hpMax + s.hp; P.hp = P.hpMax
                P.attr.str = P.attr.str + s.str
                P.arts[#P.arts + 1] = s.arts[1]; P.art_lv[s.arts[1]] = 1
                LOG.push("你拜入少林，习得 " .. DATA.art[s.arts[1]].name)
            end
        end
        WORLD.enter(P.map, P.x, P.y)
    end
    GAME.state = "play"
    GAME.dirty = true
end

-- ============================================================================
-- GAME — 状态机 / 步进 / 渲染
-- ============================================================================

GAME.state = "play"      -- 直接进游戏 (on_enter 里也不再切标题页)
GAME.dirty = true
GAME.last_draw = 0
GAME.bag_sel, GAME.sk_sel, GAME.menu_sel = 1, 1, 1

-- 相机: 以玩家为中心, 夹到地图边界 (地图比屏小则居中)
local function camera()
    if not cur then return 0, 0 end
    local mw, mh = cur.w * CFG.TILE, cur.h * CFG.TILE
    local pcx = P.x * CFG.TILE + P.px + CFG.TILE / 2          -- 玩家中心 (地图像素系)
    local pcy = P.y * CFG.TILE + P.py - PLH / 2
    local cx, cy
    if mw <= W then cx = (mw - W) / 2 else cx = pcx - W / 2 end
    if mh <= H then cy = (mh - H) / 2 else cy = pcy - H / 2 end
    if mw > W then cx = math.max(0, math.min(mw - W, cx)) end
    if mh > H then cy = math.max(0, math.min(mh - H, cy)) end
    return cx, cy
end

local function scene()
    if not cur or not RES.map then
        app.clear(WH)
        return
    end
    local cx, cy = camera()
    -- 原版地图整图: **铺满 400×300** (opaque → 白底一并刷出)
    app.img.blit(RES.map, cx, cy, 0, 0, W, H)
    -- NPC (32×32, 贴格子左上角 —— 与原站 drawImage 的 sy 算法一致)
    for i = 1, #cur.npcs do
        local n = cur.npcs[i]
        local sx = n.x * CFG.TILE - cx
        local sy = n.y * CFG.TILE - cy
        if sx > -SPW and sx < W and sy > -SPH and sy < H then
            blit_spr(n.bmp, sx, sy)
        end
    end
    -- 玩家 (32×48, 脚底对齐格子下沿 —— 原站 sy = tileY*T + T - PH)
    local sx = P.x * CFG.TILE + P.px - cx
    local sy = P.y * CFG.TILE + P.py - PLH - cy
    if sx > -PLW and sx < W and sy > -PLH and sy < H then
        blit_player(P.dir, P.frame, sx, sy)
    end
end

-- 刷新策略 (linit: "要全屏刷新, 不要局部刷新留残留"):
--   ★ 每次重画都是**整屏**: app.clear(WH) → 地图整图 → NPC → 人物 → 左右浮层 → 面板。
--     所以只要"有变化才画"就绝不会留残影 —— 残留的根因是"局部只画一块", 不是"跳过静止帧"。
--   静止时不画, 是为了不白烧 CPU (实测整屏 400×300 地图 + 人物一次约 20ms)。
function GAME.render()
    local now = app.millis()
    if not GAME.dirty and (now - GAME.last_draw) < CFG.DRAW_GAP_MS then return end
    GAME.last_draw = now
    GAME.dirty = false
    if GAME.state == "entry" then UI.entry() return end
    if GAME.state == "login" then UI.login() return end
    app.clear(WH)                -- ★ 全屏刷白: 地图比屏小时两侧/上下也不留上一帧残影
    scene()
    -- 菜单贴顶占满那一条, 所以开菜单时不再画地图名 (否则两者压在一起)
    if GAME.state == "menu" then
        UI.topmenu(GAME.menu_items, GAME.menu_sel)
    else
        hud_mapname()
    end
    hud_right()
    if GAME.state == "bag"   then UI.bag()   end
    if GAME.state == "skill" then UI.skill() end
    if DLG.on then UI.dialog() end
    if BATTLE.active then UI.battle() end
end

function GAME.gain_exp()
    local need = 20 + P.lv * 15
    while P.exp >= need do
        P.exp = P.exp - need
        P.lv = P.lv + 1
        P.points = P.points + 2
        P.hpMax = P.hpMax + 4
        P.mpMax = P.mpMax + 2
        P.hp, P.mp = P.hpMax, P.mpMax
        LOG.push("升级！等级 " .. P.lv .. "，修为 +2")
        app.tone_async(1000, 90, 55)
        need = 20 + P.lv * 15
    end
    SAVE.mark()
end
function GAME.respawn()
    if WDATA and WDATA.start then WORLD.enter(WDATA.start.map, WDATA.start.x, WDATA.start.y) end
    P.hp = math.max(1, math.floor(P.hpMax / 2))
    P.mp = P.mpMax
    BATTLE.active, DLG.on = false, false
    SAVE.mark()
end
function GAME.on_event(kind) LOG.push(kind) end

-- 走路: 4 子步 × 8px, 每子步 40ms (原版 STEPS/PX_STEP/STEP_MS)
local function begin_step(dir)
    local dx = (dir == DIR_RIGHT) and 1 or ((dir == DIR_LEFT) and -1 or 0)
    local dy = (dir == DIR_DOWN) and 1 or ((dir == DIR_UP) and -1 or 0)
    local nx, ny = P.x + dx, P.y + dy
    P.dir = dir
    if WORLD.blocked(nx, ny) then
        -- 撞墙: 原地转向 (与原站一致: 走不动也播一次步态)
        P.moving, P.step, P.step_t = true, 0, 0
        P.tx, P.ty = P.x, P.y
        return
    end
    P.moving, P.step, P.step_t = true, 0, 0
    P.tx, P.ty = nx, ny
end

function GAME.step(dt)
    tick_n = tick_n + 1
    local moved = false
    if P.moving then
        -- ★ 走不动 = 原地转身: 只在"目标格与当前格不同"时才推进像素偏移。
        --   否则会先滑到墙里再瞬间弹回 (用户 2026-10-03 反馈的"遇到不能走的地方还会往前走
        --   然后瞬间弹回来")。原站也是这么做的 (walkInPlace 时不累加 px/py)。
        local inplace = (P.x == P.tx and P.y == P.ty)
        P.step_t = P.step_t + dt
        while P.step_t >= CFG.STEP_MS and P.moving do
            P.step_t = P.step_t - CFG.STEP_MS
            P.step = P.step + 1
            local seq = { 1, 2, 0, 1 }
            P.frame = seq[P.step] or 0
            if not inplace and P.step >= 1 and P.step <= CFG.STEP_SUB then
                local dx = (P.dir == DIR_RIGHT) and 1 or ((P.dir == DIR_LEFT) and -1 or 0)
                local dy = (P.dir == DIR_DOWN) and 1 or ((P.dir == DIR_UP) and -1 or 0)
                P.px = P.px + dx * CFG.STEP_PX
                P.py = P.py + dy * CFG.STEP_PX
            end
            if P.step >= CFG.STEP_SUB then
                P.moving, P.step = false, 0
                local was = (P.x ~= P.tx) or (P.y ~= P.ty)
                P.x, P.y = P.tx, P.ty
                P.px, P.py = 0, CFG.TILE
                moved = true
                if was then
                    -- 踩到门 → 换场景 (原站 checkDoor: t==2 → doors[i])
                    local d = WORLD.door_at(P.x, P.y)
                    if d then
                        WORLD.enter(d.toMap, d.x, d.y)
                        app.tone_async(520, 60, 35)
                        SAVE.mark()
                    else
                        -- NPC 已改为不挡路, 所以不会再有"被挡"的情况; 这里不再刷提示
                        SAVE.mark()
                    end
                end
            end
        end
    elseif DLG.on or BATTLE.active or GAME.state ~= "play" then
        -- 对话/战斗/菜单时不走动
    else
        local dir = nil
        if app.key_down(A_LEFT) then dir = DIR_LEFT
        elseif app.key_down(A_RIGHT) then dir = DIR_RIGHT
        elseif app.key_down(A_UP) then dir = DIR_UP
        elseif app.key_down(A_DOWN) then dir = DIR_DOWN end
        if dir then begin_step(dir); moved = true end
    end
    SAVE.tick()
    -- 事件驱动重绘: 走动中每帧刷新; 静止时每 ~0.4s 刷一次状态栏
    if moved or P.moving or (tick_n % 24 == 0) then GAME.dirty = true end
end

local ok_t, ok_pend, ok_long = 0, false, false
local function sel_list_len()
    if GAME.state == "bag" then return math.max(1, #P.bag) end
    if GAME.state == "skill" then return #P.arts end
    return 0
end

-- ============================================================================
-- 契约函数 (必须全局: on_enter / render / poll / action / on_exit)
-- ============================================================================
function on_enter()
    ensure_data()
    if not RES.enter() then
        LOG.push(MAPDIR .. (RES.err or "?") .. " 缺失, 先跑 tools/wuxia_pack.py")
    end
    -- ★ 先给"离线 / 联网"选择 (linit 要求), 不直接开局; 地图等选完再加载, 开局更快
    kb_setup()
    GAME.entry_sel = 1
    GAME.lg_msg = ""
    GAME.state = "entry"
    GAME.dirty = true
end
function on_exit()
    if save_dirty then SAVE.write() end
    app.img.freeall()
end
function render() GAME.render() end

-- 找可交谈的 NPC: 脚下 → 面朝 → 其余三面 (顺序即优先级)。
-- 单机版的 NPC 不挡路、位置又是自定的, 若只认"面朝的那一格", 玩家会经常觉得"按了没反应"。
local function npc_talkable()
    if not cur then return nil end
    local n = WORLD.npc_at(P.x, P.y)
    if n then return n end
    local order = {
        { DIR_UP, 0, -1 }, { DIR_DOWN, 0, 1 }, { DIR_LEFT, -1, 0 }, { DIR_RIGHT, 1, 0 },
    }
    -- 面朝的那个排最前
    local dx0 = (P.dir == DIR_RIGHT) and 1 or ((P.dir == DIR_LEFT) and -1 or 0)
    local dy0 = (P.dir == DIR_DOWN) and 1 or ((P.dir == DIR_UP) and -1 or 0)
    local n1 = WORLD.npc_at(P.x + dx0, P.y + dy0)
    if n1 then return n1 end
    for i = 1, #order do
        local n2 = WORLD.npc_at(P.x + order[i][2], P.y + order[i][3])
        if n2 then return n2 end
    end
    return nil
end

function action(a)
    GAME.dirty = true
    if type(a) ~= "number" or a < 1 or a > 7 then return true end

    -- ① 进入选择: 单机离线 / 联网登录
    if GAME.state == "entry" then
        if a == A_UP then GAME.entry_sel = math.max(1, GAME.entry_sel - 1)
        elseif a == A_DOWN then GAME.entry_sel = math.min(#ENT, GAME.entry_sel + 1)
        elseif a == A_OK then
            if GAME.entry_sel == 1 then
                start_game(false)
            else
                GAME.state = "login"
                GAME.lg_field = 1
                GAME.lg_msg = ""
                GAME.lg_user = app.nvs.get_str("wx_user") or ""
                GAME.lg_pass = ""
                kb_setup()
            end
        elseif a == A_BACK then app.exit() end
        return true
    end

    -- ② 联网登录: 方向键移键盘光标, 确认=敲键, 返回=回上一步
    if GAME.state == "login" then
        if a == A_UP then kb_move(0, -1)
        elseif a == A_DOWN then kb_move(0, 1)
        elseif a == A_LEFT then kb_move(-1, 0)
        elseif a == A_RIGHT then kb_move(1, 0)
        elseif a == A_OK then
            kb_tap()
            if GAME.login_submit then
                GAME.login_submit = false
                login_submit()
            elseif GAME.login_cancel then
                GAME.login_cancel = false
                GAME.state = "entry"
            end
        elseif a == A_BACK then GAME.state = "entry" end
        return true
    end

    -- 对话最高优先
    if DLG.on then
        local n = #DLG.opts
        if a == A_UP then DLG.sel = math.max(1, DLG.sel - 1)
        elseif a == A_DOWN then DLG.sel = math.min(n, DLG.sel + 1)
        elseif a == A_OK then DLG.choose()
        elseif a == A_BACK then DLG.on = false end
        return true
    end

    -- 战斗
    if BATTLE.active then
        if a == A_UP then BATTLE.sel = math.max(1, BATTLE.sel - 1)
        elseif a == A_DOWN then BATTLE.sel = math.min(#BATTLE.acts, BATTLE.sel + 1)
        elseif a == A_OK then BATTLE.player_action(BATTLE.sel)
        elseif a == A_BACK then BATTLE.active = false; LOG.push("你收起招式。") end
        return true
    end

    -- 二级页面: 返回键菜单是**顶部横条**(左右选), 行囊/武功是居中面板(上下选)
    if GAME.state == "menu" or GAME.state == "bag" or GAME.state == "skill" then
        local n = sel_list_len()
        local mn = #GAME.menu_items
        if a == A_LEFT or a == A_UP then
            if GAME.state == "menu" then GAME.menu_sel = (GAME.menu_sel - 2) % mn + 1
            elseif GAME.state == "bag" then GAME.bag_sel = math.max(1, GAME.bag_sel - 1)
            else GAME.sk_sel = math.max(1, GAME.sk_sel - 1) end
        elseif a == A_RIGHT or a == A_DOWN then
            if GAME.state == "menu" then GAME.menu_sel = GAME.menu_sel % mn + 1
            elseif GAME.state == "bag" then GAME.bag_sel = math.min(n, GAME.bag_sel + 1)
            else GAME.sk_sel = math.min(n, GAME.sk_sel + 1) end
        elseif a == A_BACK then
            -- ★ 顶层菜单里再按一次返回 = 要退出程序 → 交给桥层弹「退出程序?」确认框
            --   (用户规范: 所有游戏/独立程序按退出键都要弹确认)。
            --   游戏内按返回仍是"呼出顶部菜单", 这一层的语义不变。
            if GAME.state == "menu" then app.exit() else GAME.state = "play" end
        elseif a == A_OK then
            if GAME.state == "menu" then
                local s = GAME.menu_sel
                if s == 1 then GAME.state = "play"
                elseif s == 2 then GAME.state = "bag"; GAME.bag_sel = 1
                elseif s == 3 then GAME.state = "skill"; GAME.sk_sel = 1
                elseif s == 4 then
                    if P.silver >= 5 then
                        P.silver = P.silver - 5
                        P.hp = P.hpMax
                        LOG.push("休息片刻，气血全复（花 5 文）")
                    else LOG.push("银子不够（需 5 文）") end
                    GAME.state = "play"
                elseif s == 5 then
                    P.mp = P.mpMax
                    LOG.push("打坐调息，内力全复。")
                    GAME.state = "play"
                else app.exit() end
            elseif GAME.state == "bag" then
                local it = P.bag[GAME.bag_sel]
                if it then INV.use(it.id) else LOG.push("这一格是空的。") end
            else
                local id = P.arts[GAME.sk_sel]
                if id then SKILL.up(id) else LOG.push("还没有武功可提升。") end
            end
        end
        return true
    end

    -- 探索态
    -- 方向键**单击 = 走一格**(按住则连续走, 由 GAME.step 里的 app.key_down 负责)。
    -- 两种都留: 触屏/手柄轻点一下就能走, 不用一直按着。
    if a == A_LEFT or a == A_RIGHT or a == A_UP or a == A_DOWN then
        if not P.moving then
            begin_step(a == A_LEFT and DIR_LEFT or a == A_RIGHT and DIR_RIGHT
                       or a == A_UP and DIR_UP or DIR_DOWN)
        end
    elseif a == A_OK then
        -- 与身边任意 NPC 交谈 (对话框从底部弹出)
        local n = npc_talkable()
        if n then DLG.open(n)
        else
            local d = WORLD.door_at(P.x, P.y)
            if d then WORLD.enter(d.toMap, d.x, d.y); SAVE.mark() end
        end
    elseif a == A_BACK then
        GAME.state = "menu"; GAME.menu_sel = 1
    end
    return true
end

-- ★ 反"果冻效应" (linit 2026-10-03: "画面移动感觉有果冻效应"):
--   原因是一次渲染要 20ms+, 帧间隔不稳定; 旧的补帧逻辑 `dt>200 → dt=STEP_MS` 之后
--   还会在 while 里**一次连推最多 6 个子步** → 玩家一帧里瞬移 48px 再停住, 视觉上就是
--   "被拉扯/回弹"的果冻感。现在: 慢帧只当"这一帧只走一点点"(dt 夹到 16ms), **绝不补步**,
--   宁可走慢一点也不要位置跳变。渲染节流也压到 16ms, 移动时逐帧刷新。
local hb = 0
function poll()
    local now = app.millis()
    local dt = now - (ok_t or 0)
    if dt <= 0 or dt > 120 then dt = 16 end          -- 慢帧/首帧: 只当一帧, 不补步
    ok_t = now
    if GAME.state == "play" and not DLG.on and not BATTLE.active then GAME.step(dt) end
    hb = hb + 1
    if hb % 16 == 0 then GAME.dirty = true end       -- 约 0.5s 一次, 支撑浮层自动隐藏
end
