--@lapp v1
--@app name=英雄坛说
--@app cat=游戏
--@app desc=离线单机 + 在线(实验): 63 张原版场景/角色创建/师承/战斗/集市/3 存档位
--@app version=3
--@app build=202610311425
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
--   · 内容  —— **原版数据**: 物品/功夫/NPC 全部来自 items.json / kf.json / npc.json
--
-- 与原版不一致的地方 (如实标注, 不假装是原版):
--   · NPC 的**地图与坐标**原站没有 (maps.json 的 npcs 只有 bmp/npcId, 位置由服务端 WS 下发,
--     客户端代码里那个变量从未被使用)。单机版用确定性哈希把 NPC 摊到各图可行走格上,
--     见 tools/wuxia_world_gen.py 的说明。
--   · 门派名表/任务/赌博/强化/仓库 等**服务端逻辑**没有公开数据, 单机版不实现、不造假
--     (清单见 docs/专题/应用资料/英雄坛说.md §4)。
--   · 2026-10-31: **彻底去掉联网登录** —— 账号/云端存档/同步都是网站服务, 不是游戏本体;
--     现在标题页只有「初入江湖 / 继续前缘 / 离开」。
--
-- 版面 (400×300 全屏, 系统 16px 字体):
--   地图铺满整屏; 左上角一枚地图名浮层; 日志浮层在 y=24 起(不压地图名);
--   返回键呼出的顶部横条菜单贴 y=0。
--
-- 素材: 不在本文件里! 见 RES 段的 ROOTS (资源包 英雄坛说.zip → /sdcard/apps/app/wuxia/)。
--
-- 模块 (单文件内局部表, 只允许向下依赖, 禁止成环):  RES / WORLD / P / NPC / LOG /
--   BATTLE / INV / SKILL / DLG / SAVE / DATA / CHAR / SHOP / UI / GAME
-- 实现说明: docs/专题/应用资料/英雄坛说.md
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

-- ★ 素材根目录解析 (2026-10-31 修「打开游戏没有任何画面」)
--   根因: 应用是**单文件 LAPP**, 而素材 (map.lua/world.lua/data.lua/63 张地图/4 张图集,
--         共 103 个文件 ≈1.9MB) 是**另外的资源包**, 不在 .lua 里。原来只认写死的
--         "/store/wuxia/" —— 那是 SPIFFS(只读镜像) 路径, 出厂 /store 是空的
--         (sync_apps.py BUILD_INTERNAL=False), 商店里也没有这条资源的资产包 ⇒
--         真机上 RES.enter() 必然失败, WORLD 为 nil, 进入后只有一片白。
--   现在: 按**优先级依次探测**三个可能位置, 谁先有 map.lua+world.lua 用谁。
--         资源包按 store/index.json 的 type=asset 装到 /sdcard/apps/app/wuxia/
--         (下载到这里 = 可写、可重装, 且不用改只读的 SPIFFS)。
local ROOTS = {
    "/sdcard/apps/app/wuxia/",     -- ① 商店资源包安装位置 (常规)
    "/sdcard/wuxia/",              -- ② 手动放到卡根的手工安装位
    "/store/wuxia/",               -- ③ 开发位: 整片烧写时 SPIFFS 里带的素材
}
local MAPDIR = ROOTS[1]
local META = nil               -- wuxia/map.lua (由 pack 生成, 含 dir/尺寸/地图名)

local function pick_root()
    for i = 1, #ROOTS do
        local r = ROOTS[i]
        if app.fs.exists(r .. "map.lua") and app.fs.exists(r .. "world.lua") then
            return r
        end
    end
    return nil
