--@lapp v1
--@app name=地球生存
--@app cat=游戏
--@app desc=开源移植: 吃光能量球, 躲开敌人与鲨鱼, 水和流沙会耗能量
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
-- Earth Survival · 掌机版 (LinTOS 0.8 Lua 单文件应用)
-- ----------------------------------------------------------------------------
-- 移植自: ~/Desktop/开源游戏参考/Love2D-TestGame (Cod-e-Codes/Love2D-TestGame)
-- 原版是 Love2D / Lua 单文件 main.lua(1603 行), **全部用几何图形绘制**(love.graphics.rectangle
-- /polygon/circle), 没有贴图 —— 所以转 1bpp 几乎零信息损失, 只要给每种地形一个**图案**区分。
--
-- 移植时改了什么:
--   · 原版把 120×68 整图铺满窗口(每格只有 3~7px) —— 掌机上太小, 改成 **32px 瓦片 + 相机跟随**,
--     视野 12×9 格。世界尺寸(120×68)与地形生成规则**原样保留**。
--   · 颜色 → **图案**: 草=稀点 / 水=横波纹 / 山=实心三角 / 流沙=点阵 / 洞=实心黑 / 墙=粗框
--   · 粒子/拖尾/屏幕抖动等纯视觉项简化(1bpp 下没有透明度)
--
-- 玩法(与原版一致):
--   吃光所有能量球 = 胜利; 能量耗尽 = 失败。
--   水里会持续掉能量并刷鲨鱼; 流沙里走得慢且掉能量。
--   敌人: 2 只快的 + 18 只慢的, 有"追到看不见就搜索最后位置"的行为, 且**不进洞穴**。
--   无敌星: 吃到后 10 秒无敌(最多同时 2 颗)。
-- ============================================================================

local A_UP    = (type(app.UP)      == "number") and app.UP      or 1
local A_DOWN  = (type(app.DOWN)    == "number") and app.DOWN    or 2
local A_LEFT  = (type(app.LEFT)    == "number") and app.LEFT    or 3
local A_RIGHT = (type(app.RIGHT)   == "number") and app.RIGHT   or 4
local A_OK    = (type(app.CONFIRM) == "number") and app.CONFIRM or 5
local A_BACK  = (type(app.BACK)    == "number") and app.BACK    or 6
local BK, WH  = 0, 1
local W, H = 400, 300

local function beep(f, ms) if app.tone_async then app.tone_async(f, ms) end end
local function buzz(ms) if app.vibrate then app.vibrate(ms) end end

-- ============================================================================
-- CFG
-- ============================================================================
local CFG = {}
CFG.TILE   = 32                                  -- ★ linit 定: 原版 60px 缩到 32px 这一档
CFG.MCOLS, CFG.MROWS = 120, 68                   -- 世界尺寸 = 原版原样
CFG.VCOLS, CFG.VROWS = 12, 8                     -- 400/32=12.5, (300-20)/32=8.75
CFG.MW, CFG.MH = CFG.MCOLS * CFG.TILE, CFG.MROWS * CFG.TILE
CFG.HUD_H  = 20
CFG.DRAW_GAP_MS = 16
CFG.PSPD   = 128                                 -- 玩家像素速度 (px/s)
CFG.INVULN_MS, CFG.INVINCIBLE_MS = 1200, 10000

-- 地形 (顺序固定, 用于数组化)
local T_GRASS, T_WATER, T_MOUNT, T_QUICK, T_CAVE, T_WALL = 1, 2, 3, 4, 5, 6
-- 可走 / 阻挡 (与原版一致: 山与墙不可走, 洞与水可走)
local BLOCK = { [T_MOUNT] = true, [T_WALL] = true }
local SAFE  = { [T_GRASS] = true, [T_QUICK] = true, [T_WATER] = true, [T_CAVE] = true }

-- ============================================================================
-- 随机 (自实现 LCG: 固件 Lua 是 32 位整数, 大乘法会变浮点)
-- ============================================================================
local SEED = 24680
local function srnd(s) SEED = (s % 65519) + 1 end
local function rnd(n)                                   -- 0..n-1
    SEED = (SEED * 75 + 74) % 65519
    return SEED % n
end
local function rng(a, b) return a + rnd(b - a + 1) end
local function rndf() return rnd(10000) / 10000 end     -- 0..1 浮点

-- ============================================================================
-- 世界
-- ============================================================================
local T = {}                                            -- T[y*MCOLS+x] = 地形 (0 基)
local function tget(x, y)
    if x < 0 or y < 0 or x >= CFG.MCOLS or y >= CFG.MROWS then return T_WALL end
    return T[y * CFG.MCOLS + x] or T_GRASS
