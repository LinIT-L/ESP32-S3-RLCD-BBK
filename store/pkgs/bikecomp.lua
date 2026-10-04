--@lapp v1
--@app name=码表
--@app cat=程序
--@app desc=自行车码表: GPS 速度/里程/轨迹, 心率栏已预留
--@app version=1.0
--@app icon=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/8AAAAAAAAD/wAAAAAAAAP/AAAAAAAAL+AAAAAAX///8AAAAAf////wAAAAB///A/AAAAAFPAAH8AAAAAAcAA9wAAAAABwAHngAAAAAHAA8OAAAAAAcAHg4AAAAABwA8DgAAAD8HAHgPwAAA/+eA8D/4AAP/84Hg//wAB8D/g8H/PgAPAD+Hg8cPAA4AH48DhweAHAAPngcHA4AcAA/8BwcDwDgAB/gOBwHAOAAP8A4DgcA4B/////+BwDgH/////gHAOAAPcA4AAcA4AAfgDgABwBwAB8AHAAHAHAAOAAcAA4AeAB4AB4AHgA8APAADwA8AB8B4AAHwHgAD//AAAP/8AAH/4AAAf/gAAH+AAAAf4AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
-- 图标自绘: blit_app_icon(x, y[, inv=true])
-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/8AAAAAAAAD/wAAAAAAAAP/AAAAAAAAL+AAAAAAX///8AAAAAf////wAAAAB///A/AAAAAFPAAH8AAAAAAcAA9wAAAAABwAHngAAAAAHAA8OAAAAAAcAHg4AAAAABwA8DgAAAD8HAHgPwAAA/+eA8D/4AAP/84Hg//wAB8D/g8H/PgAPAD+Hg8cPAA4AH48DhweAHAAPngcHA4AcAA/8BwcDwDgAB/gOBwHAOAAP8A4DgcA4B/////+BwDgH/////gHAOAAPcA4AAcA4AAfgDgABwBwAB8AHAAHAHAAOAAcAA4AeAB4AB4AHgA8APAADwA8AB8B4AAHwHgAD//AAAP/8AAH/4AAAf/gAAH+AAAAf4AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
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
-- bikecomp.lua — 自行车码表 (掌机版, 第一版: 只用本机 GPS, 不依赖 Garmin)
--
-- 数据源: app.gps.* (components/drivers/gps/gps_nmea.c + lua_bridge/luat_lib_gps.c)
--         GPS 没接好/没定位时自动降级成内置模拟, 保证 UI 能开发与验收。
--
-- 页面 (左右键切换):
--   1 码表   大号速度 + 里程/均速/最高/用时 + 卫星/心率
--   2 轨迹   面包屑轨迹图 + 坐标 + 海拔 + 航向
--   3 诊断   GPS 原始状态 (定位质量/HDOP/句子数/校验错) —— 上机调 GPS 就看这页
-- 按键: OK 开始/暂停记录 · 左右 切页 · BACK 退出
--
-- ⚠ 心率一栏是**预留位**: 现在显示 "--"。后续接 Garmin 手表广播心率 / BLE 心率带时,
--   只需把 G.hr 填上 (值 = bpm), 界面不用改。
--
-- 数值全部走**整数定标** (见 luat_lib_gps.c 文件头): 距离用分米, 速度用 0.1km/h,
-- 微度做经纬度 —— 因为本项目 Lua 是 LUA_32BITS, lua_Number 是 float, 浮点累积会漂。

local W, H = 400, 300
local SB_H = 24                      -- 状态栏高度 (svc_statusbar.c SB_STRIP_H)

-- 按键常量带数值兜底: 真机上 app.UP 等可能取不到值(读成 -2), 直接比较会让所有分支不命中。
-- 这是技能 §11.3 记录的实测坑, 一定不要图省事直接写 app.LEFT。
local A_UP    = (type(app.UP)      == "number") and app.UP      or 1
local A_DOWN  = (type(app.DOWN)    == "number") and app.DOWN    or 2
local A_LEFT  = (type(app.LEFT)    == "number") and app.LEFT    or 3
local A_RIGHT = (type(app.RIGHT)   == "number") and app.RIGHT   or 4
local A_OK    = (type(app.CONFIRM) == "number") and app.CONFIRM or 5
local A_BACK  = (type(app.BACK)    == "number") and app.BACK    or 6

local BK, WH = 0, 1                  -- app.BLACK / app.WHITE 的数值用法 (0 就是黑)
local PAGE_N = 3

