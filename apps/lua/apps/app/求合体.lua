--@lapp v1
--@app name=求合体
--@app cat=游戏
--@app desc=6x6 合成消除: 三个同种连成一片就升级, 连锁加分, 最高分存档
--@app version=1
--@app build=202611240806
--@app icon=AAAAAAAAAAA//////////H/////////+YAAAAAAAAAZgAAAAAAAABmAAAAAAAAAGYAAAAAAAAAZgAAAAAAAABmAAAAAAAAAGYAAAAAAAAAZgAAAAAAAABmAAAAAAAAAGYAAAAAAAAAZgAAAAAAAABmAAAAHAAAAGYAAAAcAAAAZgAAf///AABmAAA///4AAGYAAB///AAAZgAAD//4AABmAAAH//AAAGYAAAP/4AAAZgAAAf/AAABmAAAA/4AAAGYAAAB/AAAAZgAAABwAAABmAAAAHAAAAGYAAAAcAAAAZgAAABwAAABmAAAB/8AAAGYAAAH/wAAAZgAAAP+AAABmAAAD/+AAAGYAAAP/4AAAZgAAAH8AAABmAAAAPgAAAGYAAAA+AAAAZgAAABwAAABmAAAAHAAAAGYAAAAIAAAAZgAAAAgAAABmAAAAAAAAAGYAAAAAAAAAZgAAAAAAAABmAAAAAAAAAGYAAAAYAAAAZgAAABgAAABmAGAAGAAGAGYAYAAcAAYAZgBgABwABgBmAHAAOgAHAGYA6AA6AA6AZgDoAFkADoBmAWQAWQAWQGYCYgCYgCYgZgAAAAAAAABmAAAAAAAAAGYAAAAAAAAAZgAAAAAAAABmAAAAAAAAAGYAAAAAAAAAZ//////////j/////////8AAAAAAAAAAA=
-- 图标自绘: blit_app_icon(x, y[, inv=true])
-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "AAAAAAAAAAA//////////H/////////+YAAAAAAAAAZgAAAAAAAABmAAAAAAAAAGYAAAAAAAAAZgAAAAAAAABmAAAAAAAAAGYAAAAAAAAAZgAAAAAAAABmAAAAAAAAAGYAAAAAAAAAZgAAAAAAAABmAAAAHAAAAGYAAAAcAAAAZgAAf///AABmAAA///4AAGYAAB///AAAZgAAD//4AABmAAAH//AAAGYAAAP/4AAAZgAAAf/AAABmAAAA/4AAAGYAAAB/AAAAZgAAABwAAABmAAAAHAAAAGYAAAAcAAAAZgAAABwAAABmAAAB/8AAAGYAAAH/wAAAZgAAAP+AAABmAAAD/+AAAGYAAAP/4AAAZgAAAH8AAABmAAAAPgAAAGYAAAA+AAAAZgAAABwAAABmAAAAHAAAAGYAAAAIAAAAZgAAAAgAAABmAAAAAAAAAGYAAAAAAAAAZgAAAAAAAABmAAAAAAAAAGYAAAAYAAAAZgAAABgAAABmAGAAGAAGAGYAYAAcAAYAZgBgABwABgBmAHAAOgAHAGYA6AA6AA6AZgDoAFkADoBmAWQAWQAWQGYCYgCYgCYgZgAAAAAAAABmAAAAAAAAAGYAAAAAAAAAZgAAAAAAAABmAAAAAAAAAGYAAAAAAAAAZ//////////j/////////8AAAAAAAAAAA="
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
-- 求合体 (Triple Town-ish) · LinTOS Lua 单文件游戏
--
--   玩法: 6x6 棋盘, 每回合放一个随机物品; 3 个以上同种物品上下左右连通 -> 合成升级;
--         合成的产物若又凑够 3 个就继续合成(连锁, 每层 +25%); 棋盘填满即结束。
--
--   算法来源 (只借思路, 未复制任何代码; 两个项目均为 MIT):
--     keith-hall/triple-town-ish  src/lib/gameEngine.ts
--        - 36 格一维棋盘 + 4 邻接 BFS 连通块
--        - 连通块 >= 3 合成 NEXT_TIER, 落点 = 刚放下的格(在块内时)否则最小下标
--        - 逐轮"从最高等级往下找"-> 合成 -> 再从最高重扫 = 连锁
--        - 计分 = 产物分值 * (1 + 连锁序号*0.25) * (1 + 超额个数*0.1)
--        - Mulberry32 种子随机 (可复现), 权重表随分数分档
--        - 石头: 相邻合成 5 次后碎裂
--     WilliCommer/triple-game  lib/board.js / figures.js
--        - Pos/Board 分离, getCluster(flood fill), growto 链式升级, 百分比随机
--     PureZhao/TripleTown (Unity+xLua) 只看了 Docs 的连续 Combo / 消除 / 补缺说明,
--        它是"交换位置"的三消, 与本作放置式规则不同, 未移植任何代码。
--
--   布局: 棋盘 6x6 @ (6,12) 格 44px   -> x 6..272, y 12..287
--         右栏 x 276..394: 分数/最高/下一个(44x44 预览)/上步/回合/菜单
--         底部 y 288..299 一行 12px 提示 (不是独立信息栏)
--   存档: app.nvs 两键 (tt.best / tt.game), 自写 tag|body|校验和, 2.5s 节流写
-- ============================================================================

local W, H = app.W, app.H
local BK, WH = app.BLACK, app.WHITE
local NOW = app.millis

-- ---------------------------------------------------------------- 常量/几何
local N      = 6
local CELLS  = N * N
local LV_MAX = 9
local ROCK   = -1            -- 石头: 不参与合成, 挡路