end

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
    local root = pick_root()
    if not root then
        -- 一个都没有: 记清楚"要找什么、该放哪儿", 由 UI.err 整屏告诉用户
        RES.err = "素材包未安装"
        RES.need = ROOTS[1]
        return false
    end
    MAPDIR = root
    META  = load_mod(MAPDIR .. "map.lua")
    WDATA = load_mod(MAPDIR .. "world.lua")
    if not META or not WDATA then
        RES.err = "素材包损坏: " .. ((not META and "map.lua") or "world.lua")
        return false
    end
    MAPDIR = META.dir or MAPDIR
    -- ★ META.dir 是打包机里写死的 /store/wuxia/ —— 若素材装在 TF 卡上, 必须改回实际根目录,
    --   否则地图/图集还是会去 SPIFFS 找(必失败), 表现为"标题能进、一进地图就黑"。
    MAPDIR = pick_root() or MAPDIR
    WORLD.doors = WDATA.doors          -- 门表接线 (WORLD.door_at 要用)
    RES.spr = app.img.load(MAPDIR .. "spr.bmp")
    RES.pl  = app.img.load(MAPDIR .. "player.bmp")
    RES.sprm = app.img.load(MAPDIR .. "sprm.bmp")     -- 精灵底版 (mask): 刷白用
    RES.plm  = app.img.load(MAPDIR .. "playerm.bmp")  -- 玩家底版
    if not RES.spr then RES.err = "图集缺失: spr.bmp" ; return false end
    if not RES.pl  then RES.err = "图集缺失: player.bmp"; return false end
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
P.name, P.master, P.slot, P.ready = "无名侠客", nil, 1, false
-- 创建角色时待分配点 (初值 5 点, 见 CHAR)
P.free = 0
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
    P.name, P.master, P.ready = "无名侠客", nil, false
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
-- DATA — 原站内容数据 (物品/功夫/NPC), 从素材根目录的 data.lua 读
--   210 物品 / 211 功夫 / 453 NPC 全是 gmud.online /data/*.json 的原值。
-- ============================================================================
local DATA = { item = {}, art = {}, npc = {}, order = {}, npc_n = 0 }
local GD, GD_done = nil, false
local function gdata()
    if GD_done then return nil end          -- 已经折叠进 DATA.*, 原表不再需要
    if GD ~= nil then return GD end
    GD = false
    -- data.lua 与 map.lua 同目录; 不写死路径 (可能装在 TF 卡或 SPIFFS)
    local root = pick_root() or MAPDIR
    local d = load_mod(root .. "data.lua")
    if d then GD = d end
    return GD
end

-- 物品兜底**只在数据文件缺失时**用 (保证素材没装也能看到提示之外的界面, 不冒充原版内容)
DATA.item = {
    med1 = { name = "金创药", kind = "med", price = 20, heal = 15 },
    wpn1 = { name = "铁剑", kind = "weapon", price = 80, atk = 6 },
    arm1 = { name = "布衣", kind = "armor", price = 50, def = 4 },
}
DATA.art = {
    basic = { name = "基本拳脚", dmg = 3, cost = 0, hit = 0, maxlv = 5, weapon = false },
}

-- 原站 attrs 字段是字符串 "[160, 0, 0, 0]" / "[\"book1_skill\", 0, 0, 0]"
-- → 只取**前两个整数**当气血/内力恢复量; 书籍项里是字符串, 取不到就是 0。
local function attrs2(s)
    if type(s) ~= "string" then return 0, 0 end
    local nums = {}
    for n in string.gmatch(s, "%-?%d+") do nums[#nums + 1] = tonumber(n) end
    return nums[1] or 0, nums[2] or 0
end

local function ensure_data()
    local d = gdata()
    if not d then return end
    -- ---- 物品: 原站字段 → 游戏字段 (价格/恢复量/描述都取原值) ----
    if d.ITEM then
        for k, v in pairs(d.ITEM) do
            if v.name and v.name ~= "" and v.name ~= "NONE!!" and v.kind ~= "NULL" then
                local id = "i" .. tostring(k)
                local kind = (v.kind == "食物" and "med") or (v.kind == "丹药" and "med")
                          or (v.kind == "武器" and "weapon") or (v.kind == "防具" and "armor")
                          or ((v.kind == "书籍" or v.kind == "秘籍" or v.kind == "书") and "book")
                          or "misc"
                local a1, a2 = attrs2(v.attrs)
                local price = tonumber(v.price) or 0
                DATA.item[id] = {
                    name = v.name, kind = kind, price = price,
                    heal = (kind == "med") and a1 or 0,
                    mp   = (kind == "med") and a2 or 0,
                    -- 武器/防具: 原站没有单独的攻击/防御字段, 按品级 price 折算 —— 这是**折算**
                    -- 不是原值, 在报告里标为【推断】; 平衡参数集中在 CFG.EQ_ATK/EQ_DEF 便于调。
                    atk  = (kind == "weapon") and math.max(1, math.floor(price / 15)) or 0,
                    def  = (kind == "armor")  and math.max(1, math.floor(price / 15)) or 0,
                    exp  = tonumber(v.exp) or 0,          -- 书籍: 原站"读它需要的经验"
                    desc = v.desc or "",
                }
                DATA.order[#DATA.order + 1] = id
            end
        end
        table.sort(DATA.order, function(a, b)
            return (DATA.item[a].price or 0) < (DATA.item[b].price or 0)
        end)
    end
    -- ---- 功夫: 211 门, 按"类型"折算伤害/内力消耗 ----
    local DMG = { ["内功"] = 0, ["轻功"] = 0, ["招架"] = 0, ["知识"] = 0, ["其他"] = 0 }
    if d.KF then
        for k, v in pairs(d.KF) do
            if v.name and v.name ~= "" and v.name ~= "NONE" then
                local t = v.type or "其他"
                local dmg = DMG[t]
                if dmg == nil then dmg = 6 end          -- 拳脚/剑法/刀法/棍法/杖法/鞭法 等
                DATA.art["k" .. tostring(k)] = {
                    name = v.name, type = t, desc = v.desc or "",
                    dmg = dmg, cost = (t == "内功") and 3 or 2,
                    hit = (t == "招架" or t == "轻功") and 8 or 5,
                    maxlv = 6, weapon = (v.weapon ~= nil and v.weapon ~= ""),
                }
            end
        end
    end
    -- ---- NPC: 只留对话要用的 4 个字段 (name/pai/maxhp/msg) ----
    -- ★ data.lua 的 D.MAP 段 (59 张图的 RLE + 交互点) 运行时**不用** (world.lua 才是
    --   碰撞/门的数据源), D.ITEM/KF 折叠完也没用了。原表整份留着白占几百 KB 的 Lua 堆,
    --   折叠完就丢: GD_done 之后 gdata() 不会再返回它。
    if d.NPC then
        for k, v in pairs(d.NPC) do
            if v.name and v.name ~= "" and v.name ~= "NONE!!" then
                local id = tonumber(k) or k
                DATA.npc[id] = { name = v.name, pai = tonumber(v.pai) or 0,
                                 maxhp = tonumber(v.maxhp) or 0,
                                 msg = (v.msg ~= "NONE!!") and v.msg or "" }
                DATA.npc_n = DATA.npc_n + 1
                DATA.npc_ids = DATA.npc_ids or {}
                DATA.npc_ids[#DATA.npc_ids + 1] = id
            end
        end
    end
    GD, GD_done = nil, true
    d = nil
end

-- ============================================================================
-- BATTLE / INV / SKILL / DLG / SAVE — 与上一版一致 (数值公式见计划书 §2.9)
-- ============================================================================
BATTLE.active = false
-- (原站没有"野外随机刷怪": 战斗一律由与 NPC 切磋触发。上一版留的 pick_mob 从未被调用,
--  已删除 —— 死代码不留在交付物里。)
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
        -- 书籍: 原站 attrs 里是 "book1_skill" 这类**服务端技能组名**, 没有到 211 门功夫的
        -- 映射表 (映射在服务端)。这里按书名/描述里的门派关键词挑同"类型"的最基础功夫 ——
        -- 【推断】的替代规则, 不是原版逻辑; 原版精确映射无法从公开数据恢复。
        if P.exp < (d.exp or 0) then
            LOG.push("阅历不足，读不懂（需经验 " .. (d.exp or 0) .. "）")
            return
        end
        local types = { { "拳", "拳脚" }, { "掌", "拳脚" }, { "指", "拳脚" }, { "腿", "拳脚" },
                        { "剑", "剑法" }, { "刀", "刀法" }, { "棍", "棍法" },
                        { "杖", "杖法" }, { "鞭", "鞭法" }, { "内", "内功" },
                        { "轻", "轻功" }, { "步", "轻功" }, { "身法", "轻功" } }
        local want = nil
        local hay = (d.name or "") .. (d.desc or "")
        for i = 1, #types do
            if hay:find(types[i][1], 1, true) then want = types[i][2]; break end
        end
        local learn = nil
        if want then
            for k, v in pairs(DATA.art) do
                if v.type == want and not P.art_lv[k] then
                    if not learn or (DATA.art[learn].cost or 0) > (v.cost or 0) then learn = k end
                end
            end
        end
        if not learn then LOG.push("此书与你的路子不合，参不透。") return end
        P.exp = P.exp - (d.exp or 0)
        P.arts[#P.arts + 1] = learn; P.art_lv[learn] = 1
        LOG.push("研读 " .. d.name .. "，习得 " .. DATA.art[learn].name .. "！")
        app.tone_async(1000, 90, 50); INV.del(id, 1)
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
-- 与 NPC 交谈: 姓名/门派/气血/背景描述**都取自原站 npc.json** (真数据, 不是我编的)。
--   ⚠ 踩过的坑: D.NPC 是**数字键**表 ([153] = 丁香姨), 早先按 tostring 查 → 永远查不到,
--     于是所有 NPC 都退化成 "路人<编号>" 显示出来。数字/字符串两种键都试一次, 不再退化。
DLG.on = false

-- 按显示宽度折行 (汉字 2 列, ASCII 1 列) —— 对话长文本分页用
-- ★ 必须先按 "\n" 分段: 换行符在 16px 字库里是**没有字形**的, 直接按字符折行
--   会把 "\n" 当成一个 1 列的普通字符, 屏幕上是 "?" (实测出过一次)。
local function wrap(s, cols)
    local out = {}
    local function push_line(cur, w)
        out[#out + 1] = table.concat(cur)
    end
    for para in (tostring(s or "") .. "\n"):gmatch("(.-)\n") do
        local cur, w, i = {}, 0, 1
        while i <= #para do
            local b = para:byte(i)
            local n = 1
            if b >= 0xF0 then n = 4 elseif b >= 0xE0 then n = 3 elseif b >= 0xC0 then n = 2 end
            local ch = para:sub(i, i + n - 1)
            local cw = (n > 1) and 2 or 1
            if w + cw > cols and #cur > 0 then push_line(cur, w); cur, w = {}, 0 end
            cur[#cur + 1] = ch; w = w + cw; i = i + n
        end
        push_line(cur, w)
    end
    if #out == 0 then out[1] = "" end
    return out
end

local function npc_info(id)
    return DATA.npc[id] or DATA.npc[tostring(id)]
end

-- 说一段话: 进对话分页 (DLG.lines/DLG.page), 同时留在日志里一行摘要
function DLG.say(text)
    DLG.lines = wrap(text, 21)          -- 21 汉字/行 × 3 行/页
    DLG.page = 1
    DLG.pages = math.max(1, math.ceil(#DLG.lines / 3))
    LOG.push(text)
end

function DLG.open(npc)
    DLG.npc = npc
    DLG.sel = 1
    local info = npc_info(npc.npc)
    DLG.name = (info and info.name) or ("江湖人" .. npc.npc)
    DLG.pai = (info and tonumber(info.pai)) or 0
    DLG.info = info
    -- 拜师/请教合并成一项: 还没师承 → "拜师"; 已是他的徒弟 → "请教"
    local is_master = (P.master ~= nil and P.master == DLG.name)
    DLG.opts = { "交谈", is_master and "请教" or "拜师", "切磋", "离去" }
    DLG.on = true
    DLG.lines, DLG.page, DLG.pages = nil, 1, 1
    -- 门派名表不在原站公开数据里 (NPC 只有 pai 编号) ⇒ 只显示编号, 不编门派名
    if info then
        local hp = tonumber(info.maxhp) or 0
        DLG.brief = "气血 " .. hp .. (DLG.pai > 0 and ("  门派#" .. DLG.pai) or "  无门派")
    else
        DLG.brief = "（此人来历不详）"
    end
    GAME.dirty = true
end

function DLG.choose()
    local info = DLG.info
    if DLG.sel == 1 then
        -- 原站每个 NPC 都有一句 msg 背景描述, 直接用它 (最忠于原作)
        local msg = (info and info.msg and info.msg ~= "" and info.msg ~= "NONE!!")
                    and info.msg or (DLG.name .. "：江湖路远，后会有期。")
        DLG.say(msg)
        app.tone_async(760, 60, 35)
    elseif DLG.sel == 2 then
        if P.master ~= DLG.name then
            -- 拜师: 记录**真实 NPC 名**为师承 (门派名表缺失, 只记 pai 编号)
            P.master, P.sect = DLG.name, (DLG.pai > 0 and tostring(DLG.pai) or nil)
            P.hpMax = P.hpMax + 6; P.mpMax = P.mpMax + 2
            P.hp, P.mp = P.hpMax, P.mpMax
            P.points = P.points + 1
            DLG.say("你向 " .. DLG.name .. " 磕了三个头，算是记了名。\n"
                    .. "气血上限 +6，内力上限 +2，修为 +1。")
            DLG.opts[2] = "请教"
            app.tone_async(980, 90, 50)
        else
            P.points = P.points + 1
            DLG.say(DLG.name .. "：" .. (info and info.msg or "武学之道，先练桩马，再谈招式。")
                    .. "\n（修为 +1）")
            app.tone_async(700, 60, 35)
        end
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
    SAVE.mark()
    GAME.dirty = true
end

-- ============================================================================
-- SAVE — 本地存档 (2026-10-31 重做: 多槽 + 版本号 + 校验 + 写后回读)
--   ① 三个独立槽 (wxs1/wxs2/wxs3) → 不同角色互不覆盖
--   ② 每条存档自带 format=2 与 fnv 校验; 校验不过**不加载**, 也不会被半截数据带崩
--   ③ 写入后立刻回读比对 —— NVS 写失败/被截断当场就知道, 不会等到下次读档才发现
--   ④ 旧版单槽 "wxsave" (逗号分隔, 无版本) 首次进游戏自动迁移到 1 号槽
-- ============================================================================
local save_dirty, save_t = false, 0
local SAVE_FMT = 2

local function fnv(s)
    local h = 2166136261
    for i = 1, #s do
        h = (h ~ s:byte(i)) & 0xFFFFFFFF
        h = (h * 16777619) & 0xFFFFFFFF
    end
    return string.format("%08x", h)
end
local function skey(slot) return "wxs" .. tostring(slot) end

function SAVE.mark() save_dirty = true end

function SAVE.serialize()
    local bag = {}
    for i = 1, #P.bag do bag[#bag + 1] = P.bag[i].id .. ":" .. P.bag[i].n end
    local arts = {}
    for i = 1, #P.arts do arts[#arts + 1] = P.arts[i] .. ":" .. (P.art_lv[P.arts[i]] or 0) end
    local body = table.concat({
        "v=" .. SAVE_FMT,
        P.name or "无名侠客", P.gender or 0, P.sect or "-", P.master or "-",
        P.map, P.x, P.y,
        P.lv, P.exp, P.hpMax, P.hp, P.mpMax, P.mp, P.silver, P.points,
        P.attr.str, P.attr.spd, P.attr.wit,
        P.equip.weapon or "-", P.equip.armor or "-",
        table.concat(arts, ";"), table.concat(bag, ";"),
    }, "|")
    return "WX" .. SAVE_FMT .. "|" .. fnv(body) .. "|" .. body
end

function SAVE.write(slot)
    slot = slot or P.slot or 1
    if not P.ready then return false end
    local s = SAVE.serialize()
    save_dirty = false; save_t = app.millis()
    if not app.nvs.set_str(skey(slot), s) then LOG.push("存档写入失败"); return false end
    local back = app.nvs.get_str(skey(slot))          -- ③ 写后回读
    if back ~= s then LOG.push("存档校验失败（写入后回读不一致）"); return false end
    P.slot = slot
    return true
end

-- 解析一条存档 → 表 (不改 P); 校验/版本不过返回 nil, 第二个返回值是原因
function SAVE.parse(s)
    if type(s) ~= "string" or s == "" then return nil, "空" end
    local f = {}
    for w in string.gmatch(s, "[^|]*") do f[#f + 1] = w end
    if f[1] ~= "WX2" or #f < 4 then return nil, "格式不符" end
    local body = s:sub(#f[1] + #f[2] + 3)
    if fnv(body) ~= f[2] then return nil, "校验和不对" end
    local t = {
        name = f[4], gender = tonumber(f[5]) or 0,
        sect = (f[6] ~= "-") and f[6] or nil, master = (f[7] ~= "-") and f[7] or nil,
        map = tonumber(f[8]) or 1, x = tonumber(f[9]) or 4, y = tonumber(f[10]) or 4,
        lv = tonumber(f[11]) or 1, exp = tonumber(f[12]) or 0,
        hpMax = tonumber(f[13]) or 30, hp = tonumber(f[14]) or 30,
        mpMax = tonumber(f[15]) or 10, mp = tonumber(f[16]) or 10,
        silver = tonumber(f[17]) or 20, points = tonumber(f[18]) or 0,
        str = tonumber(f[19]) or 3, spd = tonumber(f[20]) or 2, wit = tonumber(f[21]) or 2,
        weapon = (f[22] ~= "-") and f[22] or nil, armor = (f[23] ~= "-") and f[23] or nil,
        arts = {}, bag = {},
    }
    for pair in string.gmatch(f[24] or "", "[^;]+") do
        local id, lv = string.match(pair, "([^:]+):(%d+)")
        if id then t.arts[#t.arts + 1] = { id = id, lv = tonumber(lv) or 1 } end
    end
    for pair in string.gmatch(f[25] or "", "[^;]+") do
        local id, n = string.match(pair, "([^:]+):(%d+)")
        if id then t.bag[#t.bag + 1] = { id = id, n = tonumber(n) or 1 } end
    end
    return t
end

-- 槽位摘要 (读档列表用; 返回 nil = 空槽)
function SAVE.peek(slot)
    local t, why = SAVE.parse(app.nvs.get_str(skey(slot)))
    if not t then return nil, why end
    return t
end

-- 旧版单槽迁移: "lv,exp,hpMax,mpMax,silver,points,str,spd,wit,sect,weapon,armor,arts,bag,map,x,y,gender"
function SAVE.migrate_v1()
    local s = app.nvs.get_str("wxsave")
    if type(s) ~= "string" or s == "" then return false end
    if SAVE.peek(1) then return false end                  -- 1 号槽已有新档, 不动
    local f = {}
    for w in string.gmatch(s, "[^,]*") do f[#f + 1] = w end
    if #f < 13 then return false end
    local old = P                               -- 借 P 组装, 完事由调用方 player_init 复位
    P.name = "旧档侠客"
    P.lv, P.exp = tonumber(f[1]) or 1, tonumber(f[2]) or 0
    P.hpMax, P.mpMax = tonumber(f[3]) or 30, tonumber(f[4]) or 10
    P.hp, P.mp = P.hpMax, P.mpMax
    P.silver, P.points = tonumber(f[5]) or 20, tonumber(f[6]) or 0
    P.attr = { str = tonumber(f[7]) or 3, spd = tonumber(f[8]) or 2, wit = tonumber(f[9]) or 2 }
    P.sect = (f[10] ~= "-") and f[10] or nil
    P.equip = { weapon = (f[11] ~= "-") and f[11] or nil, armor = (f[12] ~= "-") and f[12] or nil }
    P.arts, P.art_lv = { "basic" }, { basic = 1 }
    for pair in string.gmatch(f[13] or "", "[^|]+") do
        local id, lv = string.match(pair, "([^:]+):(%d+)")
        if id and DATA.art[id] then P.arts[#P.arts + 1] = id; P.art_lv[id] = tonumber(lv) or 1 end
    end
    P.bag = {}
    for pair in string.gmatch(f[14] or "", "[^|]+") do
        local id, n = string.match(pair, "([^:]+):(%d+)")
        if id and DATA.item[id] then P.bag[#P.bag + 1] = { id = id, n = tonumber(n) or 1 } end
    end
    if #f >= 17 then P.map = tonumber(f[15]) or 1; P.x = tonumber(f[16]) or 4; P.y = tonumber(f[17]) or 4 end
    if #f >= 18 then P.gender = tonumber(f[18]) or 0 end
    P.ready = true
    local ok = SAVE.write(1)
    _ = old
    return ok
end

-- 把解析出来的档写进 P (继续游戏)
function SAVE.apply(t)
    P.name, P.gender = t.name, t.gender
    P.sect, P.master = t.sect, t.master
    P.lv, P.exp = t.lv, t.exp
    P.hpMax, P.hp, P.mpMax, P.mp = t.hpMax, t.hp, t.mpMax, t.mp
    P.silver, P.points = t.silver, t.points
    P.attr = { str = t.str, spd = t.spd, wit = t.wit }
    P.equip = { weapon = t.weapon, armor = t.armor }
    P.arts, P.art_lv = { "basic" }, { basic = 1 }
    for i = 1, #t.arts do
        if DATA.art[t.arts[i].id] then
            P.arts[#P.arts + 1] = t.arts[i].id
            P.art_lv[t.arts[i].id] = t.arts[i].lv
        end
    end
    P.bag = {}
    for i = 1, #t.bag do
        if DATA.item[t.bag[i].id] then P.bag[#P.bag + 1] = t.bag[i] end
    end
    P.map, P.x, P.y = t.map, t.x, t.y
    P.ready = true
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
    local sn = P.master and ("师承 " .. tostring(P.master)) or "无门无派"
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
    -- ★ 桌面 y 从 24 起: y=2..19 是左上角地图名浮层, 从 3 起会**压在地图名上**
    --   (用户要求"不允许文字重叠")。开菜单时顶部横条占 y0..19, 从 24 起正好避开。
    local y0 = 24
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
    -- ★ 底部提示行留够 20px: 16px 字 + 上下各 2px 内边距。原来按 22 算,
    --   提示文字的**下沿正好压在面板边框上** (实测截图可见字形被边框切掉)。
    local ph = 34 + n * lh + 24
    if ph > 258 then ph = 258 end
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
        if ry + lh > py + ph - 26 then break end
        local is_sel = (i == sel)
        -- 选中: 整行反白 + 行首 ▶。★ 反白**从光标位右侧开始**画: 整个行反白时
        --   箭头是黑墨画在黑底上 → 完全看不见 (实测: 选中行看不出光标)。
        if is_sel then
            app.fill(px + 18, ry - 1, px + pw - 3, ry + 16, BK)
            art("arrow", px + 5, ry + 3)
        end
        gt(px + 22, ry, rows[i], is_sel)
    end
    gt(px + 8, py + ph - 21, footer or "确认=选择  返回=上一级", false)
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

-- 顶部菜单项 (返回键弹出) —— 7 项正好铺满 400px, 再宽会折行
GAME.menu_items = { "继续", "行囊", "武功", "集市", "休息", "打坐", "退出" }
function UI.battle()
    local m = BATTLE.mob
    local rows = BATTLE.acts
    panel(string.format("对阵  %s   气血 %d/%d", m.name, math.max(0, m.hp), m.maxhp or m.hp),
          rows, BATTLE.sel, 1, nil, "确认=出手  返回=关闭")
end
-- 对话框 (底部覆盖, 原版框线 + 长文本 3 行/页分页)
function UI.dialog()
    local ph = 112
    local py = H - ph
    app.fill(0, py, W - 1, H - 1, WH)
    app.rect(4, py + 2, W - 5, H - 3, BK)
    -- 头像 (原版 32×32 线稿, 男/女)
    art(P.gender == 0 and "mport" or "fport", 10, py + 8)
    gt(48, py + 4, DLG.name or "？", false)
    if DLG.lines then
        local base = ((DLG.page or 1) - 1) * 3
        for i = 1, 3 do
            local s = DLG.lines[base + i]
            if s then gt(48, py + 22 + (i - 1) * 16, s, false) end
        end
        local pages = DLG.pages or 1
        if pages > 1 then
            local tag = string.format("%d/%d", DLG.page or 1, pages)
            gt(W - 12 - app.mixw(tag), py + 4, tag, false)
            art("pagedown", W - 26, py + 24)
        end
    else
        gt(48, py + 22, DLG.brief or "", false)
        if DLG.info and DLG.info.desc and DLG.info.desc ~= "" then
            gt(48, py + 40, DLG.info.desc, false)
        end
    end
    -- 选项: 底部横排 (左右选) —— 与"上下=翻页/左右=选择"的分工一致
    local y = py + 84
    local total = 0
    for i = 1, #DLG.opts do total = total + app.mixw(DLG.opts[i]) + 16 end
    local x = math.floor((W - total) / 2)
    for i = 1, #DLG.opts do
        local w = app.mixw(DLG.opts[i]) + 8
        if i == DLG.sel then app.fill(x, y - 2, x + w, y + 16, BK) end
        gt(x + 4, y, DLG.opts[i], i == DLG.sel)
        x = x + w + 8
    end
end

-- ============================================================================
-- ONL — 官方在线适配层 (2026-10-31 起: **只管能力自检, 不接未验证的接口**)
--
-- 调查结论 (依据: 官网首页内联脚本 + 公开 GET /api/stats, 见 docs/专题/应用资料/英雄坛说.md):
--   · 认证 = WebSocket(wss://www.gmud.online/) 首包 {"type":"login",username,password}
--     回 {"type":"login_ok","player":{...}}。**没有 Token、没有 Cookie**, 会话绑在长连接上。
--   · 角色/地图/NPC/战斗/仓库/拍卖 全是同一个 WS 上的 ~90 种 JSON 消息, 服务端权威。
--   · 唯一 HTTP 接口是 GET /api/stats (公开只读: 注册数/在线数)。
--   · **没有"存档上传/下载"接口** —— 进度就是服务端那份角色, 客户端拿到的是快照。
-- 所以本档能做的"在线"只有: 服务器可达性 + 公开统计 (只读), 其余必须等固件具备 WSS 客户端。
-- 这里**不写任何猜测的接口**, 也不做"假装登录成功"的假界面。
-- ============================================================================
local ONL = {
    pending = false, t0 = 0,
    http_ok = false,        -- 固件是否提供 app.net.http_url (可发 https)
    stats = nil,            -- /api/stats 解析结果
    err = nil,              -- 失败原因 (中文, 直接显示)
    tried = false,
}
local ONL_STATS_URL = "https://www.gmud.online/api/stats"

-- 能力探测: **不要用 type(x)=="function" 判** —— 主机模拟器把 C 回调暴露成 userdata
-- (可调用但不是 function), 真机是 function。用"存不存在 + pcall 调用"才两边都成立。
local function has_call(name)
    local t = (type(app.net) == "table") and app.net[name] or nil
    if t == nil then return false end
    local ok = pcall(function() return t end)
    return ok
end

function ONL.caps()
    local wifi = false
    if has_call("is_connected") then
        local ok, v = pcall(app.net.is_connected)
        wifi = ok and (v == true)
    end
    return {
        http_url = has_call("http_url"),
        wifi     = wifi,
        wss      = false,   -- 固件目前没有 WebSocket 客户端 (全仓无 ws_client / esp_websocket)
    }
end

-- 探一次公开统计 (只读, 4s 超时)。返回 nil 时 ONL.err 有原因。
function ONL.probe()
    ONL.tried = true
    local c = ONL.caps()
    ONL.http_ok = c.http_url
    if not c.http_url then ONL.err = "固件不支持 HTTPS 请求"; return end
    if not c.wifi then ONL.err = "未联网（先到设置里连 WiFi）"; return end
    local ok, body, code = pcall(app.net.http_url, ONL_STATS_URL, 4000)
    if not ok then ONL.err = "请求异常"; return end
    if type(body) ~= "string" or #body == 0 then
        ONL.err = (type(code) == "number" and code > 0) and ("服务器返回 " .. code) or "连接失败"
        return
    end
    local r = tonumber(string.match(body, "\"registered\"%s*:%s*(%d+)"))
    local o = tonumber(string.match(body, "\"online\"%s*:%s*(%d+)"))
    if not r then ONL.err = "返回格式不认识"; return end
    ONL.stats, ONL.err = { registered = r, online = o or 0 }, nil
    GAME.dirty = true
end

-- ============================================================================
-- 进入界面 (2026-10-31 双模式): 本地为主, 在线只做自检入口。
--   ★ 本地模式不需要登录/网络, 与上一版完全一致; 在线入口不写未验证接口。
-- ============================================================================
local ENT = { "初入江湖", "继续前缘", "在线(实验)", "离开" }
GAME.entry_sel = 1

-- 标题 + 头像 + 菜单 (男女主角头像用原版 32×32 素材)
function UI.entry()
    app.clear(WH)
    art("mface", 100, 22)
    app.zh24(152, 26, "英雄坛说", false)
    art("fface", 268, 22)
    -- 分隔线压到标题下方: 菜单有 4 项 (含"在线(实验)"), 原来放 y=193 会被菜单压住
    app.hline(64, 190, 96, BK)
    app.hline(210, 336, 96, BK)
    art("icon", 197, 93)

    local y = 112
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
        y = y + 34
    end
    local hint = "方向键选择   确认=进入   返回=退出"
    app.mix((400 - app.mixw(hint)) / 2, 268, hint, false)
end

-- 在线(实验)页: 只显示**已经核实**的事实, 不摆假登录框
function UI.online()
    app.clear(WH)
    app.zh24(14, 6, "在线模式", false)
    app.mix(110, 14, "(实验)", false)
    app.hline(14, W - 15, 34, BK)

    local c = ONL.caps()
    local y = 42
    local function row(k, v)
        app.mix(14, y, k, false)
        app.mix(150, y, v, false)          -- 值列固定 x=150: 标签最宽 "WebSocket" 也压不到
        y = y + 20
    end
    row("HTTPS", c.http_url and "支持(证书校验)" or "不支持")
    row("网络", c.wifi and "已连接" or "未连接")
    row("WebSocket", c.wss and "支持" or "不支持")

    y = y + 6
    app.hline(14, W - 15, y - 4, BK)
    if ONL.pending then
        app.mix(14, y, "正在连接 gmud.online …", false)
    elseif ONL.stats then
        app.mix(14, y, "服务器: 可达", false)
        app.mix(14, y + 20, "注册玩家 " .. ONL.stats.registered ..
                "   在线 " .. ONL.stats.online, false)
        app.mix(14, y + 40, "数据来源: GET /api/stats (公开只读)", false)
    elseif ONL.err then
        app.mix(14, y, "服务器: " .. ONL.err, false)
    else
        app.mix(14, y, "按确认键开始检测", false)
    end

    -- 说清楚"为什么还不能登录" —— 这是能力缺口, 不是账号问题
    local yy = 208
    app.hline(14, W - 15, yy - 6, BK)
    app.mix(14, yy, "官方登录走 WebSocket(同域 wss), 本固件", false)
    app.mix(14, yy + 18, "没有 WS 客户端, 登录/角色/存档暂不", false)
    app.mix(14, yy + 36, "可用。接入前不会显示假登录框。", false)
    app.mix(14, 280, "确认=重新检测   返回=回标题", false)
end

-- 素材缺失整屏提示 (不是 4 秒就消失的小条 —— 那正是"白屏"的观感来源)
function UI.err()
    app.clear(WH)
    app.rect(8, 8, W - 9, H - 9, BK)
    app.zh24(24, 18, "缺少素材包", false)
    app.mix(24, 56, "本游戏的地图/人物/数据不在 .lua 里,", false)
    app.mix(24, 74, "需要单独安装资源包。", false)
    app.hline(24, W - 24, 96, BK)
    app.mix(24, 106, "应安装到:", false)
    app.mix(24, 124, RES.need or ROOTS[1], false)
    if RES.err and RES.err ~= "素材包未安装" then
        app.mix(24, 146, "当前问题: " .. RES.err, false)
    end
    app.mix(24, 176, "办法: 应用商店 > 英雄坛说资源包 > 安装", false)
    app.mix(24, 194, "或把 wuxia/ 整个目录拷到", false)
    app.mix(24, 212, "/sdcard/apps/app/wuxia/ 下。", false)
    app.mix(24, 250, "返回=退出", false)
end

-- 读档列表: 三个槽
function UI.slots()
    app.clear(WH)
    app.mix(12, 6, "选择存档", false)
    app.hline(12, W - 13, 24, BK)
    local y = 34
    for i = 1, 3 do
        local t, why = SAVE.peek(i)
        app.rect(12, y, W - 13, y + 44, BK)
        local mark = (i == GAME.slot_sel)
        if mark then app.fill(13, y + 1, W - 14, y + 43, BK) end
        local title = t and (t.name .. "  Lv" .. t.lv) or "空位"
        app.mix(20, y + 4, (mark and "> " or "  ") .. i .. ". " .. title, mark)
        if t then
            local sub = (t.master and ("师承 " .. t.master) or "无门无派")
                        .. "   银 " .. t.silver .. "   地图 " .. t.map
            app.mix(20, y + 24, sub, mark)
        elseif why then
            app.mix(20, y + 24, "（" .. why .. "）", mark)
        end
        y = y + 52
    end
    app.mix(12, 268, "上下=选择  确认=读取  返回=回标题", false)
end

-- ============================================================================
-- CHAR — 创建角色 (性别 / 姓名 / 属性分配)
--   原站的属性分配规则不在公开数据里 ⇒ 这里用 **5 点自由分配 + 气血/内力按属性折算**,
--   属于单机版自定规则, 已在交付说明里标注, 不冒充原版数值。
-- ============================================================================
local CHAR = { step = 1, gender = 0, name = "", free = 5, sel = 1 }
local ALLOC = { str = 0, spd = 0, wit = 0 }
local ATTRS = { { "力量", "str" }, { "身法", "spd" }, { "悟性", "wit" } }

local function char_reset()
    CHAR.step, CHAR.gender, CHAR.name, CHAR.free, CHAR.sel = 1, 0, "", 5, 1
    ALLOC = { str = 0, spd = 0, wit = 0 }
    GAME.kb_idx = -1
end

-- 键盘 (输入姓名用; 与设置页 WiFi 键盘同一套: 方向键移光标, 确认=敲键)
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
    if not ok then
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
-- 返回: "done" = 输入结束, 其它 = 改名字
local function kb_tap()
    local i = GAME.kb_idx
    if i < 0 then return nil end
    local act = app.kbd.press(i)
    local ch = app.kbd.char(i)
    if act == app.kbd.CHAR and ch then
        if #CHAR.name < 8 then CHAR.name = CHAR.name .. ch end
    elseif act == app.kbd.BKSP then
        CHAR.name = CHAR.name:sub(1, -2)
    elseif act == app.kbd.ENTER or act == app.kbd.TAB then
        return "done"
    elseif act == app.kbd.ESC then
        return "back"
    end
    return nil
end

function UI.char()
    if CHAR.step == 2 then
        app.clear(WH)
        app.mix(12, 6, "取个名字", false)
        app.mix(12, 26, "名字 " .. (CHAR.name == "" and "_" or CHAR.name), false)
        app.mix(12, 46, "方向键移光标  确认=敲键  返回=输入完毕", false)
        app.kbd.draw(GAME.kb_idx)
        return
    end
    app.clear(WH)
    app.zh24(24, 10, "初入江湖", false)
    app.hline(24, W - 25, 40, BK)
    -- 性别 (选中时先铺黑再写白字 —— app.mix 的 inv 只反白字, 不铺底)
    local y = 54
    local gtxt = "性别   " .. (CHAR.gender == 0 and "男" or "女")
    if CHAR.step == 1 then
        app.fill(20, y - 3, 20 + app.mixw(gtxt) + 8, y + 17, BK)
        app.mix(24, y, gtxt, true)
        app.mix(250, y, "左右=切换", false)
    else
        app.mix(24, y, gtxt, false)
    end
    -- 姓名
    app.mix(24, y + 24, "姓名   " .. (CHAR.name == "" and "无名侠客" or CHAR.name), CHAR.step == 2)
    -- 属性
    app.mix(24, y + 54, "属性 (剩余点数 " .. CHAR.free .. ")", false)
    for i = 1, #ATTRS do
        local a = ATTRS[i]
        local base = (a[2] == "str") and 3 or 2
        local s = string.format("%s  %d", a[1], base + ALLOC[a[2]])
        local sy = y + 76 + (i - 1) * 20
        if CHAR.step == 3 and CHAR.sel == i then app.fill(24, sy - 2, 24 + 180, sy + 16, BK) end
        app.mix(28, sy, s, CHAR.step == 3 and CHAR.sel == i)
    end
    app.mix(24, y + 146, "确认=加一点   左键=减一点   右键=开始", false)
    app.mix(24, y + 166, "返回=上一步", false)
    if CHAR.step == 1 then
        app.mix(24, y + 186, "提示: 确认进入下一步", false)
    end
end

-- ============================================================================
-- SHOP — 集市 (买/卖)。物品/价格/恢复量全部来自 data.lua 原值。
--   左右 = 切"买入/卖出", 上下 = 选择, 确认 = 成交。原站交易在服务端, 这里按
--   **原站标价**做单机买卖 (卖出价 = 半价), 属于单机化规则, 已标注。
-- ============================================================================
local SHOP = { tab = 1, sel = 1, list = {} }

local function shop_list()
    local L = {}
    if SHOP.tab == 1 then
        for i = 1, #DATA.order do
            local id = DATA.order[i]
            local d = DATA.item[id]
            if d and (d.price or 0) > 0 then L[#L + 1] = { id = id, d = d } end
        end
    else
        for i = 1, #P.bag do
            local it = P.bag[i]
            local d = DATA.item[it.id]
            if d then L[#L + 1] = { id = it.id, d = d, n = it.n } end
        end
    end
    SHOP.list = L
    if SHOP.sel > #L then SHOP.sel = math.max(1, #L) end
    return L
end

function UI.shop()
    local L = shop_list()
    local VIS = 7
    local sel = SHOP.sel
    local first = 1
    if #L > VIS then
        first = sel - math.floor(VIS / 2)
        if first < 1 then first = 1 end
        if first > #L - VIS + 1 then first = math.max(1, #L - VIS + 1) end
    end
    local lh = 20
    local pw, ph = 340, 34 + VIS * lh + 26
    local px, py = math.floor((W - pw) / 2), 16
    app.fill(px, py, px + pw - 1, py + ph - 1, WH)
    app.rect(px, py, px + pw - 1, py + ph - 1, BK)
    app.hline(px, px + pw - 1, py + 24, BK)
    local tabname = (SHOP.tab == 1) and "买入" or "卖出"
    gt(px + 8, py + 4, string.format("集市 %s   银两 %d   第%d/%d", tabname, P.silver, sel, #L), false)
    if #L == 0 then
        gt(px + 12, py + 34, (SHOP.tab == 1) and "货架上没有可买的东西。" or "行囊是空的。", false)
    end
    for i = first, math.min(#L, first + VIS - 1) do
        local row = py + 28 + (i - first) * lh
        local it = L[i]
        local is_sel = (i == sel)
        if is_sel then app.fill(px + 2, row - 1, px + pw - 3, row + 16, BK) end
        local price = (SHOP.tab == 1) and (it.d.price or 0) or math.floor((it.d.price or 0) / 2)
        local nm = it.d.name .. (it.n and (" x" .. it.n) or "")
        gt(px + 12, row, nm, is_sel)
        local ps = tostring(price) .. "文"
        gt(px + pw - 12 - app.mixw(ps), row, ps, is_sel)
    end
    gt(px + 8, py + ph - 21, "左右=买/卖  上下=选  确认=成交  返回=关闭", false)
end

-- 开局: 新角色
local function start_new(slot)
    player_init()
    P.name = (CHAR.name ~= "" and CHAR.name) or "无名侠客"
    P.gender = CHAR.gender
    P.attr = { str = 3 + ALLOC.str, spd = 2 + ALLOC.spd, wit = 2 + ALLOC.wit }
    P.hpMax = 30 + P.attr.str * 2
    P.mpMax = 10 + P.attr.wit * 2
    P.hp, P.mp = P.hpMax, P.mpMax
    P.slot, P.ready = slot or 1, true
    if WDATA and WDATA.start then
        WORLD.enter(WDATA.start.map, WDATA.start.x, WDATA.start.y)
    end
    SAVE.write(P.slot)
    LOG.push("你踏进江湖，身上只有二十文钱。")
    GAME.state = "play"; GAME.dirty = true
end

-- 开局: 读档
local function start_load(slot, t)
    player_init()
    SAVE.apply(t)
    P.slot = slot
    if not (WDATA and WDATA.start and WORLD.enter(P.map, P.x, P.y)) then
        if WDATA and WDATA.start then
            WORLD.enter(WDATA.start.map, WDATA.start.x, WDATA.start.y)
        end
    end
    LOG.push("你回到了江湖（" .. (P.name or "") .. "）")
    GAME.state = "play"; GAME.dirty = true
end

-- ============================================================================
-- GAME — 状态机 / 步进 / 渲染
-- ============================================================================

GAME.state = "play"      -- 真正状态由 on_enter 决定 (entry/err/play)
GAME.slot_sel = 1
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
    -- ★ 源矩形自己夹在图像内: 地图比屏小的时候 camera 会给出**负偏移**,
    --   直接把负源坐标丢给 blit 时, 固件内部会补偏移、主机模拟器会跳过 ——
    --   两边画出来不一样 (而且负偏移的语义不写在 API 契约里, 不该依赖)。
    --   这里先算出可见源矩形, 传进来的 sx/sy 恒 >= 0。
    local mw, mh = cur.w * CFG.TILE, cur.h * CFG.TILE
    local sx0 = math.max(0, math.floor(cx))
    local sy0 = math.max(0, math.floor(cy))
    local sx1 = math.min(mw, math.floor(cx) + W)
    local sy1 = math.min(mh, math.floor(cy) + H)
    if sx1 > sx0 and sy1 > sy0 then
        app.img.blit(RES.map, sx0, sy0, sx0 - cx, sy0 - cy, sx1 - sx0, sy1 - sy0)
    end
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
    if GAME.state == "err"   then UI.err()   return end
    if GAME.state == "entry" then UI.entry() return end
    if GAME.state == "online" then UI.online() return end
    if GAME.state == "slots" then UI.slots() return end
    if GAME.state == "char"  then UI.char()  return end
    app.clear(WH)                -- ★ 全屏刷白: 地图比屏小时两侧/上下也不留上一帧残影
    scene()
    -- 菜单贴顶占满那一条, 所以开菜单时不再画地图名 (否则两者压在一起)
    -- 面板(行囊/武功/集市)铺开时会盖住左上角, 这时地图名与日志浮层**都不画** ——
    -- 否则它们从面板边缘露出来, 观感就是"字叠字"(用户明确要求不许重叠)。
    if GAME.state == "menu" then
        UI.topmenu(GAME.menu_items, GAME.menu_sel)
    elseif GAME.state == "play" then
        hud_mapname()
    end
    if GAME.state == "play" or GAME.state == "menu" then hud_right() end
    if GAME.state == "bag"   then UI.bag()   end
    if GAME.state == "skill" then UI.skill() end
    if GAME.state == "shop"  then UI.shop()  end
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
    -- ① 先确认素材在不在; 不在就整屏说清楚, 而不是"进去一片白"
    if not RES.enter() then
        GAME.state = "err"
        GAME.dirty = true
        return
    end
    ensure_data()
    SAVE.migrate_v1()          -- 旧单槽存档 → 1 号槽 (只在 1 号槽为空时做一次)
    char_reset()
    GAME.entry_sel = 1
    GAME.slot_msg = nil
    GAME.state = "entry"
    GAME.dirty = true
end
function on_exit()
    if P.ready then SAVE.write(P.slot) end       -- 退出必存一次 (不等 3 秒节流)
    app.img.freeall()
    RES.ready, RES.spr, RES.pl, RES.sprm, RES.plm, RES.map = false, nil, nil, nil, nil, nil
end
function render() GAME.render() end

-- 找一个空槽 (都满就覆盖 1 号)
local function free_slot()
    for i = 1, 3 do
        if not SAVE.peek(i) then return i end
    end
    return 1
end

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

    -- ⓪ 素材缺失页: 只有"返回/确认"= 退出
    if GAME.state == "err" then
        if a == A_BACK or a == A_OK then app.exit() end
        return true
    end

    -- ① 标题页: 初入江湖 / 继续前缘 / 离开 (无任何联网入口)
    if GAME.state == "entry" then
        if a == A_UP then GAME.entry_sel = math.max(1, GAME.entry_sel - 1)
        elseif a == A_DOWN then GAME.entry_sel = math.min(#ENT, GAME.entry_sel + 1)
        elseif a == A_OK then
            if GAME.entry_sel == 1 then
                char_reset()
                GAME.state = "char"
            elseif GAME.entry_sel == 2 then
                GAME.slot_sel, GAME.slot_msg = 1, nil
                GAME.state = "slots"
            elseif GAME.entry_sel == 3 then
                -- ★ 在线(实验): 只做能力自检 + 公开统计, 不碰本地存档, 不影响本地模式
                ONL.pending, ONL.t0 = true, app.millis()
                ONL.stats, ONL.err = nil, nil
                GAME.state = "online"
            else
                app.exit()
            end
        elseif a == A_BACK then app.exit() end
        return true
    end

    -- ② 选档
    if GAME.state == "slots" then
        if a == A_UP then GAME.slot_sel = math.max(1, GAME.slot_sel - 1)
        elseif a == A_DOWN then GAME.slot_sel = math.min(3, GAME.slot_sel + 1)
        elseif a == A_OK then
            local t, why = SAVE.peek(GAME.slot_sel)
            if t then start_load(GAME.slot_sel, t) else GAME.slot_msg = why or "空位" end
        elseif a == A_BACK then GAME.state = "entry" end
        return true
    end

    -- ②b 在线(实验)页: 确认=重新检测, 返回=回标题 (没有任何会改动本地数据的操作)
    if GAME.state == "online" then
        if a == A_OK then
            ONL.pending, ONL.t0 = true, app.millis()
            ONL.stats, ONL.err = nil, nil
        elseif a == A_BACK then
            GAME.state = "entry"
        end
        return true
    end

    -- ③ 创建角色: 性别 → 姓名 → 属性
    if GAME.state == "char" then
        if CHAR.step == 1 then
            if a == A_LEFT or a == A_RIGHT then CHAR.gender = 1 - CHAR.gender
            elseif a == A_OK then
                CHAR.step = 2
                kb_setup()
            elseif a == A_BACK then GAME.state = "entry" end
        elseif CHAR.step == 2 then
            if a == A_UP then kb_move(0, -1)
            elseif a == A_DOWN then kb_move(0, 1)
            elseif a == A_LEFT then kb_move(-1, 0)
            elseif a == A_RIGHT then kb_move(1, 0)
            elseif a == A_OK then
                if kb_tap() == "done" then CHAR.step = 3 end
            elseif a == A_BACK then CHAR.step = 3 end     -- 返回 = 输入完毕(留空则用默认名)
        else
            if a == A_UP then CHAR.sel = math.max(1, CHAR.sel - 1)
            elseif a == A_DOWN then CHAR.sel = math.min(#ATTRS, CHAR.sel + 1)
            elseif a == A_OK then
                local k = ATTRS[CHAR.sel][2]
                if CHAR.free > 0 then CHAR.free = CHAR.free - 1; ALLOC[k] = ALLOC[k] + 1 end
            elseif a == A_LEFT then
                local k = ATTRS[CHAR.sel][2]
                if ALLOC[k] > 0 then ALLOC[k] = ALLOC[k] - 1; CHAR.free = CHAR.free + 1 end
            elseif a == A_RIGHT then
                start_new(free_slot())
            elseif a == A_BACK then CHAR.step = 2 end
        end
        return true
    end

    -- 对话 (上下=翻页, 左右=选项)
    if DLG.on then
        local n = #DLG.opts
        if a == A_LEFT then DLG.sel = math.max(1, DLG.sel - 1)
        elseif a == A_RIGHT then DLG.sel = math.min(n, DLG.sel + 1)
        elseif a == A_UP then
            if DLG.pages and DLG.pages > 1 then
                DLG.page = math.max(1, (DLG.page or 1) - 1)
            end
        elseif a == A_DOWN then
            if DLG.pages and DLG.pages > 1 then
                DLG.page = math.min(DLG.pages, (DLG.page or 1) + 1)
            end
        elseif a == A_OK then DLG.choose()
        elseif a == A_BACK then DLG.on = false; DLG.lines = nil end
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

    -- 集市 (左右=买/卖, 上下=选, 确认=成交)
    if GAME.state == "shop" then
        if a == A_LEFT or a == A_RIGHT then
            SHOP.tab = (SHOP.tab == 1) and 2 or 1
            SHOP.sel = 1
        elseif a == A_UP then SHOP.sel = math.max(1, SHOP.sel - 1)
        elseif a == A_DOWN then SHOP.sel = SHOP.sel + 1
        elseif a == A_OK then
            local it = SHOP.list[SHOP.sel]
            if not it then LOG.push("这里没有东西。")
            elseif SHOP.tab == 1 then
                if P.silver < it.d.price then LOG.push("银两不够（需 " .. it.d.price .. " 文）")
                else
                    P.silver = P.silver - it.d.price
                    INV.add(it.id, 1)
                    LOG.push("买下 " .. it.d.name .. "（-" .. it.d.price .. " 文）")
                    app.tone_async(720, 70, 40)
                end
            else
                local half = math.max(1, math.floor((it.d.price or 0) / 2))
                if INV.del(it.id, 1) then
                    P.silver = P.silver + half
                    LOG.push("卖掉 " .. it.d.name .. "（+" .. half .. " 文）")
                    app.tone_async(560, 70, 40)
                end
            end
            SAVE.mark()
        elseif a == A_BACK then GAME.state = "play" end
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
                elseif s == 4 then GAME.state = "shop"; SHOP.tab, SHOP.sel = 1, 1
                elseif s == 5 then
                    if P.silver >= 5 then
                        P.silver = P.silver - 5
                        P.hp = P.hpMax
                        LOG.push("休息片刻，气血全复（花 5 文）")
                    else LOG.push("银子不够（需 5 文）") end
                    GAME.state = "play"
                elseif s == 6 then
                    P.mp = P.mpMax
                    LOG.push("打坐调息，内力全复。")
                    GAME.state = "play"
                else
                    SAVE.write(P.slot)          -- 退出前存一次
                    app.exit()
                end
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

    -- ★ 在线自检: **只在这里**发网络请求, 且只在"在线(实验)"页、由用户动作触发。
    --   先让"正在连接"那一帧画出去 (等 40ms), 再跑阻塞请求 —— 否则用户看到的是卡住。
    --   本地模式 (play/menu/...) 永远不会走到这条分支。
    if GAME.state == "online" and ONL.pending and (now - ONL.t0) >= 40 then
        ONL.pending = false
        local ok, err = pcall(ONL.probe)
        if not ok then ONL.err = "检测异常: " .. tostring(err) end
        GAME.dirty = true
    end

    hb = hb + 1
    if hb % 16 == 0 then GAME.dirty = true end       -- 约 0.5s 一次, 支撑浮层自动隐藏
end