-- 速度低于这个值不计入"移动时间" (即自动暂停); 单位 0.1km/h
local MOVE_TH_KMH10 = 12
-- 两点间距超过这个值认为跳点, 不计入里程 (米)
local JUMP_MAX_M = 200
-- 轨迹打点间隔 (移动超过多少米存一个点)
local TRACK_STEP_M = 8
local TRACK_MAX = 240                -- 环形轨迹点上限

-- 全部字段给默认值: render() 有可能在第一次 poll() 之前被调用, 不能依赖"poll 已经跑过"。
local G = {
  page = 1,
  rec = false,          -- 记录中
  t_ms = 0,             -- 累计移动时间 (自动暂停: 低速不计)
  dist_dm = 0,          -- 累计距离 (分米)
  max_kmh10 = 0,        -- 最高速 (0.1km/h)
  p_lat = nil, p_lon = nil,          -- 上一次 poll 的位置 (判跳点/算里程)
  tr_lat = nil, tr_lon = nil,        -- 上一个轨迹点的位置 (判打点间距)
  last_tick = 0,
  track = {}, tr_n = 0, tr_full = false,
  hr = nil,             -- 心率 (预留: 后续接 BLE 心率带 / Garmin 手表广播)
  fix = nil,            -- 最新定位快照
  src = "初始化",        -- 数据源角标
  key = nil,
}

-- ================= 小工具 =================

local function pad(n, w)
  local s = tostring(n)
  while #s < w do s = "0" .. s end
  return s
end