local CELL   = 44
local BX     = 6
local BY     = 12
local BS     = CELL * N      -- 264
local PX     = BX + BS + 4   -- 274  右栏起点
local PW     = W - PX - 6    -- 120

local NAME = { "", "草", "灌木", "小树", "大树", "木屋", "石屋", "塔楼", "城堡", "世界树" }

-- 合成产物分值 (键 = 合成后的等级; 1 级不能凭空产生分值)
-- ⚠ 必须写显式键: 列表构造从 1 开始, 会让"下标=等级"整体错一位 (计分恒 0)
local POINTS = { [2] = 5, [3] = 20, [4] = 60, [5] = 200,
                 [6] = 700, [7] = 2400, [8] = 8000, [9] = 30000 }

-- 出生权重 {等级, 权重}; ROCK 为 -1
local W_LOW  = { { 1, 60 }, { 2, 22 }, { 3, 10 }, { ROCK, 8 } }
local W_HIGH = { { 1, 52 }, { 2, 20 }, { 3, 12 }, { 4, 8 },  { ROCK, 8 } }
local ROCK_CRACKS = 5        -- 相邻合成几次后碎裂

-- 4 邻接表 (预计算)
local NB = {}
for r = 1, N do
  for c = 1, N do
    local t = {}
    if c > 1 then t[#t + 1] = (r - 1) * N + c - 1 end
    if c < N then t[#t + 1] = (r - 1) * N + c + 1 end
    if r > 1 then t[#t + 1] = (r - 2) * N + c end
    if r < N then t[#t + 1] = r * N + c end
    NB[(r - 1) * N + c] = t
  end
end

-- --------------------------------------------------------------------- 状态
local G = {
  page = "game",             -- game / over / help
  seed = 0, st = 0,          -- 种子 + RNG 状态
  b = {}, cr = {},           -- 棋盘 1..36 (0 空 / 1..9 等级 / -1 石头), 石头裂纹数
  score = 0, best = 0, turn = 0,
  next = 1,                  -- 下一个物品
  over = false,
  chain = 0, gain = 0,       -- 本回合连锁数与得分
  hint = {},                 -- 落子即合成的空位 -> true
  flash = nil,               -- { i=格, t0=毫秒, dur=毫秒 }
  new_best = false,
  toast_msg = "", toast_until = 0,
  dlg_kind = nil,
  save_dirty = false, save_at = 0,
  sfx = true, vib = false,
}

-- 触摸状态
local T = { down = false, sx = 0, sy = 0 }

local function now() return NOW() end
local function cell_xy(i)
  local r, c = (i - 1) // N, (i - 1) % N
  return BX + c * CELL, BY + r * CELL
end
local function cell_at(x, y)
  if x < BX or x >= BX + BS or y < BY or y >= BY + BS then return 0 end
  local c = (x - BX) // CELL
  local r = (y - BY) // CELL
  return r * N + c + 1
end

-- ------------------------------------------------------- 12px 混排小字 (中文+ASCII)
local function mix12(x, y, s, inv)
  local i, n = 1, #s
  while i <= n do
    local b = s:byte(i)
    if b < 0x80 then
      app.texts(x, y, s:sub(i, i), inv); x = x + 8; i = i + 1
    elseif b >= 0xF0 then
      app.zhs(x, y, s:sub(i, i + 3), inv); x = x + 12; i = i + 4
    elseif b >= 0xE0 then
      app.zhs(x, y, s:sub(i, i + 2), inv); x = x + 12; i = i + 3
    else
      app.zhs(x, y, s:sub(i, i + 1), inv); x = x + 12; i = i + 2
    end
  end
  return x
end

local function toast(msg, ms)
  G.toast_msg = msg
  G.toast_until = now() + (ms or 1400)
end

local function beep(f, ms, amp)
  if G.sfx then app.tone_async(f, ms or 40, amp or 40) end
end

local function buzz()
  if G.vib then app.vibrate(30) end
end

-- ============================================================================
-- 1. 随机数: Mulberry32 (与参考引擎同款, 保证"种子 -> 序列"可复现)
-- ============================================================================
local M32 = 0xFFFFFFFF
local function imul(a, b)
  return ((a & M32) * (b & M32)) & M32
end

-- 返回 新状态, [0,1)
local function rng_next(st)
  local ns = (st + 0x6D2B79F5) & M32
  local t = ns
  t = imul(t ~ (t >> 15), t | 1)
  t = (t ~ ((t + imul(t ~ (t >> 7), t | 61)) & M32)) & M32
  local v = ((t ~ (t >> 14)) & M32) / 4294967296
  return ns, v
end

local function rand_int(n)          -- 1..n
  local ns, v = rng_next(G.st)
  G.st = ns
  local k = math.floor(v * n) + 1
  if k > n then k = n end
  return k
end

-- ============================================================================
-- 2. 棋盘模型: 连通块 / 合成 / 连锁 / 计分  (不涉及任何绘制)
-- ============================================================================
local function count_empty()
  local n = 0
  for i = 1, CELLS do if G.b[i] == 0 then n = n + 1 end end
  return n
end

local function contains(t, v)
  for k = 1, #t do if t[k] == v then return true end end
  return false
end

-- 以 start 为中心的同类连通块大小 (用于"落子即合成"提示)
local function comp_size(start, lv)
  local seen, q, head, n = { [start] = true }, { start }, 1, 0
  while head <= #q do
    local cur = q[head]; head = head + 1
    n = n + 1
    if n >= 3 then return n end
    for _, j in ipairs(NB[cur]) do
      if not seen[j] and G.b[j] == lv then
        seen[j] = true; q[#q + 1] = j
      end
    end
  end
  return n
end

-- 找出某一等级的全部连通块 (>=3 个)
local function find_comps(lv)
  local out, seen = {}, {}
  for i = 1, CELLS do
    if G.b[i] == lv and not seen[i] then
      local cells, q, head = {}, { i }, 1
      seen[i] = true
      local mn = i
      while head <= #q do
        local cur = q[head]; head = head + 1
        cells[#cells + 1] = cur
        if cur < mn then mn = cur end
        for _, j in ipairs(NB[cur]) do
          if not seen[j] and G.b[j] == lv then
            seen[j] = true; q[#q + 1] = j
          end
        end
      end
      if #cells >= 3 then out[#out + 1] = { cells = cells, anchor = mn } end
    end
  end
  return out
end

-- 计分: 产物分值 x 连锁加成 x 超额个数加成 (取整)
local function points(newlv, chain, n)
  local base = POINTS[newlv] or 0
  local m = (1 + (chain - 1) * 0.25) * (1 + math.max(0, n - 3) * 0.1)
  return math.floor(base * m + 0.5)
end

-- 一轮合成: 从最高等级往下找, 找到就合成并返回事件
local function merge_pass(prefer)
  for lv = LV_MAX - 1, 1, -1 do
    local comps = find_comps(lv)
    if #comps > 0 then
      local pick = comps[1]
      if prefer and prefer > 0 then
        for _, c in ipairs(comps) do
          if contains(c.cells, prefer) then pick = c; break end
        end
      end
      local anchor = pick.anchor
      if prefer and prefer > 0 and contains(pick.cells, prefer) then anchor = prefer end
      for _, j in ipairs(pick.cells) do G.b[j] = 0; G.cr[j] = 0 end
      G.b[anchor] = lv + 1
      G.cr[anchor] = 0
      return { lv = lv, n = #pick.cells, anchor = anchor }
    end
  end
  return nil
end

-- 连锁解算: 一直合成到没有可合的; 返回最后一次合成的落点
local function resolve(prefer)
  local anchor = prefer
  while true do
    local ev = merge_pass(prefer)
    if not ev then break end
    G.chain = G.chain + 1
    G.gain = G.gain + points(ev.lv + 1, G.chain, ev.n)
    -- 石头: 合成落点相邻的石头开裂
    for _, j in ipairs(NB[ev.anchor]) do
      if G.b[j] == ROCK then
        local c = (G.cr[j] or 0) + 1
        if c >= ROCK_CRACKS then G.b[j], G.cr[j] = 0, 0 else G.cr[j] = c end
      end
    end
    anchor = ev.anchor
    prefer = ev.anchor
  end
  return anchor
end

-- 该位置放 lv 会不会立刻凑够 3 个
local function would_merge(i, lv)
  if lv <= 0 then return false end
  G.b[i] = lv
  local n = comp_size(i, lv)
  G.b[i] = 0
  return n >= 3
end

local function spawn_lv()
  local w = (G.score < 1000) and W_LOW or W_HIGH
  local total = 0
  for k = 1, #w do total = total + w[k][2] end
  local ns, v = rng_next(G.st)
  G.st = ns
  local t, acc = v * total, 0
  for k = 1, #w do
    acc = acc + w[k][2]
    if t <= acc then return w[k][1] end
  end
  return w[#w][1]
end

-- 空位提示: 哪些空格放下去会立刻合成
local function refresh_hint()
  G.hint = {}
  if G.over or G.next <= 0 then return end
  for i = 1, CELLS do
    if G.b[i] == 0 then
      if would_merge(i, G.next) then G.hint[i] = true end
    end
  end
end

local function new_game()
  G.b, G.cr = {}, {}
  for i = 1, CELLS do G.b[i], G.cr[i] = 0, 0 end
  G.seed = (NOW() * 2654435761) & M32
  G.st = G.seed
  G.score, G.turn, G.over = 0, 0, false
  G.chain, G.gain, G.new_best = 0, 0, false
  G.flash = nil

  -- 开局撒 0..3 个, 不许一上来就摆出可合成的局面
  local k = rand_int(4) - 1
  for _ = 1, k do
    local lv = spawn_lv()
    for _try = 1, 12 do
      local i = rand_int(CELLS)
      if G.b[i] == 0 then
        if not would_merge(i, lv) then G.b[i] = lv; break end
      end
    end
  end

  G.next = spawn_lv()
  G.over = (count_empty() == 0)
  refresh_hint()
end

-- 放置: 唯一的"玩家动作"入口
local function place(i)
  if G.over then return false, "over" end
  if i < 1 or i > CELLS then return false, "oob" end
  if G.b[i] ~= 0 then return false, "occupied" end

  G.b[i] = G.next
  G.cr[i] = 0
  G.turn = G.turn + 1
  G.chain, G.gain = 0, 0

  local anchor = i
  if G.next > 0 then anchor = resolve(i) end

  G.score = G.score + G.gain
  G.next = spawn_lv()
  G.over = (count_empty() == 0)
  G.flash = { i = anchor, t0 = now(), dur = 260 }
  refresh_hint()

  if G.over then
    if G.score > G.best then
      G.best, G.new_best = G.score, true
      app.nvs.set_int("tt.best", G.best)
    end
    G.over_at = now() + 500      -- 先让最后一手的闪光播完, 再切结算页
    beep(200, 220, 50)
  elseif G.chain >= 2 then
    beep(960, 60, 46)
  elseif G.chain == 1 then
    beep(700, 45, 44)
  else
    beep(440, 30, 36)
  end
  buzz()
  G.save_dirty = true
  return true
end

-- ============================================================================
-- 3. 存档
-- ============================================================================
local function crc(s)
  local v = 0
  for k = 1, #s do v = (v * 31 + s:byte(k)) % 1000003 end
  return string.format("%06d", v)
end

local function board_str()
  local t = {}
  for i = 1, CELLS do
    local v = G.b[i]
    if v == ROCK then t[i] = "x"
    elseif v == 0 then t[i] = "0"
    else t[i] = tostring(v) end
  end
  return table.concat(t)
end

local function crack_str()
  local t = {}
  for i = 1, CELLS do t[i] = tostring(math.min(9, G.cr[i] or 0)) end
  return table.concat(t)
end

local function save_game()
  -- 必须存 RNG 状态与"下一个": 否则续局后随机序列从种子重头再来一遍
  local body = table.concat({ "T1", tostring(G.seed), tostring(G.st), tostring(G.score),
                              tostring(G.turn), tostring(G.next),
                              board_str(), crack_str() }, "|")
  app.nvs.set_str("tt.game", "G|" .. body .. "|" .. crc(body))
  G.save_dirty, G.save_at = false, now()
end

local function load_game()
  local s = app.nvs.get_str("tt.game")
  if type(s) ~= "string" or #s < 16 then return false end
  local f = {}
  for part in (s .. "|"):gmatch("(.-)|") do f[#f + 1] = part end
  if #f ~= 10 or f[1] ~= "G" or f[2] ~= "T1" then return false end
  local body = table.concat(f, "|", 2, 9)
  if crc(body) ~= f[10] then return false end

  local bs, cs = f[8], f[9]
  if #bs ~= CELLS or #cs ~= CELLS then return false end
  local b, cr = {}, {}
  for i = 1, CELLS do
    local ch = bs:sub(i, i)
    local v
    if ch == "x" then v = ROCK
    elseif ch == "0" then v = 0
    else v = tonumber(ch) end
    if not v or v > LV_MAX then return false end
    b[i] = v
    cr[i] = tonumber(cs:sub(i, i)) or 0
  end
  -- 空棋盘 + 0 分没有恢复价值
  local empty = 0
  for i = 1, CELLS do if b[i] == 0 then empty = empty + 1 end end
  if empty == CELLS then return false end

  G.seed = tonumber(f[3]) or 0
  G.st = tonumber(f[4]) or G.seed
  G.score = tonumber(f[5]) or 0
  G.turn = tonumber(f[6]) or 0
  G.next = tonumber(f[7]) or 1
  G.b, G.cr = b, cr
  G.over = false
  G.over_at = 0
  G.chain, G.gain, G.new_best = 0, 0, false
  G.flash = nil
  -- 满盘存档(在结算页退出): 恢复后必须仍是"已结束", 否则玩家卡在无处落子的棋盘
  G.over = (count_empty() == 0)
  refresh_hint()
  return true
end

local function load_best()
  local v = app.nvs.get_int("tt.best")
  G.best = (type(v) == "number" and v > 0) and v or 0
end

function save_all()
  save_game()
end

-- ============================================================================
-- 4. 绘制: 物品图 (1bpp 原创, 全部程序化绘制, 无素材文件)
--    每个图在 44x44 格里居中, 有效区约 28x30, 左下角留出等级角标
-- ============================================================================
-- 等腰三角形: (cx, cy) 是**底边**中点, 顶点在 cy-h+1
-- ⚠ 宽度系数必须随 k 递减, 否则画出来是倒三角 (曾把树/屋顶全画成 ▼)
local function tri(cx, cy, half, h)
  if h < 2 then return end
  for k = 0, h - 1 do
    local w = math.floor(half * (h - 1 - k) / (h - 1) + 0.5)
    app.hline(cx - w, cx + w, cy - k, BK)
  end
end

local function disc(cx, cy, r)
  for dy = -r, r do
    local t = r * r - dy * dy
    if t > 0 then
      local dx = math.floor(math.sqrt(t) + 0.5)
      app.hline(cx - dx, cx + dx, cy + dy, BK)
    end
  end
end

local ART = {}

-- 1 草: 三簇草叶 (中间高, 两侧带斜叶)
ART[1] = function(x, y)
  local cx, cb = x + 22, y + 34
  for k = -1, 1 do
    local bx = cx + k * 12
    local h = 15 - math.abs(k) * 3
    app.vline(bx, cb - h, cb, BK)
    for t = 2, h - 3 do
      local o = (t + 1) // 2
      app.px(bx - o, cb - t, BK)
      app.px(bx + o, cb - t, BK)
    end
  end
  app.hline(cx - 16, cx + 15, cb + 1, BK)
end

-- 2 灌木: 三球冠 + 短干
ART[2] = function(x, y)
  local cx, cb = x + 22, y + 34
  disc(cx - 6, cb - 9, 6)
  disc(cx + 6, cb - 9, 6)
  disc(cx, cb - 14, 7)
  app.fill(cx - 1, cb - 7, cx + 1, cb, BK)
  app.hline(cx - 10, cx + 9, cb + 1, BK)
end

-- 3 小树: 单层三角冠 + 干
ART[3] = function(x, y)
  local cx, cb = x + 22, y + 34
  app.fill(cx - 1, cb - 7, cx + 1, cb, BK)
  tri(cx, cb - 7, 9, 13)
  app.hline(cx - 8, cx + 7, cb + 1, BK)
end

-- 4 大树: 双层三角冠 + 粗干
ART[4] = function(x, y)
  local cx, cb = x + 22, y + 34
  app.fill(cx - 2, cb - 8, cx + 2, cb, BK)
  tri(cx, cb - 8, 12, 11)
  tri(cx, cb - 16, 8, 9)
  app.hline(cx - 9, cx + 8, cb + 1, BK)
end

-- 5 木屋: 方墙 + 三角顶 + 门 + 窗
ART[5] = function(x, y)
  local cx, cb = x + 22, y + 34
  app.fill(cx - 10, cb - 12, cx + 10, cb, BK)
  tri(cx, cb - 12, 13, 9)
  app.fill(cx - 3, cb - 7, cx + 2, cb, WH)
  app.vline(cx, cb - 6, cb, BK)
  app.fill(cx + 4, cb - 10, cx + 8, cb - 6, WH)
end

-- 6 石屋: 厚墙 + 石块纹理 + 烟囱 + 门
ART[6] = function(x, y)
  local cx, cb = x + 22, y + 34
  app.fill(cx - 11, cb - 13, cx + 11, cb, BK)
  app.fill(cx + 4, cb - 22, cx + 8, cb - 14, BK)
  tri(cx, cb - 13, 13, 8)
  -- 石块 (稀疏分布, 太密会变成条码)
  app.fill(cx - 9, cb - 10, cx - 5, cb - 8, WH)
  app.fill(cx + 3, cb - 11, cx + 7, cb - 9, WH)
  app.fill(cx - 7, cb - 6, cx - 3, cb - 4, WH)
  app.fill(cx + 3, cb - 6, cx + 6, cb - 4, WH)
  app.fill(cx - 2, cb - 5, cx + 2, cb, WH)     -- 门
end

-- 7 塔楼: 高瘦主体 + 顶部城齿 + 窗 + 门
ART[7] = function(x, y)
  local cx, cb = x + 22, y + 34
  app.fill(cx - 9, cb - 24, cx + 9, cb, BK)
  for k = 0, 2 do                              -- 城齿
    local bx = cx - 9 + k * 7
    app.fill(bx, cb - 28, bx + 3, cb - 24, BK)
  end
  app.fill(cx - 2, cb - 19, cx + 2, cb - 13, WH)   -- 窗
  app.fill(cx - 3, cb - 8, cx + 2, cb, WH)         -- 门
  app.hline(cx - 11, cx + 10, cb + 1, BK)          -- 基座
end

-- 8 城堡: 主体 + 双塔 + 城齿 + 城门
ART[8] = function(x, y)
  local cx, cb = x + 22, y + 34
  app.fill(cx - 14, cb - 14, cx + 14, cb, BK)
  app.fill(cx - 14, cb - 22, cx - 7, cb - 14, BK)
  app.fill(cx + 7, cb - 22, cx + 14, cb - 14, BK)
  for bx = cx - 14, cx - 9, 3 do app.fill(bx, cb - 22, bx + 1, cb - 20, WH) end
  for bx = cx + 7, cx + 12, 3 do app.fill(bx, cb - 22, bx + 1, cb - 20, WH) end
  for bx = cx - 13, cx + 11, 4 do app.fill(bx, cb - 14, bx + 1, cb - 12, WH) end
  app.fill(cx - 4, cb - 9, cx + 3, cb, WH)      -- 城门
  app.vline(cx, cb - 8, cb, BK)
  app.hline(cx - 15, cx + 14, cb + 1, BK)
end

-- 9 世界树: 大圆冠 + 粗干 + 根 + 叶隙
ART[9] = function(x, y)
  local cx, cb = x + 22, y + 34
  disc(cx, cb - 19, 13)
  app.fill(cx - 3, cb - 17, cx + 3, cb, BK)
  app.hline(cx - 11, cx - 4, cb, BK)
  app.hline(cx + 4, cx + 10, cb, BK)
  for _, p in ipairs({ { -7, -22 }, { 4, -26 }, { 9, -18 }, { -2, -15 }, { 1, -21 } }) do
    app.px(cx + p[1], cb + p[2], WH)
    app.px(cx + p[1] + 1, cb + p[2], WH)
  end
end

-- 石头: 不规则块 + 裂纹 (裂纹越多线越多)
local function art_rock(x, y, cracks)
  local cx, cb = x + 22, y + 34
  app.fill(cx - 11, cb - 11, cx + 11, cb, BK)
  app.fill(cx - 13, cb - 6, cx - 11, cb - 2, BK)
  app.fill(cx + 11, cb - 7, cx + 13, cb - 3, BK)
  app.fill(cx - 7, cb - 13, cx + 5, cb - 11, BK)
  app.vline(cx - 3, cb - 8, cb - 4, WH)
  app.hline(cx, cx + 4, cb - 5, WH)
  if (cracks or 0) >= 3 then app.vline(cx + 4, cb - 9, cb - 2, WH) end
end

local function draw_item(v, x, y, cracks)
  if v == ROCK then
    art_rock(x, y, cracks)
  elseif v and v >= 1 and v <= LV_MAX then
    ART[v](x, y)
  end
end

-- ============================================================================
-- 5. 页面绘制
-- ============================================================================
local function draw_board()
  app.fill(BX, BY, BX + BS - 1, BY + BS - 1, WH)
  for i = 1, CELLS do
    local x, y = cell_xy(i)
    local v = G.b[i]
    if v == 0 then
      if G.hint[i] then
        local cx, cy = x + 22, y + 22
        app.rect(cx - 3, cy - 3, cx + 3, cy + 3, BK)
      end
    else
      draw_item(v, x, y, G.cr[i])
    end
  end
  -- 网格 1px
  for k = 0, N do
    app.vline(BX + k * CELL, BY, BY + BS, BK)
    app.hline(BX, BX + BS, BY + k * CELL, BK)
  end
  -- 外框 2px
  app.rect(BX - 1, BY - 1, BX + BS + 1, BY + BS + 1, BK)
end

-- 合成闪光: 落点闪一个内框 (不改内容, 不糊掉物品)
local function draw_flash()
  if not G.flash then return end
  local dt = now() - G.flash.t0
  if dt > G.flash.dur then G.flash = nil; return end
  if (dt // 65) % 2 == 0 then
    local x, y = cell_xy(G.flash.i)
    app.rect(x + 2, y + 2, x + CELL - 3, y + CELL - 3, BK)
    app.rect(x + 4, y + 4, x + CELL - 5, y + CELL - 5, BK)
  end
end

-- 按钮矩形 (绘制与命中必须同源: 曾出现"画在 216..262, 命中判 236..272"的错位)
local MENU_BTN = { PX, 204, W - 8, 242 }
local OVER_BTN = { { 20, 216, 190, 262 }, { 210, 216, 380, 262 } }

local function in_btn(x, y, r)
  return x >= r[1] and x <= r[3] and y >= r[2] and y <= r[4]
end

local function btn(x0, y0, x1, y1, label, inv)
  if inv then
    app.fill(x0, y0, x1, y1, BK)
  else
    app.rect(x0, y0, x1, y1, BK)
  end
  app.mixc((x0 + x1) // 2, y0 + (y1 - y0 - 16) // 2, label, inv)
end

local function draw_panel()
  local x = PX
  -- 小标签 (12px) + 大数字, iOS 风格: 标签压小, 数值突出
  app.zhs(x, 4, "分数")
  local s = tostring(G.score)
  if #s <= 6 then app.text(x, 18, s) else app.textm(x, 21, s) end

  app.zhs(x, 48, "最高")
  app.textm(x, 62, tostring(G.best))

  app.zhs(x, 88, "下一个")
  app.rect(x, 104, x + 43, 147, BK)
  draw_item(G.next, x, 104, 0)
  app.mix(x + 48, 104, NAME[G.next] or "")
  app.texts(x + 48, 126, "Lv" .. tostring(G.next))

  if G.chain >= 2 then
    mix12(x, 154, "连锁 x" .. G.chain)
    mix12(x, 168, "本步 +" .. G.gain)
  elseif G.gain > 0 then
    mix12(x, 154, "本步 +" .. G.gain)
  end
  mix12(x, 184, "回合 " .. G.turn)

  btn(MENU_BTN[1], MENU_BTN[2], MENU_BTN[3], MENU_BTN[4], "菜单")
end

local function draw_toast()
  if G.toast_until <= now() then return end
  if G.toast_msg == "" then return end
  app.fill(0, BY + BS + 4, W - 1, H - 1, WH)
  mix12(8, BY + BS + 6, G.toast_msg)
end

local function draw_game()
  app.clear(WH)
  draw_board()
  draw_flash()
  draw_panel()
  draw_toast()
end

local function max_level()
  local m = 0
  for i = 1, CELLS do if G.b[i] > m then m = G.b[i] end end
  return m
end

local function draw_over()
  app.clear(WH)
  app.mixc(W // 2, 28, "棋盘满了")
  local s = tostring(G.score)
  app.text(W // 2 - #s * 8, 62, s)
  app.mixc(W // 2, 98, "最高 " .. G.best)
  if G.new_best then
    app.mixc(W // 2, 122, "新纪录")
  elseif G.best > G.score then
    app.mixc(W // 2, 122, "还差 " .. (G.best - G.score) .. " 分")
  end
  app.mixc(W // 2, 152, "回合 " .. G.turn .. "   最高 " .. (NAME[max_level()] or "—"))
  for k = 1, 2 do local b = OVER_BTN[k]; btn(b[1], b[2], b[3], b[4], k == 1 and "再来一局" or "看看棋盘") end
end

local HELP = {
  "点空格放下一个物品",
  "三个以上同种连成一片就合成升级",
  "只算上下左右, 斜角不算",
  "合成后再凑够就继续合, 越连分越高",
  "石头挡路: 旁边合成 5 次会碎",
  "没有空格就结束",
}

local function draw_help()
  app.clear(WH)
  app.mixc(W // 2, 16, "怎么玩")
  for k = 1, #HELP do
    app.mix(24, 48 + (k - 1) * 22, HELP[k])
  end
  btn(120, 236, 280, 272, "知道了")
end

function render()
  -- 满盘后延迟 0.5s 再切结算页, 让最后一手的闪光播完
  if G.over and G.page == "game" and now() >= (G.over_at or 0) then G.page = "over" end
  if G.page == "over" then draw_over()
  elseif G.page == "help" then draw_help()
  else draw_game() end
end

-- ============================================================================
-- 6. 输入与交互
-- ============================================================================
local function open_menu()
  G.dlg_kind = "menu"
  app.dlg.list("暂停", { "继续游戏", "重新开始", "玩法说明" }, 1, "回合 " .. G.turn)
end

local function on_tap(x, y)
  if G.page == "help" then
    G.page = "game"; return
  end
  if G.page == "over" then
    if in_btn(x, y, OVER_BTN[1]) then
      new_game()
      G.page = "game"
      save_game()
      beep(520, 40, 40)
    elseif in_btn(x, y, OVER_BTN[2]) then
      G.page = "game"
    end
    return
  end

  -- 右栏
  if x >= PX - 2 then
    if in_btn(x, y, MENU_BTN) then open_menu() end
    return
  end

  local i = cell_at(x, y)
  if i == 0 then return end
  local ok, why = place(i)
  if not ok then
    if why == "occupied" then toast("这里已经有东西了", 900) end
  end
end

function poll()
  -- 系统弹窗结果
  local r = app.dlg.result()
  if r ~= nil then
    local kind = G.dlg_kind
    G.dlg_kind = nil
    T.down = false            -- 弹窗刚关: 丢掉按下沿, 免得关闭后补一发落子
    if kind == "menu" then
      if r == 1 then
        -- 继续
      elseif r == 2 then
        new_game(); G.page = "game"; save_game()
      elseif r == 3 then
        G.page = "help"
      end
    end
  end

  -- 存档节流
  if G.save_dirty and now() - G.save_at > 2500 then save_game() end

  -- 弹窗未关: 固件已屏蔽 app.touch, 这里也把按下沿清掉并跳过,
  -- 否则"按下 -> 弹窗 -> 松手"会在关闭后按旧坐标补一次点击
  if G.dlg_kind ~= nil then T.down = false; return end

  -- 触摸: 抬起即点击
  local x, y = app.touch()
  if type(x) == "number" then
    if not T.down then
      T.down, T.sx, T.sy = true, x, y
    end
  elseif T.down then
    T.down = false
    on_tap(T.sx, T.sy)
  end
end

-- BACK: 帮助/结算页先回棋盘, 其它交回框架 (框架自己弹退出确认)
function action(a)
  if a == app.BACK then
    if G.page == "help" or G.page == "over" then
      G.page = "game"
      return true
    end
  end
  return false
end

-- ============================================================================
-- 7. 生命周期
-- ============================================================================
function on_enter()
  load_best()
  if not load_game() then new_game() end
  G.page = G.over and "over" or "game"
  T.down = false
  G.save_dirty = false

  -- 开机提示不是必要功能, 但首次进入给一句操作说明更友好
  if G.turn == 0 then toast("点空格放下一个物品", 2200) end
end

function on_exit()
  save_game()
end

-- ============================================================================
-- 8. 自检与探针 (主机测试用, 不影响游戏)
-- ============================================================================
local function set_board(str, cracks)
  G.b, G.cr = {}, {}
  for i = 1, CELLS do
    local ch = str:sub(i, i)
    if ch == "x" then G.b[i] = ROCK
    elseif ch == "0" then G.b[i] = 0
    else G.b[i] = tonumber(ch) or 0 end
    G.cr[i] = 0
  end
  if cracks then
    for i = 1, CELLS do G.cr[i] = tonumber(cracks:sub(i, i)) or 0 end
  end
  G.over = false
  G.chain, G.gain = 0, 0
end

-- 主机测试用: 导出 RNG 序列, 与 Python 侧 Mulberry32 逐值比对
function _rng_seq(st, n)
  local out, s = {}, st
  for k = 1, n do
    local nx, v = rng_next(s)
    s = nx
    out[k] = string.format("%.9f", v)
  end
  return table.concat(out, ",")
end

-- 主机测试用: 直接操作引擎 (主机跑不了触摸宏时用)
function _set_board(str, cracks)
  set_board(str, cracks)
  return board_str()
end

function _set_next(v) G.next = v; G.over = false; refresh_hint() end

function _set_score(v) G.score = v end

function _set_page(pg) G.page = pg end

function _place(i) return place(i) end

function _get(k)
  if k == "score" then return G.score end
  if k == "gain" then return G.gain end
  if k == "chain" then return G.chain end
  if k == "turn" then return G.turn end
  if k == "over" then return G.over end
  if k == "next" then return G.next end
  if k == "page" then return G.page end
  if k == "best" then return G.best end
  return nil
end

function _cell(i) return G.b[i], G.cr[i] end

-- 主机测试用: 铺满只留一格, 走真实 place 路径验证"满盘结束"
function _fill_for_over()
  for i = 1, CELLS do G.b[i], G.cr[i] = 0, 0 end
  for i = 1, CELLS - 1 do G.b[i] = ROCK end
  G.next, G.score, G.turn, G.over = 1, 0, 0, false
  G.page = "game"
  refresh_hint()
end

function _probe()
  return {
    page = G.page,
    score = G.score, best = G.best, turn = G.turn, next = G.next,
    over = G.over and 1 or 0, chain = G.chain, gain = G.gain,
    board = board_str(),
    hints = (function()
      local t = {}
      for i = 1, CELLS do if G.hint[i] then t[#t + 1] = i end end
      return table.concat(t, ",")
    end)(),
    bx = BX, by = BY, bs = BS, cell = CELL, px = PX, pw = PW,
  }
end

function _selftest()
  local fail, n = {}, 0
  local function ck(c, msg)
    n = n + 1
    if not c then fail[#fail + 1] = msg end
  end

  local keep = { seed = G.seed, score = G.score, turn = G.turn }
  local keep_board = board_str()

  -- ① RNG 可复现
  local s1 = 12345
  local ns1, v1 = rng_next(s1)
  local ns2, v2 = rng_next(s1)
  ck(ns1 == ns2 and v1 == v2, "同一状态 RNG 结果不一致")
  ck(v1 >= 0 and v1 < 1, "RNG 越界")
  local ns3 = rng_next(99999)
  ck(ns1 ~= ns3, "不同种子 RNG 无差异")
  -- 序列确定: 连取 5 个, 重放应完全一致
  local function seq(st)
    local out = {}
    for k = 1, 5 do
      local nx, vv = rng_next(st)
      st = nx
      out[k] = string.format("%.9f", vv)
    end
    return table.concat(out, ",")
  end
  ck(seq(777) == seq(777), "RNG 序列不可复现")

  -- ② 三连合成: 产物落在刚放下的格
  set_board("110000" .. string.rep("0", 30))
  G.b[1], G.b[2], G.next = 1, 1, 1
  G.score = 0
  local ok = place(3)
  ck(ok, "合法落子被拒")
  ck(G.b[3] == 2, "三连未合成 2 级 (实际 " .. tostring(G.b[3]) .. ")")
  ck(G.b[1] == 0 and G.b[2] == 0, "合成后原格未清空")
  ck(G.score == 5, "三连得分错 (实际 " .. G.score .. ")")
  ck(G.chain == 1, "连锁数错")

  -- ③ 四连: 超额 +10%
  set_board("111000" .. string.rep("0", 30))
  G.b[1], G.b[2], G.b[3], G.next = 1, 1, 1, 1
  G.score, G.chain, G.gain = 0, 0, 0
  place(4)
  ck(G.b[4] == 2, "四连未合成")
  ck(G.score == 6, "四连超额加成错 (期望 6, 实际 " .. G.score .. ")")

  -- ④ 连锁: 合成出的 2 级与旁边两个 2 级再合成
  set_board("110220" .. string.rep("0", 30))
  G.b[1], G.b[2] = 1, 1
  G.b[4], G.b[5] = 2, 2
  G.next = 1
  G.score, G.chain, G.gain = 0, 0, 0
  place(3)     -- 1,2,3 -> 2 落在 3; 与 4,5 组成 3 个 2 -> 3
  ck(G.b[3] == 3, "连锁未升到 3 级 (实际 " .. tostring(G.b[3]) .. ")")
  ck(G.b[4] == 0 and G.b[5] == 0, "连锁后未清空")
  ck(G.chain == 2, "连锁层数错 (实际 " .. G.chain .. ")")
  ck(G.score == 30, "连锁得分错 (期望 30, 实际 " .. G.score .. ")")

  -- ⑤ 斜角不算连通
  set_board("100000" .. "010000" .. "001000" .. string.rep("0", 18))
  G.b[1], G.b[8], G.b[15] = 1, 1, 1
  G.chain, G.gain, G.score = 0, 0, 0
  resolve(1)
  ck(G.b[1] == 1 and G.b[8] == 1 and G.b[15] == 1, "斜角三连被误判为连通")
  ck(G.chain == 0, "斜角不应产生连锁")

  -- ⑥ 石头: 相邻合成 5 次碎裂
  set_board("110x00" .. string.rep("0", 30))
  G.b[1], G.b[2], G.b[4], G.cr[4] = 1, 1, ROCK, 0
  G.next = 1
  G.chain, G.gain, G.score = 0, 0, 0
  local cracked = 0
  for k = 1, 5 do
    -- 每次都重置成"两个 1 + 落点"
    G.b[1], G.b[2], G.b[3] = 1, 1, 0
    G.next = 1
    place(3)
    if G.b[4] == 0 then cracked = k; break end
  end
  ck(cracked == 5, "石头未在第 5 次相邻合成时碎裂 (实际 " .. tostring(cracked) .. ")")

  -- ⑦ 已占格拒绝放置
  set_board(string.rep("0", 36))
  G.b[1] = 2
  G.next = 1
  local ok2, why2 = place(1)
  ck(not ok2 and why2 == "occupied", "占用格未被拒绝")
  ck(G.b[1] == 2, "拒绝时改动了棋盘")

  -- ⑧ 棋盘填满 -> 结束
  set_board(string.rep("x", 36))
  G.b[36] = 0
  G.next = 1
  G.score = 0
  place(36)
  ck(G.over, "填满后未判定结束")
  ck(G.over_at and G.over_at > 0, "结束后未安排结算页")
  ck(G.page == "game", "结算页应延后 0.5s 再切")

  -- ⑨ 存档往返
  set_board("123456" .. string.rep("0", 30))
  G.score, G.turn, G.seed = 4321, 17, 987654321
  G.b[5] = ROCK
  G.cr[5] = 2
  G.next = 4
  local saved_st = G.st
  save_game()
  G.b, G.score, G.turn, G.next, G.st = {}, 0, 0, 1, 0
  ck(load_game(), "存档读取失败")
  ck(G.score == 4321 and G.turn == 17, "存档数值未恢复")
  ck(G.b[1] == 1 and G.b[6] == 6 and G.b[5] == ROCK, "存档棋盘未恢复")
  ck(G.cr[5] == 2, "石头裂纹未恢复")
  ck(G.next == 4, "下一个物品未恢复")
  ck(G.st == saved_st, "RNG 状态未恢复 (续局会重复序列)")

  -- ⑩ 坏档不崩、不误恢复
  app.nvs.set_str("tt.game", "G|T1|1|2|短|短|000000")
  ck(load_game() == false, "坏档未拒绝")

  -- ⑪ 提示: 落子即合成的位置被标出
  set_board("110000" .. string.rep("0", 30))
  G.b[1], G.b[2] = 1, 1
  G.next, G.over = 1, false
  refresh_hint()
  ck(G.hint[3] == true, "三缺一的位置未给提示")

  -- 恢复现场
  G.b = {}
  for i = 1, CELLS do G.b[i], G.cr[i] = 0, 0 end
  for i = 1, CELLS do
    local ch = keep_board:sub(i, i)
    if ch == "x" then G.b[i] = ROCK
    elseif ch ~= "0" then G.b[i] = tonumber(ch) or 0 end
  end
  G.seed, G.score, G.turn = keep.seed, keep.score, keep.turn
  G.over = false
  G.page = "game"
  refresh_hint()

  return { ok = (#fail == 0), count = n, fail = fail }
end