end
local function tset(x, y, v)
    if x < 0 or y < 0 or x >= CFG.MCOLS or y >= CFG.MROWS then return end
    T[y * CFG.MCOLS + x] = v
end
local function walkable(x, y) return not BLOCK[tget(x, y)] end

local WORLD = {}
local function gen_world()
    srnd(app.millis() + 11)
    T = {}
    for i = 0, CFG.MCOLS * CFG.MROWS - 1 do T[i] = T_GRASS end

    -- ① 边界墙 (原版)
    for x = 0, CFG.MCOLS - 1 do tset(x, 0, T_WALL); tset(x, CFG.MROWS - 1, T_WALL) end
    for y = 0, CFG.MROWS - 1 do tset(0, y, T_WALL); tset(CFG.MCOLS - 1, y, T_WALL) end

    -- ② 15 个圆形水体 (原版) + 点对点连线保证连通
    local bodies = {}
    for _ = 1, 15 do
        local cx, cy = rng(5, CFG.MCOLS - 6), rng(5, CFG.MROWS - 6)
        local sz = rng(3, 6)
        local body = {}
        for dy = -sz, sz do
            for dx = -sz, sz do
                local nx, ny = cx + dx, cy + dy
                if nx > 1 and nx < CFG.MCOLS - 1 and ny > 1 and ny < CFG.MROWS - 1
                   and (dx * dx + dy * dy) <= sz * sz then
                    tset(nx, ny, T_WATER)
                    body[#body + 1] = ny * 1000 + nx
                end
            end
        end
        if #body > 0 then bodies[#bodies + 1] = body end
    end
    for i = 1, #bodies - 1 do
        local p1 = bodies[i][rng(1, #bodies[i])]
        local p2 = bodies[i + 1][rng(1, #bodies[i + 1])]
        local x1, y1 = p1 % 1000, p1 // 1000
        local x2, y2 = p2 % 1000, p2 // 1000
        local dx, dy = x2 - x1, y2 - y1
        local steps = math.max(math.abs(dx), math.abs(dy))
        if steps > 0 then
            for s = 0, steps do
                local nx = math.floor(x1 + dx * s / steps + 0.5)
                local ny = math.floor(y1 + dy * s / steps + 0.5)
                if nx > 1 and nx < CFG.MCOLS - 1 and ny > 1 and ny < CFG.MROWS - 1 then
                    tset(nx, ny, T_WATER)
                end
            end
        end
    end

    -- ③ 12 座大山 + 16 座小峰 (原版数值)
    local function blob(n, smin, smax, kind)
        for _ = 1, n do
            local cx, cy = rng(4, CFG.MCOLS - 5), rng(4, CFG.MROWS - 5)
            local sz = rng(smin, smax)
            for dy = -sz, sz do
                for dx = -sz, sz do
                    local nx, ny = cx + dx, cy + dy
                    if nx > 1 and nx < CFG.MCOLS - 1 and ny > 1 and ny < CFG.MROWS - 1
                       and (dx * dx + dy * dy) <= sz * sz and tget(nx, ny) ~= T_WATER then
                        tset(nx, ny, kind)
                    end
                end
            end
        end
    end
    blob(12, 4, 8, T_MOUNT)
    blob(16, 2, 4, T_MOUNT)

    -- ④ 8 条蜿蜒隧道 (只在山里挖, 30% 分叉 —— 原版逻辑)
    for _ = 1, 8 do
        local cx, cy = rng(5, CFG.MCOLS - 6), rng(5, CFG.MROWS - 6)
        local ex, ey = rng(5, CFG.MCOLS - 6), rng(5, CFG.MROWS - 6)
        local steps = math.max(math.abs(ex - cx), math.abs(ey - cy)) + rng(5, 15)
        for _s = 1, steps do
            if tget(cx, cy) == T_MOUNT then
                tset(cx, cy, T_CAVE)
                if rnd(100) < 30 then
                    local bd = rng(2, 4)
                    local bdx, bdy = rng(-1, 1), rng(-1, 1)
                    for b = 1, bd do
                        local bx, by = cx + bdx * b, cy + bdy * b
                        if tget(bx, by) == T_MOUNT then tset(bx, by, T_CAVE) end
                    end
                end
            end
            if cx < ex then cx = cx + 1 elseif cx > ex then cx = cx - 1 end
            if cy < ey then cy = cy + 1 elseif cy > ey then cy = cy - 1 end
            if rnd(100) < 20 then
                cx = math.max(1, math.min(cx + rng(-1, 1), CFG.MCOLS - 2))
                cy = math.max(1, math.min(cy + rng(-1, 1), CFG.MROWS - 2))
            end
        end
    end

    -- ⑤ 20 片流沙 (只覆盖草地)
    for _ = 1, 20 do
        local cx, cy = rng(4, CFG.MCOLS - 5), rng(4, CFG.MROWS - 5)
        local sz = rng(3, 8)
        for dy = -sz, sz do
            for dx = -sz, sz do
                local nx, ny = cx + dx, cy + dy
                if nx > 2 and nx < CFG.MCOLS - 2 and ny > 2 and ny < CFG.MROWS - 2
                   and (dx * dx + dy * dy) <= sz * sz and tget(nx, ny) == T_GRASS then
                    tset(nx, ny, T_QUICK)
                end
            end
        end
    end

    -- ⑥ 出生点: 找能站的草地
    local sx, sy = CFG.MCOLS // 2, CFG.MROWS // 2
    for r = 0, 40 do
        local found = false
        for dy = -r, r do
            for dx = -r, r do
                if math.abs(dx) == r or math.abs(dy) == r then
                    local x, y = sx + dx, sy + dy
                    if walkable(x, y) and tget(x, y) == T_GRASS then sx, sy, found = x, y, true; break end
                end
            end
            if found then break end
        end
        if found then break end
    end
    WORLD.spawn_x, WORLD.spawn_y = sx, sy
end

-- ============================================================================
-- 实体
-- ============================================================================
local P = { x = 0, y = 0 }                              -- 像素坐标
local ORBS, ENEMIES, SHARKS, POWERS = {}, {}, {}, {}
local CAM = { x = 0, y = 0 }
local GAME = {
    state = "play", dirty = true, last_draw = -9999,
    page_dirty = true, play_full = true, last_state = nil, hud_key = nil,
    msg = nil, msg_t = 0, pulse = 0,
    water_t = 0, shark_t = 0, power_t = 0,
    score = 0, elapsed = 0,
    dyn_cur = {}, dyn_prev = {}, dn = 0, dn_prev = 0,
}

local function msg(s) GAME.msg, GAME.msg_t = s, app.millis(); GAME.dirty = true end

local function reset()
    gen_world()
    P.x, P.y = WORLD.spawn_x * CFG.TILE + CFG.TILE // 2, WORLD.spawn_y * CFG.TILE + CFG.TILE // 2
    P.energy, P.invuln, P.invincible = 100, 0, 0
    P.dir = 0
    -- 能量球 60 个 (原版: 只在草/流沙/洞)
    ORBS = {}
    local tries = 0
    while #ORBS < 60 and tries < 3000 do
        tries = tries + 1
        local x, y = rng(2, CFG.MCOLS - 3), rng(2, CFG.MROWS - 3)
        local t = tget(x, y)
        if t == T_GRASS or t == T_QUICK or t == T_CAVE then
            ORBS[#ORBS + 1] = { x = x, y = y, ph = rnd(628) / 100 }
        end
    end
    -- 敌人 20 只: 前 2 只快 (原版)
    ENEMIES = {}
    tries = 0
    while #ENEMIES < 20 and tries < 3000 do
        tries = tries + 1
        local x, y = rng(3, CFG.MCOLS - 4), rng(3, CFG.MROWS - 4)
        local t = tget(x, y)
        if t == T_GRASS or t == T_MOUNT or t == T_QUICK then
            local fast = (#ENEMIES < 2)
            ENEMIES[#ENEMIES + 1] = {
                x = x * CFG.TILE + CFG.TILE // 2, y = y * CFG.TILE + CFG.TILE // 2,
                fast = fast,
                spd = fast and rng(120, 180) or rng(20, 50),      -- px/s
                cd = 0, searching = false, lx = 0, ly = 0, st = 0,
            }
        end
    end
    SHARKS, POWERS = {}, {}
    GAME.score, GAME.elapsed = 0, 0
    GAME.water_t, GAME.shark_t, GAME.power_t = 0, 0, 0
    GAME.state, GAME.dirty, GAME.page_dirty, GAME.play_full = "play", true, true, true
    GAME.last_state, GAME.hud_key = nil, nil
    GAME.dn, GAME.dn_prev = 0, 0
    CAM.x, CAM.y = -1, -1
end

-- ============================================================================
-- 相机 (整格吸附)
-- ============================================================================
local function cam_follow()
    local tx = math.floor(P.x + CFG.TILE / 2) - W // 2
    local ty = math.floor(P.y + CFG.TILE / 2) - (H - CFG.HUD_H) // 2
    if tx < 0 then tx = 0 elseif tx > CFG.MW - W then tx = CFG.MW - W end
    if ty < 0 then ty = 0 elseif ty > CFG.MH - (H - CFG.HUD_H) then ty = CFG.MH - (H - CFG.HUD_H) end
    tx = (tx // CFG.TILE) * CFG.TILE
    ty = (ty // CFG.TILE) * CFG.TILE
    if tx ~= CAM.x or ty ~= CAM.y then CAM.x, CAM.y = tx, ty; return true end
    return false
end

-- ============================================================================
-- 地形绘制 (6 种图案; 32px 一格)
-- ============================================================================
-- 地形图案 —— ★ 1bpp 的核心原则: **纸本身就是地面**。
-- ⛔ 第一版给草地画了草簇、流沙画了 4×4 点阵, 结果**两种都变成"满屏规律点"**、完全分不出来,
--    水面又画了 12 条线 ⇒ 一屏全是横线。**图案越少越清楚**: 让最常见的草**完全留白**,
--    其余地形只用"一眼能认出的**少量**记号":
--      草 = 空白 · 水 = 2 条断波纹 · 流沙 = 3 个小叉(稀疏) · 山 = 实心三角 · 洞 = 实心黑 · 墙 = 双线框
-- 地形 = **不透明图集** (tile.bmp, 6 格 @32px, 由 tools 生成)
-- ★★ 为什么必须用"不透明 blit"而不是"草地留白 + 全屏 clear":
--    帧缓冲是 **x 主序** (相邻 x 相隔 75 字节) → 全屏 fill 12 万像素全是 PSRAM 随机写 ⇒
--    **一次几十毫秒**; 而相机每走 32px 就跨一格 ⇒ 每 0.25 秒顿一次 = 用户说的"走一下顿一下"。
--    改成 opaque blit: 白底和墨色一起刷出来, **完全不需要 clear**, 而且 108 次 blit 走的是
--    行缓冲重排的紧凑循环, 比逐像素 API 快一个量级。
local RES = { dir = "/store/earthsurvival/" }

local function blit_tile(t, sx, sy)
    if not RES.tile then return end
    local S = CFG.TILE
    local ox, oy, w, h = 0, 0, S, S
    if sx < 0 then ox = -sx; w = w - ox; sx = 0 end
    if sy < CFG.HUD_H then                                    -- 被状态条压住的行: 只画露出来的部分
        local clip = CFG.HUD_H - sy
        if clip >= S then return end
        oy = clip; h = h - clip; sy = CFG.HUD_H
    end
    if sx + w > W then w = W - sx end
    if sy + h > H then h = H - sy end
    if w <= 0 or h <= 0 then return end
    app.img.blit(RES.tile, (t - 1) * S + ox, oy, sx, sy, w, h)
end

local function draw_tile(t, sx, sy) blit_tile(t, sx, sy) end

local function blit_cell(cx, cy)
    if cx < 0 or cy < 0 or cx >= CFG.MCOLS or cy >= CFG.MROWS then return end
    local sx, sy = cx * CFG.TILE - CAM.x, cy * CFG.TILE - CAM.y
    if sx <= -CFG.TILE or sy <= -CFG.TILE or sx >= W or sy >= H then return end
    if sy < CFG.HUD_H then
        -- 首行被状态条压着: 只画露出来的部分, 并作废 HUD 缓存
        GAME.hud_key = nil
        local clip = CFG.HUD_H - sy
        if clip > 0 and clip < CFG.TILE then
            sy = sy + clip
        else
            return
        end
    end
    draw_tile(tget(cx, cy), sx, sy)
end

function GAME.draw_static()
    local c0, r0 = CAM.x // CFG.TILE, CAM.y // CFG.TILE
    for r = r0, r0 + CFG.VROWS + 1 do
        for c = c0, c0 + CFG.VCOLS + 1 do
            local sx, sy = c * CFG.TILE - CAM.x, r * CFG.TILE - CAM.y
            if sx > -CFG.TILE and sy > -CFG.TILE and sx < W and sy < H and sy >= CFG.HUD_H then
                draw_tile(tget(c, r), sx, sy)
            end
        end
    end
    GAME.dn_prev = 0
end

local function dcell(x, y)
    local n = GAME.dn + 1
    if n > 3000 then return end
    GAME.dn = n
    GAME.dyn_cur[n] = y * 1000 + x
end
-- ★ 按**像素包围盒**精确标脏 (而不是固定 3×3):
--   ① 3×3 会白擦 9 格 (9×1024 px), 精确只要 1~4 格 → 快 3~4 倍;
--   ② 精确覆盖**杜绝残留** —— 拖影就是"有格子没被标到"。
local function dbox(px, py, w, h)
    local c0 = math.floor(px / CFG.TILE)
    local r0 = math.floor(py / CFG.TILE)
    local c1 = math.floor((px + w - 1) / CFG.TILE)
    local r1 = math.floor((py + h - 1) / CFG.TILE)
    for r = r0, r1 do
        for c = c0, c1 do dcell(c, r) end
    end
end

local function draw_dynamic()
    local pv, pn = GAME.dyn_prev, GAME.dn_prev or 0
    for i = 1, pn do
        local c = pv[i]
        blit_cell(c % 1000, c // 1000)
    end
    local tmp = GAME.dyn_cur
    GAME.dyn_cur, GAME.dyn_prev = GAME.dyn_prev, GAME.dyn_cur
    GAME.dn_prev, GAME.dn = GAME.dn, 0

    local function vis(sx, sy) return sx > -40 and sy > CFG.HUD_H - 40 and sx < W and sy < H end
    local S = CFG.TILE

    -- 能量球: 空心方框 + 中心点（原来是实心方块，和慢敌人同形状只差大小，糊成一片）
    for i = 1, #ORBS do
        local o = ORBS[i]
        local sx, sy = o.x * S - CAM.x + 12, o.y * S - CAM.y + 12
        if vis(sx, sy) then
            dbox(o.x * S + 12, o.y * S + 12, 8, 8)
            app.rect(sx, sy, sx + 7, sy + 7, BK)
            app.fill(sx + 3, sy + 3, sx + 4, sy + 4, BK)
        end
    end
    -- 无敌星: 实心 + 四角
    for i = 1, #POWERS do
        local o = POWERS[i]
        local sx, sy = o.x * S - CAM.x + 8, o.y * S - CAM.y + 8
        if vis(sx, sy) then
            dbox(o.x * S + 8, o.y * S + 8, 16, 16)
            app.fill(sx + 4, sy, sx + 11, sy + 15, BK)
            app.fill(sx, sy + 4, sx + 15, sy + 11, BK)
        end
    end
    -- 敌人: 快的=三角, 慢的=方块 (原版就是不同多边形)
    for i = 1, #ENEMIES do
        local e = ENEMIES[i]
        local gx, gy = math.floor(e.x) // S, math.floor(e.y) // S
        local sx, sy = gx * S - CAM.x, gy * S - CAM.y
        dbox(e.x - 10, e.y - 10, 20, 20)
        if vis(sx, sy) then
            if e.fast then
                -- 倒三角（尖朝下）：和鲨鱼的正三角区分开
                for k = 0, 7 do
                    app.hline(sx + 8 + k, sx + 23 - k, sy + 8 + k, BK)
                end
            else
                -- 实心方块 + 两只白眼：像"生物"，和能量球的空心框区分
                app.fill(sx + 8, sy + 8, sx + 23, sy + 23, BK)
                app.fill(sx + 11, sy + 12, sx + 13, sy + 14, WH)
                app.fill(sx + 18, sy + 12, sx + 20, sy + 14, WH)
            end
        end
    end
    -- 鲨鱼: 大三角 + 底边（加了底边才像"鱼"，否则和快敌人的三角分不清）
    for i = 1, #SHARKS do
        local s2 = SHARKS[i]
        local gx, gy = math.floor(s2.x) // S, math.floor(s2.y) // S
        local sx, sy = gx * S - CAM.x, gy * S - CAM.y
        dbox(s2.x - 12, s2.y - 12, 24, 24)
        if vis(sx, sy) then
            for k = 0, 10 do
                app.hline(sx + 16 - k, sx + 15 + k, sy + 6 + k, BK)
            end
            app.hline(sx + 8, sx + 23, sy + 17, BK)      -- 底边
            app.hline(sx + 9, sx + 22, sy + 18, BK)
        end
    end
    -- 玩家: 实心圆 + 无敌时外圈
    local pgx = math.floor(P.x) // S
    local pgy = math.floor(P.y) // S
    dbox(P.x - 14, P.y - 14, 28, 28)
    local psx, psy = math.floor(P.x) - CAM.x, math.floor(P.y) - CAM.y
    if psx > -40 and psy > CFG.HUD_H - 40 and psx < W and psy < H then
        local blink = (P.invuln > 0) and (math.floor(app.millis() / 100) % 2 == 0)
        if not blink then
            local cx, cy, r = psx, psy, 9
            for dy = -r, r do
                local dx = math.floor(math.sqrt(r * r - dy * dy) + 0.5)
                app.hline(cx - dx, cx + dx, cy + dy, BK)
            end
            if P.invincible > 0 then
                for dy = -r - 4, r + 4 do
                    local dx = math.floor(math.sqrt((r + 4) * (r + 4) - dy * dy) + 0.5)
                    app.px(cx - dx, cy + dy, BK); app.px(cx + dx, cy + dy, BK)
                end
            end
        end
    end
end

-- ============================================================================
-- HUD
-- ============================================================================
local function hud()
    local key = P.energy .. "|" .. #ORBS .. "|" .. #ENEMIES .. "|" ..
                (#SHARKS > 0 and 1 or 0) .. "|" .. math.floor((P.invincible + 999) / 1000) ..
                "|" .. (GAME.elapsed // 1000)
    if GAME.hud_key == key then return end
    GAME.hud_key = key
    app.fill(0, 0, W - 1, CFG.HUD_H - 1, WH)
    app.hline(0, W - 1, CFG.HUD_H - 1, BK)
    -- 能量条 (原版 100 上限)
    local bw = 90
    app.rect(4, 5, 4 + bw, 14, BK)
    local fw = (bw - 2) * P.energy // 100
    if fw > 0 then
        if P.energy <= 30 then
            -- ★ 低电量警告: 单色屏不能变色, 改用**竖条纹**填充 —— 远远一看就和满格实心不同
            app.fill(5, 6, 4 + fw, 13, BK)              -- 先铺黑
            for x = 5, 4 + fw, 3 do
                for y = 6, 13 do app.px(x, y, WH) end   -- 再挖白竖缝
            end
        else
            app.fill(5, 6, 4 + fw, 13, BK)
        end
    end
    app.mix(100, 3, "球" .. #ORBS, false)
    app.mix(148, 3, "敌" .. #ENEMIES, false)
    if P.invincible > 0 then app.mix(196, 3, "无敌", false) end
    if #SHARKS > 0 then app.mix(244, 3, "鲨鱼", false) end
    app.mix(300, 3, (GAME.elapsed // 1000) .. "s", false)
end

-- ============================================================================
-- 逻辑
-- ============================================================================
local function hurt(amount, ms)
    if P.invincible > 0 then return end
    P.energy = P.energy - amount
    P.invuln = ms or CFG.INVULN_MS
    buzz(40); beep(180, 50)
    if P.energy <= 0 then
        P.energy = 0
        GAME.state = "dead"
        GAME.page_dirty = true
    end
    GAME.dirty = true
end

local function spawn_shark()
    if #SHARKS >= 5 then return end
    local px, py = math.floor(P.x / CFG.TILE), math.floor(P.y / CFG.TILE)
    for _ = 1, 50 do
        local x, y = px + rng(-15, 15), py + rng(-15, 15)
        if x > 1 and x < CFG.MCOLS - 2 and y > 1 and y < CFG.MROWS - 2 and tget(x, y) == T_WATER then
            SHARKS[#SHARKS + 1] = {
                x = x * CFG.TILE + CFG.TILE // 2, y = y * CFG.TILE + CFG.TILE // 2,
                spd = rng(90, 140), cd = 0, life = 10000,
            }
            return
        end
    end
end

local function spawn_power()
    if #POWERS >= 2 then return end
    for _ = 1, 100 do
        local x, y = rng(3, CFG.MCOLS - 4), rng(3, CFG.MROWS - 4)
        local t = tget(x, y)
        if t == T_GRASS or t == T_CAVE then
            POWERS[#POWERS + 1] = { x = x, y = y }
            return
        end
    end
end

local function step(dt)
    local ms = dt
    local ptx, pty = math.floor(P.x / CFG.TILE), math.floor(P.y / CFG.TILE)
    local pt = tget(ptx, pty)

    -- 移动 (方向键按住 / 手柄)
    local dx, dy = 0, 0
    if app.key_down(A_UP) then dy = -1
    elseif app.key_down(A_DOWN) then dy = 1
    elseif app.key_down(A_LEFT) then dx = -1
    elseif app.key_down(A_RIGHT) then dx = 1 end
    -- ★ 方向键做**保持 120ms 的惯性**:
    --   固件的"按住态"是按输入任务节拍刷新的, 直接单帧读会出现"按下→松开→按下"的抖动,
    --   表现就是**走一下顿一下**。记住方向并在 120ms 内续命, 抖动就被抹平了。
    if dx ~= 0 or dy ~= 0 then
        P.hdx, P.hdy, P.hold = dx, dy, 120
    elseif (P.hold or 0) > 0 then
        P.hold = P.hold - dt
        dx, dy = P.hdx or 0, P.hdy or 0
    end

    local spd = CFG.PSPD * (dt / 1000)
    if pt == T_QUICK then spd = spd * 0.4 end                 -- 流沙减速 (原版)
    if dx ~= 0 or dy ~= 0 then
        P.dir = (dx ~= 0) and dx or dy
        local nx, ny = P.x + dx * spd, P.y + dy * spd
        -- 分轴判碰撞, 避免卡角
        if walkable(math.floor(nx / CFG.TILE), math.floor(P.y / CFG.TILE)) then P.x = nx end
        if walkable(math.floor(P.x / CFG.TILE), math.floor(ny / CFG.TILE)) then P.y = ny end
        GAME.dirty = true
    end

    -- 地形效果
    if pt == T_WATER then
        GAME.water_t = GAME.water_t + dt
        if GAME.water_t >= 600 then                               -- 水里持续掉能量
            GAME.water_t = 0
            P.energy = math.max(0, P.energy - 1)
            GAME.dirty = true
        end
        GAME.shark_t = GAME.shark_t + dt
        if GAME.shark_t >= 2500 then GAME.shark_t = 0; spawn_shark() end
    elseif pt == T_QUICK then
        GAME.water_t = GAME.water_t + dt
        if GAME.water_t >= 1200 then
            GAME.water_t = 0
            P.energy = math.max(0, P.energy - 1)
            GAME.dirty = true
        end
    else
        GAME.water_t = 0
    end
    if P.energy <= 0 then P.energy = 0; GAME.state = "dead"; GAME.page_dirty = true end

    -- 能量球
    for i = #ORBS, 1, -1 do
        local o = ORBS[i]
        local ox, oy = o.x * CFG.TILE + CFG.TILE // 2, o.y * CFG.TILE + CFG.TILE // 2
        local ddx, ddy = ox - P.x, oy - P.y
        if ddx * ddx + ddy * ddy < 22 * 22 then
            table.remove(ORBS, i)
            GAME.score = GAME.score + 1
            beep(880, 40)
            GAME.dirty = true
            if #ORBS == 0 then
                GAME.state = "win"; GAME.page_dirty = true
            end
        end
    end
    -- 无敌星
    for i = #POWERS, 1, -1 do
        local o = POWERS[i]
        local ox, oy = o.x * CFG.TILE + CFG.TILE // 2, o.y * CFG.TILE + CFG.TILE // 2
        local ddx, ddy = ox - P.x, oy - P.y
        if ddx * ddx + ddy * ddy < 24 * 24 then
            table.remove(POWERS, i)
            P.invincible = CFG.INVINCIBLE_MS
            beep(1200, 80)
            msg("无敌 10 秒")
            GAME.dirty = true
        end
    end

    -- 计时器
    if P.invuln > 0 then P.invuln = math.max(0, P.invuln - dt); GAME.dirty = true end
    if P.invincible > 0 then P.invincible = math.max(0, P.invincible - dt); GAME.dirty = true end
    GAME.power_t = GAME.power_t + dt
    if GAME.power_t >= 20000 then GAME.power_t = 0; spawn_power() end
    GAME.elapsed = GAME.elapsed + dt

    -- 敌人 AI: 视野内直追; 丢失后去"最后已知位置"搜索; 都不进洞穴 (原版行为)
    local SIGHT = 9 * CFG.TILE
    for i = 1, #ENEMIES do
        local e = ENEMIES[i]
        local vx, vy = P.x - e.x, P.y - e.y
        local dist = math.sqrt(vx * vx + vy * vy)
        local tx, ty
        if dist < SIGHT then
            e.searching = false
            e.lx, e.ly = P.x, P.y
            tx, ty = P.x, P.y
        elseif e.lx ~= 0 then
            e.searching = true
            tx, ty = e.lx, e.ly
        else
            e.st = e.st - dt
            if e.st <= 0 then
                e.st = rng(800, 2000)
                e.lx, e.ly = 0, 0
            end
            tx, ty = nil, nil
        end
        if tx then
            local ddx, ddy = tx - e.x, ty - e.y
            local dl = math.sqrt(ddx * ddx + ddy * ddy)
            if dl > 1 then
                local mv = e.spd * (dt / 1000)
                local nx, ny = e.x + ddx / dl * mv, e.y + ddy / dl * mv
                local ntx, nty = math.floor(nx / CFG.TILE), math.floor(ny / CFG.TILE)
                local nt = tget(ntx, nty)
                if not BLOCK[nt] and nt ~= T_CAVE then            -- 不进洞穴
                    e.x, e.y = nx, ny
                end
            end
            if e.searching and dist > SIGHT and dl < 20 then e.lx, e.ly = 0, 0 end
        end
        -- 接触伤害
        local cd = e.x - P.x
        local cy2 = e.y - P.y
        if cd * cd + cy2 * cy2 < 20 * 20 then hurt(10) end
    end

    -- 鲨鱼
    for i = #SHARKS, 1, -1 do
        local s2 = SHARKS[i]
        s2.life = s2.life - dt
        local ddx, ddy = P.x - s2.x, P.y - s2.y
        local dl = math.sqrt(ddx * ddx + ddy * ddy)
        if dl > 1 then
            local mv = s2.spd * (dt / 1000)
            s2.x, s2.y = s2.x + ddx / dl * mv, s2.y + ddy / dl * mv
        end
        if dl < 20 then hurt(15) end
        if (s2.life <= 0 and pt ~= T_WATER) or dl > 900 then table.remove(SHARKS, i) end
    end
    if #ENEMIES > 0 or #SHARKS > 0 then
        GAME.tick = (GAME.tick or 0) + dt
        if GAME.tick >= 100 then GAME.tick = 0; GAME.dirty = true end
    end
end

-- ============================================================================
-- 渲染
-- ============================================================================
local function panel(title, sub)
    app.fill(60, 90, W - 61, H - 91, WH)
    app.rect(60, 90, W - 61, H - 91, BK)
    app.rect(61, 91, W - 62, H - 92, BK)
    app.mix(150, 106, title, false)
    app.hline(68, W - 69, 130, BK)
    app.mix(100, 146, sub, false)
    app.mix(96, 190, "确认=重来   返回=退出", false)
end

function render()
    local now = app.millis()
    if not GAME.dirty then return end
    if (now - GAME.last_draw) < CFG.DRAW_GAP_MS then return end
    GAME.last_draw = now
    GAME.dirty = false
    local st = GAME.state
    if GAME.last_state ~= st then GAME.last_state = st; GAME.page_dirty = true end

    if st == "dead" or st == "win" then
        if GAME.page_dirty then
            GAME.page_dirty = false
            app.clear(WH)
            if st == "dead" then panel("能量耗尽", "存活 " .. (GAME.elapsed // 1000) .. " 秒")
            else panel("全部收集!", "用时 " .. (GAME.elapsed // 1000) .. " 秒, 还有 " .. #ENEMIES .. " 只敌人") end
        end
        return
    end

    if cam_follow() then GAME.play_full = true end
    if GAME.play_full then
        GAME.play_full = false
        GAME.hud_key = nil
        -- ⛔ 这里**故意不调 app.clear()**: 图集是不透明的(白底也会刷),
        --    全屏重铺已经把每个像素覆盖掉; 而 clear 一次 = 12 万次 PSRAM 随机写 ≈ 几十毫秒,
        --    正是"走一下顿一下"的元凶。
        GAME.draw_static()
        GAME.dn, GAME.dn_prev = 0, 0
    end
    draw_dynamic()
    hud()
    if GAME.msg and (app.millis() - GAME.msg_t) <= 1500 then
        local w = app.mixw(GAME.msg)
        local mx = math.floor((W - w - 10) / 2)
        app.fill(mx, H - 34, mx + w + 9, H - 15, WH)
        app.rect(mx, H - 34, mx + w + 9, H - 15, BK)
        app.mix(mx + 5, H - 33, GAME.msg, false)
    end
end

-- ============================================================================
-- 契约函数
-- ============================================================================
function poll()
    local now = app.millis()
    local dt = now - (GAME.last_t or now)
    if dt < 0 then dt = 0 elseif dt > 200 then dt = 200 end
    GAME.last_t = now
    if GAME.state == "play" then step(dt) end
end

function on_enter()
    RES.tile = app.img.load(RES.dir .. "tile.bmp", true)   -- true = 不透明(白底一并刷出)
    reset()
    msg(RES.tile and "吃光能量球, 水里有鲨鱼" or "素材缺失")
end

function on_exit()
    app.img.freeall()
end

function action(a)
    if type(a) ~= "number" or a < 1 or a > 7 then return true end
    GAME.dirty = true
    if GAME.state == "dead" or GAME.state == "win" then
        if a == A_OK then reset()
        elseif a == A_BACK then app.exit() end
        return true
    end
    if a == A_BACK then app.exit() end
    return true
end