-- 0.1 定标整数 → "23.4" (速度用 0.1km/h, 航向用 0.1 度, 同一个格式化)
-- 必须先取整: 定标值可能是浮点 (例如 4*dt 累出来的航向), 否则会打成 "128.0.3.1999…"
local function fmt_tenth(v)
  v = math.floor((v or 0) + 0.5)
  if v < 0 then v = 0 end
  return (v // 10) .. "." .. (v % 10)
end

-- 分米 → "12.34" km (两位小数)
local function fmt_km(dm)
  local c = dm // 100              -- 百分之一公里
  return (c // 100) .. "." .. pad(c % 100, 2)
end

-- 毫秒 → "00:42:10"
local function fmt_hms(ms)
  local s = ms // 1000
  return pad(s // 3600, 2) .. ":" .. pad((s // 60) % 60, 2) .. ":" .. pad(s % 60, 2)
end

local function fmt_dms(v, pos_c, neg_c)
  local a = v < 0 and -v or v
  local hemi = (v < 0) and neg_c or pos_c
  return (a // 1000000) .. "." .. pad(a % 1000000, 6) .. hemi
end

-- 两点距离 (米, 整数). 经度按当前纬度收缩; 平面近似在岛内骑行完全够用。
local function dist_m(lat1, lon1, lat2, lon2)
  local dy = (lat2 - lat1) * 0.111320            -- 米/微度
  local cl = math.cos((lat1 / 1000000) * math.pi / 180)
  local dx = (lon2 - lon1) * 0.111320 * cl
  local d = math.sqrt(dx * dx + dy * dy)
  return math.floor(d + 0.5)
end

-- ================= 数据源 =================

-- 无 app.gps 时 (或 GPS 起不来) 用内置模拟, 让界面可验证。
-- ⚠ 三条踩过的坑, 改这段时别再犯:
--   ① 位移必须按**真实经过时间**推进。写成"每次调用推一步" → poll 调得越勤车越快,
--      第一版就这样把均速算成了 51 km/h。
--   ② 单位: latf/lonf 是**度**, 而 dist/111320 得来的增量也是**度**。第一版把"微度增量"
--      加到"度"变量上, 33 秒后纬度跑到 604°N。
--   ③ 内部用浮点累加, 只在输出时取整 —— 每步都取整会丢掉不足 1 微度的位移, 距离偏小。
local SIM = { t0 = nil, t_last = nil, latf = 25.046000, lonf = 119.119000, crs = 0 }

local function sim_fix()
  local ms = app.millis() or 0
  if not SIM.t0 then SIM.t0 = ms; SIM.t_last = ms end
  local t  = (ms - SIM.t0) / 1000
  local dt = (ms - (SIM.t_last or ms)) / 1000
  if dt < 0 or dt > 2 then dt = 0 end          -- 首帧/跳变不计
  SIM.t_last = ms

  local kmh10 = 216 + math.floor(58 * math.sin(t * 0.45))
  SIM.crs = (SIM.crs + 4 * dt) % 360           -- 4 度/秒, 慢慢绕圈

  local mps = kmh10 / 36.0                     -- 0.1km/h → m/s
  local d   = mps * dt
  local rad = SIM.crs * math.pi / 180
  SIM.latf = SIM.latf + (d * math.cos(rad)) / 111320.0
  local cl = math.cos(SIM.latf * math.pi / 180)
  if cl < 0.2 then cl = 0.2 end
  SIM.lonf = SIM.lonf + (d * math.sin(rad)) / (111320.0 * cl)

  return {
    valid = true, demo = true,
    lat_e6 = math.floor(SIM.latf * 1000000), lon_e6 = math.floor(SIM.lonf * 1000000),
    kmh10 = kmh10, crs10 = math.floor(SIM.crs * 10),   -- 定标值必须是整数, 否则格式化会串位
    alt_m = 32, alt_dm = 325, hdop100 = 80,
    sats = 11, quality = 1, dim = 3,
    hh = 8, mm = 0, ss = math.floor(t) % 60,
    yy = 2026, mo = 10, dd = 3,
    age_ms = 0, sentences = 0, badcs = 0,
  }
end

local function read_fix()
  if app.gps and app.gps.fix then
    local f = app.gps.fix()
    if f then
      G.src = f.demo and "演示" or "GPS"
      return f
    end
    G.src = "无数据"
    return nil
  end
  G.src = "模拟"
  return sim_fix()
end

-- ================= 累计逻辑 =================

local function push_track(lat, lon)
  G.tr_n = G.tr_n + 1
  local i = ((G.tr_n - 1) % TRACK_MAX) + 1
  G.track[i] = { lat, lon }
  if G.tr_n > TRACK_MAX then G.tr_full = true end
  G.tr_lat, G.tr_lon = lat, lon          -- 记住"上一个轨迹点", 打点距离以它为基准
end

local function accumulate(fix, dt_ms)
  if not fix or not fix.valid then return end

  local lat, lon = fix.lat_e6, fix.lon_e6

  if G.p_lat then
    -- 里程: 与**上一次 poll** 的位移 (poll 频率高, 这个 d 很小, 适合判跳点)
    local d = dist_m(G.p_lat, G.p_lon, lat, lon)
    if d <= JUMP_MAX_M then
      G.dist_dm = G.dist_dm + d * 10          -- 米 → 分米
      if G.rec and (fix.kmh10 or 0) >= MOVE_TH_KMH10 then
        G.t_ms = G.t_ms + dt_ms
      end
    end
  end

  G.p_lat, G.p_lon = lat, lon

  -- 轨迹打点: 必须与**上一个轨迹点**比距离。
  -- ⚠ 写成"与上一次 poll 比"是错的: 真机上 poll 每帧都跑 (20~50Hz), 单帧位移只有
  --   0.1~0.3m, 永远达不到 8m → 一条轨迹都存不下来 (这个 bug 会被低频的模拟器掩盖)。
  if G.tr_lat then
    local dp = dist_m(G.tr_lat, G.tr_lon, lat, lon)
    if dp >= TRACK_STEP_M and dp <= JUMP_MAX_M then push_track(lat, lon) end
  else
    push_track(lat, lon)
  end

  local k = fix.kmh10 or 0
  if k > G.max_kmh10 then G.max_kmh10 = k end
end

-- 均速 (0.1km/h, 纯整数算, 避免浮点): dist_dm*3.6/t_s
local function avg_kmh10()
  local ts = G.t_ms // 1000
  if ts < 1 or G.dist_dm < 1 then return 0 end
  return (G.dist_dm * 36) // (10 * ts)
end

-- ================= 绘制 =================

-- 标签 + 数值 一行。
-- ⚠ 两侧都必须走 app.mix, 原因有两条 (都是踩出来的):
--   ① app.textsb 是**纯 ASCII** 字体 → 送汉字画成 '?';
--   ② app.zh 是**汉字专用**字体 → 送 ASCII (如 "HDOP") 画成空白, 标签直接消失。
--      app.mix 两头都认, 用它就不用再按内容挑字体。
local function cell(x, y, label, value, inv)
  local on = inv and true or false
  app.mix(x, y, label, on)
  app.mix(x + app.mixw(label) + 4, y, value, on)
end

local function draw_header(title)
  app.mix(6, SB_H + 4, title, false)
  local st = G.rec and "REC" or "PAUSE"
  app.textsb(W - 6 - #st * 8, SB_H + 4, st, false)
  app.hline(0, W - 1, SB_H + 22, BK)
end

-- 大号速度: 8×16 字模 * scale6 = 每字 48×96
local function draw_speed(fix)
  local k = fix and fix.kmh10 or 0
  app.bigc(W // 2, SB_H + 30, fmt_tenth(k), 6, false)
  app.textsb(W // 2 - 16, SB_H + 130, "km/h", false)
end

local function page_main(fix)
  draw_header("BIKECOMP")
  draw_speed(fix)

  local y0 = SB_H + 152
  app.hline(0, W - 1, y0 - 6, BK)

  local y1, y2 = y0, y0 + 22
  cell(8,   y1, "里程", fmt_km(G.dist_dm) .. "km")
  cell(208, y1, "用时", fmt_hms(G.t_ms))
  cell(8,   y2, "均速", fmt_tenth(avg_kmh10()) .. "km/h")
  cell(208, y2, "最高", fmt_tenth(G.max_kmh10) .. "km/h")

  app.hline(0, W - 1, y2 + 20, BK)

  -- 底部状态: 卫星 / 心率(预留) / 数据源
  local y3 = y2 + 28
  local sats = fix and fix.sats or 0
  if fix and fix.valid then
    cell(8, y3, "卫星", tostring(sats) .. " 颗")
  else
    cell(8, y3, "卫星", tostring(sats) .. " 等定位")
  end
  local hr = G.hr and (tostring(G.hr) .. " bpm") or "--"
  cell(208, y3, "心率", hr)

  -- 数据源角标: 一眼看出现在是真 GPS 还是模拟
  do
    local sw = app.mixw(G.src)
    app.mix(W - 6 - sw, H - 14, G.src, false)
  end
end

-- 面包屑轨迹: 把已存点缩放到方框里
local function page_track(fix)
  draw_header("TRACK")

  -- 方框给底部 3 行文字让位: 3 行 × 16px, 从 H-60 起 → 到 H-60+16=H-44 … 末行 H-28+16=H-12
  local bx0, by0, bx1, by1 = 8, SB_H + 26, W - 9, H - 68
  app.rect(bx0, by0, bx1, by1, BK)

  local n = G.tr_n < TRACK_MAX and G.tr_n or TRACK_MAX
  if n >= 2 then
    local la0, la1, lo0, lo1 = nil, nil, nil, nil
    for i = 1, n do
      local p = G.track[i]
      if p then
        if not la0 or p[1] < la0 then la0 = p[1] end
        if not la1 or p[1] > la1 then la1 = p[1] end
        if not lo0 or p[2] < lo0 then lo0 = p[2] end
        if not lo1 or p[2] > lo1 then lo1 = p[2] end
      end
    end
    local dla = (la1 - la0) > 0 and (la1 - la0) or 1
    local dlo = (lo1 - lo0) > 0 and (lo1 - lo0) or 1
    local iw = (bx1 - bx0) - 8
    local ih = (by1 - by0) - 8
    local px, py = nil, nil
    for i = 1, n do
      local p = G.track[i]
      if p then
        local x = bx0 + 4 + ((p[2] - lo0) * iw) // dlo
        local y = by1 - 4 - ((p[1] - la0) * ih) // dla   -- 纬度向上
        -- 点稀疏时连成线, 更看得出路线
        if px and (math.abs(x - px) > 1 or math.abs(y - py) > 1) then
          local steps = math.max(math.abs(x - px), math.abs(y - py))
          for s = 1, steps do
            app.px(px + (x - px) * s // steps, py + (y - py) * s // steps, BK)
          end
        else
          app.px(x, y, BK)
        end
        px, py = x, y
      end
    end
  else
    app.mixc(W // 2, (by0 + by1) // 2 - 8, "等待轨迹", false)
  end

  -- 底部三行: 坐标 / 海拔+航向 / 点数+距离  (末行必须 ≤ H-16, 否则被屏幕切掉)
  local yy = H - 60
  if fix and fix.valid then
    app.mix(8, yy, "坐标 " .. fmt_dms(fix.lat_e6, "N", "S") .. " " .. fmt_dms(fix.lon_e6, "E", "W"), false)
    app.mix(8, yy + 16, "海拔 " .. tostring(fix.alt_m or 0) .. "m  航向 " .. fmt_tenth(fix.crs10 or 0) .. "度", false)
    app.mix(8, yy + 32, "点数 " .. tostring(n) .. "  距离 " .. fmt_km(G.dist_dm) .. "km", false)
  else
    app.mixc(W // 2, yy + 16, "无定位", false)
  end
end

-- 上机调 GPS 就看这页
local function page_diag(fix)
  draw_header("GPS INFO")

  local y = SB_H + 30
  local function ln(label, val, inv)
    cell(8, y, label, val, inv)
    y = y + 20
  end

  if not fix then
    cell(8, y, "状态", "无数据")
    return
  end

  ln("定位", fix.valid and "有效" or "无效", not fix.valid)
  ln("质量", tostring(fix.quality or 0) .. "  维度 " .. tostring(fix.dim or 0) .. "D")
  ln("卫星", tostring(fix.sats or 0) .. " 颗")
  ln("HDOP", tostring((fix.hdop100 or 0) // 10) .. "." .. tostring((fix.hdop100 or 0) % 10))
  ln("海拔", tostring(fix.alt_m or 0) .. " m")
  ln("句子", tostring(fix.sentences or 0) .. "  校验错 " .. tostring(fix.badcs or 0))
  -- G.src 本身在模拟时已经是"模拟", 再拼一次会变成"模拟 (模拟)"
  ln("数据源", G.src)
  ln("已收点", tostring(G.tr_n))

  -- 底下一行: 上机排查的提示
  app.mix(8, H - 20, "无句子=线或波特率 校验错多=波特率", false)
end

-- ================= 契约函数 =================

function render()
  app.clear()
  local fix = G.fix
  if G.page == 1 then page_main(fix)
  elseif G.page == 2 then page_track(fix)
  else page_diag(fix) end
end

function on_enter()
  G.page = 1
  G.rec = false
  G.t_ms, G.dist_dm, G.max_kmh10 = 0, 0, 0
  G.track, G.tr_n, G.tr_full = {}, 0, false
  G.p_lat, G.p_lon = nil, nil
  G.tr_lat, G.tr_lon = nil, nil
  G.hr = nil
  G.last_tick = app.millis() or 0
  G.key = nil

  -- GPS 起不来不算错误: 降级到模拟, 界面照样能用
  if app.gps and app.gps.start then
    if not app.gps.start() then
      G.src = "启动失败"
      app.log("[bikecomp] gps.start() 失败, 走模拟")
    end
  end
  app.redraw()
end

function on_exit()
  G.track = {}
end

-- 缓存 key: 任何"会变的显示值"都要进来, 否则界面停了不刷新。
-- ⚠ 秒数按秒取整再进 key —— 放毫秒会让 poll 每帧都重画。
local function ui_key(fix)
  local kmh = fix and fix.kmh10 or -1
  local sats = fix and fix.sats or -1
  local ok = (fix and fix.valid) and 1 or 0
  return table.concat({
    G.page, G.rec and 1 or 0, kmh, sats, ok,
    G.dist_dm // 100, G.t_ms // 1000, G.max_kmh10, G.tr_n // 8,
    fix and (fix.crs10 or -1) or -1,
    fix and (fix.alt_m or -1) or -1,
  }, "|")
end

function poll()
  local now = app.millis() or 0
  local dt = now - (G.last_tick or now)
  G.last_tick = now
  if dt < 0 or dt > 5000 then dt = 0 end       -- 首帧/异常时基不参与累计

  local fix = read_fix()
  G.fix = fix
  accumulate(fix, dt)

  local k = ui_key(fix)
  if k ~= G.key then
    G.key = k
    app.redraw()
  end
end

function action(a)
  if type(a) ~= "number" then return true end
  if a == A_LEFT then
    G.page = (G.page - 2) % PAGE_N + 1
  elseif a == A_RIGHT then
    G.page = G.page % PAGE_N + 1
  elseif a == A_OK then
    G.rec = not G.rec
    if app.tone_async then app.tone_async(G.rec and 1200 or 700, 60) end
  elseif a == A_UP or a == A_DOWN then
    -- 上下 = 快速切页 (给没有触摸的掌机留一手)
    G.page = (a == A_UP) and ((G.page - 2) % PAGE_N + 1) or (G.page % PAGE_N + 1)
  elseif a == A_BACK then
    app.exit()
  end
  app.redraw()
  return true          -- 恒 true: BACK 语义由本应用自己拥有, 框架不要替我 pop
end
