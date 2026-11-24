--@lapp v1
--@app name=码表
--@app cat=程序
--@app desc=自行车码表: GPS 速度/里程/轨迹, 心率栏已预留
--@app version=1
--@app build=202611232005
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
-- ★ 2026-11-23: 状态栏高度一律向 OS 要 (app.statusbar_h), 不写死。
local SB_H = (app.statusbar_h and app.statusbar_h()) or 24
-- 版式栅格: 横屏 400×300 / 竖屏 300×400 (设置里可切, 走 app.rotate —— 与阅读器同一套)。
-- ★ 应用内不画标题: 状态栏之下直接就是内容。
local PAD, CT, CB = 8, SB_H + 4, 248
local NAV_LIDY, NAV_VALLEY, NAV_SLOPE, NAV_TY = 269, 294, 25, 273  -- 原 nd_draw_nav 尺寸
-- 药丸按钮: 内容区底部、导航条之上, 不压谷线导航条
local PILL_X0, PILL_Y0, PILL_X1, PILL_Y1 = 236, 200, 392, 230
local G                                        -- 前向声明: relayout 在 G 定义之前就要用它

-- 每次绘制/命中前重算栅格。横竖屏两套尺寸, 但三页共用同一套边距语义。
local function relayout()
  if G.port then
    W, H = 300, 400
    PAD  = 8
    CT   = 10                                    -- 竖屏不画系统状态栏(见 render)
    CB   = H - 52
    NAV_LIDY, NAV_VALLEY = H - 58, H - 6
    NAV_SLOPE = NAV_VALLEY - NAV_LIDY
    NAV_TY     = H - 23
    NAV_LIDY   = NAV_TY - 4
    NAV_VALLEY = H - 6
    NAV_SLOPE  = NAV_VALLEY - NAV_LIDY
    PILL_X0, PILL_Y0 = PAD, H - 106
    PILL_X1, PILL_Y1 = W - PAD, H - 76
  else
    W, H = 400, 300
    PAD  = 8
    CT   = SB_H + 4
    CB   = 248
    NAV_TY     = H - 23          -- 与原 nd_draw_nav 一致
    NAV_LIDY   = NAV_TY - 4
    NAV_VALLEY = H - 6
    NAV_SLOPE  = NAV_VALLEY - NAV_LIDY
    PILL_X0, PILL_Y0 = 236, 200
    PILL_X1, PILL_Y1 = 392, 230
  end
end

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
G = {
  page = 1,
  rec = false,          -- 记录中
  t_ms = 0,             -- 累计移动时间 (自动暂停: 低速不计)
  dist_dm = 0,          -- 累计距离 (分米)
  max_kmh10 = 0,        -- 最高速 (0.1km/h)
  p_lat = nil, p_lon = nil,          -- 上一次 poll 的位置 (判跳点/算里程)
  tr_lat = nil, tr_lon = nil,        -- 上一个轨迹点的位置 (判打点间距)
  last_tick = 0,
  track = {}, tr_n = 0, tr_full = false,
  spd_hist = {}, spd_n = 0,          -- 速度走势 (每 2s 一点)
  alt_hist = {}, alt_n = 0,          -- 海拔剖面 (每 2s 一点)
  hist_ms = 0,                       -- 上次采样时间
  hr = nil,             -- 心率 (预留: 后续接 BLE 心率带 / Garmin 手表广播)
  fix = nil,            -- 最新定位快照
  src = "初始化",        -- 数据源角标
  key = nil,
  tdown = false,        -- 触摸按下沿 (触摸适配用)
  force_sim = false,    -- 设置里可强制走模拟数据源 (GPS 没接时演示/自检)
  port = false,         -- 显示方向: false=横屏 400x300 / true=竖屏 300x400 (设置里切)
  menu = nil,           -- 设置菜单层级 (root/dir/data), 二级菜单用
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
local SIM = { t0 = nil, t_last = nil, latf = 39.904200, lonf = 116.407400, crs = 0 }  -- 演示轨迹起点: 通用坐标

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
  if G.force_sim then                 -- 设置里选了"强制模拟": 不启动 GPS 也能演示/自检
    G.src = "模拟"
    return sim_fix()
  end
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

  -- 走势采样: 每 2 秒记一点 (60 点 = 2 分钟窗口), 供主界面走势条与海拔剖面用
  G.hist_ms = G.hist_ms + dt_ms
  if G.hist_ms >= 2000 then
    G.hist_ms = 0
    local HN = 60
    G.spd_n = G.spd_n + 1
    G.spd_hist[((G.spd_n - 1) % HN) + 1] = k
    G.alt_n = G.alt_n + 1
    G.alt_hist[((G.alt_n - 1) % HN) + 1] = fix.alt_m or 0
  end
end

-- 均速 (0.1km/h, 纯整数算, 避免浮点): dist_dm*3.6/t_s
local function avg_kmh10()
  local ts = G.t_ms // 1000
  if ts < 1 or G.dist_dm < 1 then return 0 end
  return (G.dist_dm * 36) // (10 * ts)
end

-- ================= 绘制 =================
-- 版式参考 Helm One：**左大数字卡 + 右长驻数据列 + 底部走势条**，
-- 单色屏上没有颜色可用，所以层次全靠「字号 + 留白 + 图案」拉开。
--
-- ⚠ 两侧文字都必须走 app.mix：
--   ① app.textsb 是**纯 ASCII** 字体 → 送汉字画成 '?'；
--   ② app.zh 是**汉字专用**字体 → 送 ASCII 画成空白。
--   app.mix 两头都认，用它就不用按内容挑字体。

local function kv(xl, xr, y, label, value, inv)
  local on = inv and true or false
  app.mix(xl, y, label, on)
  local vw = app.mixw(value)
  app.mix(xr - vw, y, value, on)
end

-- 记录胶囊的位置 (绘制与点击命中共用同一份几何 —— 两处各写一套必然错位)
local function disk(cx, cy, r, filled)
  for dy = -r, r do
    local dx = math.floor(math.sqrt(r * r - dy * dy) + 0.5)
    app.hline(cx - dx, cx + dx, cy + dy, BK)
  end
  if not filled then                       -- 空心: 再用白填内圈
    local rr = r - 1
    for dy = -rr, rr do
      local dx = math.floor(math.sqrt(rr * rr - dy * dy) + 0.5)
      app.hline(cx - dx, cx + dx, cy + dy, WH)
    end
  end
end

-- ★ 2026-11-23 linit 规则: **应用内不画标题栏**（"骑行/轨迹/数据"这类页名一律不占行）。
--   状态栏之下直接排版内容; 当前页由底部中间提示表达。已无 draw_header。
-- 速度走势条：最近 60 个采样画成竖条，一眼看出"刚才快还是慢"
local function spd_bar(x0, y0, x1, y1)
  local n = G.spd_n < 60 and G.spd_n or 60
  if n < 1 then return end
  local mx = 1
  for i = 1, n do
    local v = G.spd_hist[i] or 0
    if v > mx then mx = v end
  end
  -- 固定柱宽 4px（柱宽随点数变化会让"只有一个采样"变成一整片黑）
  local bw = 4
  local cap = (x1 - x0 + 1) // bw
  local first = (n > cap) and (n - cap + 1) or 1
  for i = first, n do
    local h = (y1 - y0) * (G.spd_hist[i] or 0) // mx
    if h < 3 then h = 3 end                 -- 有速度就至少给 3px，否则看不出来
    if h > (y1 - y0) then h = y1 - y0 end
    local bx = x0 + (i - first) * bw
    app.fill(bx, y1 - h, bx + bw - 2, y1, BK)
  end
end

-- 海拔剖面：折线 + 上下刻度
local function alt_profile(x0, y0, x1, y1)
  app.rect(x0, y0, x1, y1, BK)
  local n = G.alt_n < 60 and G.alt_n or 60
  if n < 2 then
    app.mixc((x0 + x1) // 2, (y0 + y1) // 2 - 8, "暂无海拔", false)
    return
  end
  local mn, mx = 99999, -99999
  for i = 1, n do
    local v = G.alt_hist[i] or 0
    if v < mn then mn = v end
    if v > mx then mx = v end
  end
  if mx - mn < 5 then mx = mn + 5 end
  local iw = (x1 - x0) - 10
  local ih = (y1 - y0) - 12
  local px, py = nil, nil
  for i = 1, n do
    local v = G.alt_hist[i] or 0
    local x = x0 + 5 + (i - 1) * iw // (n - 1)
    local y = y1 - 5 - (v - mn) * ih // (mx - mn)
    if px then
      local steps = math.max(math.abs(x - px), math.abs(y - py))
      if steps > 0 then
        for s = 1, steps do
          app.px(px + (x - px) * s // steps, py + (y - py) * s // steps, BK)
        end
      else
        app.px(x, y, BK)
      end
    else
      app.px(x, y, BK)
    end
    px, py = x, y
  end
  app.mix(x1 - 46, y0 + 3, tostring(mx) .. "m", false)
  app.mix(x1 - 46, y1 - 17, tostring(mn) .. "m", false)
end

-- ① 主界面：左大速度 + 右 KPI 列 + 走势条
--   版式取开源码表的通用做法 (Wahoo/Garmin 的 "速度主导" 布局):
--   一屏只有一个视觉主角 (速度), 其余数据用小字右对齐成列, 只靠"分隔细线"分区,
--   1bpp 屏上不用大块边框 —— 边框多了整屏发黑、数字反而不突出。
-- 「开始骑行」= 底部按键区的**药丸按钮**(iOS 主操作样式):
--   实心黑底 + 反白字, 待录写「开始骑行」, 录制中写「暂停骑行」。
--   位置/命中复用同一组几何常量(PILL_*), 绘制与点击不会错位。
local function draw_pill_btn()
  local r  = (PILL_Y1 - PILL_Y0) // 2
  local cy = (PILL_Y0 + PILL_Y1) // 2
  app.fill(PILL_X0 + r, PILL_Y0, PILL_X1 - r, PILL_Y1, BK)
  for dy = -r, r do
    local dx = math.floor(math.sqrt(math.max(0, r * r - dy * dy)))
    app.hline(PILL_X0 + r - dx, PILL_X0 + r + dx, cy + dy, BK)
    app.hline(PILL_X1 - r - dx, PILL_X1 - r + dx, cy + dy, BK)
  end
  app.mixc((PILL_X0 + PILL_X1) // 2, cy - 8, G.rec and "暂停骑行" or "开始骑行", true)
end

local function page_main(fix)
  local k = fix and fix.kmh10 or 0
  if G.port then
    -- 竖屏: 速度居中 -> 2×2 KPI -> 走势 -> 大圆钮
    app.bigc(W // 2, CT + 8, fmt_tenth(k), 5, false)
    app.mixc(W // 2, CT + 92, "km/h", false)
    app.hline(PAD, W - PAD, CT + 116, BK)
    local rows = {
      { "里程", fmt_km(G.dist_dm) .. "km", "用时", fmt_hms(G.t_ms) },
      { "均速", fmt_tenth(avg_kmh10()), "最高", fmt_tenth(G.max_kmh10) },
    }
    local ry = CT + 124
    for i = 1, #rows do
      kv(PAD + 2, 140, ry, rows[i][1], rows[i][2], false)
      kv(156, W - PAD, ry, rows[i][3], rows[i][4], false)
      ry = ry + 26
    end
    app.mix(PAD, ry + 2, "走势", false)
    app.mix(W - PAD - app.mixw(tostring(fix and fix.sats or 0) .. " 星"), ry + 2,
            tostring(fix and fix.sats or 0) .. " 星", false)
    spd_bar(PAD, ry + 22, W - PAD, PILL_Y0 - 10)
    draw_pill_btn()
    return
  end

  local lx0, lx1 = PAD + 2, 236
  local cx = (lx0 + lx1) // 2
  app.bigc(cx, CT + 6, fmt_tenth(k), 6, false)        -- 6 倍字 ≈ 96px 高
  app.mixc(cx, CT + 108, "km/h", false)
  app.hline(lx0, lx1, CT + 132, BK)
  app.mix(lx0 + 2, CT + 136, "速度走势", false)
  app.mix(lx1 - app.mixw(tostring(fix and fix.sats or 0) .. " 星"),
          CT + 136, tostring(fix and fix.sats or 0) .. " 星", false)
  spd_bar(lx0, CT + 158, lx1, CB)

  -- 右上 KPI 列 (4 行): 标签左 / 数值右, 行间 1px 细线
  local rx0, rx1 = 246, W - PAD
  local rows = {
    { "里程", fmt_km(G.dist_dm) .. "km" },
    { "用时", fmt_hms(G.t_ms) },
    { "均速", fmt_tenth(avg_kmh10()) },
    { "最高", fmt_tenth(G.max_kmh10) },
  }
  local ry = CT + 4
  for i = 1, #rows do
    kv(rx0, rx1, ry, rows[i][1], rows[i][2], false)     -- 16px 字占 ry..ry+15
    ry = ry + 34
    if i < #rows then app.hline(rx0, rx1, ry - 6, BK) end   -- 文字下留 12px, 不压字
  end
  draw_pill_btn()


end

-- ② 轨迹页：上轨迹图 + 中海拔剖面 + 下三行
local function page_track(fix)
  local map_h = G.port and 136 or 68            -- 竖屏多出的高度全给地图
  local bx0, by0, bx1, by1 = PAD, CT + 4, W - PAD, CT + 4 + map_h
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
        local y = by1 - 4 - ((p[1] - la0) * ih) // dla
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

  -- 中：海拔剖面
  alt_profile(PAD, by1 + 8, W - PAD, by1 + (G.port and 112 or 66))

  -- 下：两行概要 (16px 字, 行距 20; 必须收在 CB 以内, 不许压到底部导航条)
  local yy = by1 + (G.port and 124 or 78)
  if fix and fix.valid then
    app.mix(PAD, yy, "坐标 " .. fmt_dms(fix.lat_e6, "N", "S") .. " " .. fmt_dms(fix.lon_e6, "E", "W"), false)
    app.mix(PAD, yy + 20, "海拔 " .. tostring(fix.alt_m or 0) .. "m  航向 " .. fmt_tenth(fix.crs10 or 0)
            .. "度  点数 " .. tostring(n), false)
  else
    app.mixc(W // 2, yy + 10, "无定位", false)
  end
end

-- ③ 数据页：十项两列（版式同 Helm One 的「完整数据」）
local function page_diag(fix)
  local hd = function(v) return tostring((v or 0) // 10) .. "." .. tostring((v or 0) % 10) end
  local rows = {
    { "定位", (fix and fix.valid) and "有效" or "无效", not (fix and fix.valid) },
    { "卫星", tostring(fix and fix.sats or 0) .. " 颗", false },
    { "海拔", tostring(fix and fix.alt_m or 0) .. " m", false },
    { "HDOP", hd(fix and fix.hdop100), false },
    { "句子", tostring(fix and fix.sentences or 0), false },
    { "校验错", tostring(fix and fix.badcs or 0), (fix and (fix.badcs or 0) > 0) or false },
    { "维度", tostring(fix and fix.dim or 0) .. "D", false },
    { "已收点", tostring(G.tr_n), false },
    { "数据源", G.src, false },
    { "均速", fmt_tenth(avg_kmh10()) .. " km/h", false },
  }

  if G.port then
    -- 竖屏: 单列 10 行 (300 宽放不下两列)
    local y0, dy = CT + 6, 32
    for i = 1, 10 do
      kv(PAD + 2, W - PAD, y0 + (i - 1) * dy, rows[i][1], rows[i][2], rows[i][3])
    end
  else
    local y0, dy = CT + 10, 36
    for i = 1, 10 do
      local col = (i - 1) // 5
      local row = (i - 1) % 5
      local xl = (col == 0) and (PAD + 2) or 206
      local xr = (col == 0) and 190 or (W - PAD)
      kv(xl, xr, y0 + row * dy, rows[i][1], rows[i][2], rows[i][3])
    end
    app.vline(198, CT + 8, CB - 6, BK)        -- 两列之间的分隔线
    app.mix(PAD, CB - 16, "无句子=接线/波特率  校验错多=波特率", false)
  end
end

-- ================= 谷线导航条 Valley Nav（左右斜坡 + 谷线） =================
-- 正式名字「谷线导航条」(Valley Nav)，原始实现 page_netdect.c: nd_draw_nav。
-- 本页用法: 左斜坡 = 设置（保持原样）; 右斜坡 = 下一页; 中间缺口 = 当前页提示。
-- 主操作「开始骑行」是内容区里的独立大圆钮, 不放在导航条里。
-- ⛔ 弹窗不许自绘: 设置走 app.dlg.list（规范 §5.1）。
local PAGE_NAME = { "骑行", "轨迹", "数据" }

local function nav_left_text()  return "设置" end
local function nav_right_text() return "下一页" end
local function nav_left_end()   return PAD + app.mixw(nav_left_text()) + 10 end
local function nav_right_start() return W - PAD - app.mixw(nav_right_text()) - 10 end

-- 完全照抄 page_netdect.c nd_draw_nav 的几何: 两个选项都在斜坡里, 中间只放一句话。
-- ⛔ 不要往导航条上塞按钮/分隔线 —— 谷线样式固定, 不允许改。
local function draw_nav()
  local up, dn = nav_left_text(), nav_right_text()
  local wu, wd = app.mixw(up), app.mixw(dn)
  local xu, xd = PAD, W - PAD - wd
  app.mix(xu, NAV_TY, up, false)
  app.mix(xd, NAV_TY, dn, false)

  local Lend   = xu + wu + 10
  local Rstart = xd - 10
  app.hline(0, Lend, NAV_LIDY, BK)               -- 左横杠贴左屏幕边框
  app.hline(Rstart, W - 1, NAV_LIDY, BK)         -- 右横杠贴右屏幕边框
  local vl, vr = Lend + NAV_SLOPE, Rstart - NAV_SLOPE
  if vl < vr then app.hline(vl, vr, NAV_VALLEY, BK) end
  for d = 0, NAV_SLOPE do                        -- 两条 45° 斜坡
    app.px(Lend + d, NAV_LIDY + d, BK)
    app.px(Rstart - d, NAV_LIDY + d, BK)
  end
  app.mixc((vl + vr) // 2, NAV_TY, PAGE_NAME[G.page] .. "  " .. G.page .. "/" .. PAGE_N, false)
end

-- ---- 设置弹窗: 一律走系统 os_dialog（规范 §5.1, 不许自绘）----
local function reset_ride()
  G.t_ms, G.dist_dm, G.max_kmh10 = 0, 0, 0
  G.track, G.tr_n, G.tr_full = {}, 0, false
  G.p_lat, G.p_lon = nil, nil
  G.tr_lat, G.tr_lon = nil, nil
  G.spd_hist, G.spd_n, G.hist_ms = {}, 0, 0
  G.alt_hist, G.alt_n = {}, 0
end

local function open_root()
  if not (app.dlg and app.dlg.list) then return end
  G.menu = "root"
  app.dlg.list("设置", {
    "显示方向  " .. (G.port and "竖屏" or "横屏"),
    "记录与数据",
    "关于",
  }, 1, nil)
end

local function open_dir()
  G.menu = "dir"
  app.dlg.list("显示方向", {
    (G.port and "横屏" or "[当前] 横屏"),
    (G.port and "[当前] 竖屏" or "竖屏"),
  }, G.port and 2 or 1, nil)
end

local function open_data()
  G.menu = "data"
  app.dlg.list("记录与数据", {
    "记录  " .. (G.rec and "进行中" or "已暂停"),
    "清空本次数据",
    "数据源  " .. (G.force_sim and "强制模拟" or "自动"),
  }, 1, nil)
end

-- 方向切换: 驱动 + UI 宽高判据 + 触摸坐标 三处一起改（app.rotate 内部做完, 与阅读器同一套）
local function set_orient(port)
  if G.port == port then return end
  G.port = port
  relayout()
  if app.rotate then app.rotate(port and 2 or 0) end
  app.redraw()
end

local function settings_result(r)
  local menu = G.menu or "root"
  G.menu = nil
  if not r or r < 1 then return end
  if menu == "root" then
    if r == 1 then open_dir()
    elseif r == 2 then open_data()
    elseif app.dlg.toast then app.dlg.toast("码表 v1  数据源 app.gps(降级模拟)") end
  elseif menu == "dir" then
    set_orient(r == 2)
    open_root()
  else
    if r == 1 then
      G.rec = not G.rec
      if app.tone_async then app.tone_async(G.rec and 1200 or 700, 60) end
      open_data()
    elseif r == 2 then
      reset_ride()
      if app.dlg.toast then app.dlg.toast("已清空本次数据") end
    else
      G.force_sim = not G.force_sim
      open_data()
    end
  end
end

-- 单击分发: 内容区大圆钮 = 开始/暂停骑行; 左斜坡 = 设置; 右斜坡 = 下一页; 中间提示不响应。
-- 切页也支持左右滑动(>=50px)。
local function on_tap(x, y)
  if x >= PILL_X0 - 6 and x <= PILL_X1 + 6 and y >= PILL_Y0 - 6 and y <= PILL_Y1 + 6 then
    G.rec = not G.rec
    if app.tone_async then app.tone_async(G.rec and 1200 or 700, 60) end
    return true
  end
  if y < NAV_LIDY - 10 then return false end
  if x <= nav_left_end() then open_root(); return true end
  return false                                   -- 中间提示区: 不响应
end

-- ================= 契约函数 =================

function render()
  relayout()                                   -- 横竖屏两套栅格, 每帧按当前方向重算
  -- ★ 2026-11-23 状态栏统一: 只声明"本页要", 时间/电量/Wi-Fi/蓝牙由 OS 每帧刷新。
  --   竖屏(300×400)时状态栏占用整行且方向不匹配, 所以竖屏一律不要系统状态栏。
  --   内容一律从 SB_H 之下开始画 —— 以前没人画这 24px, 顶上顶着上一屏的残影。
  if app.statusbar then app.statusbar(not G.port) end
  app.clear()
  local fix = G.fix
  if G.page == 1 then page_main(fix)
  elseif G.page == 2 then page_track(fix)
  else page_diag(fix) end
  draw_nav()
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
  G.tdown = false          -- 触摸按下沿状态 (进页面复位, 否则第一次点击会被当成"一直在按")
  G.force_sim = false

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
    G.force_sim and 1 or 0, G.src,
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

  relayout()                                   -- 命中判定要用当前方向的几何
  -- 触摸: 抬起时判定「单击」或「左右滑动切页」。
  -- · 按下 24px 内位移才算单击 (防止滑动误触斜坡选项);
  -- · 水平位移 >=50px 视为翻页 (左滑=下一页, 右滑=上一页);
  -- · 弹窗期间的触摸由 Lua 桥屏蔽, 不会穿透到这里。
  local tx, ty = app.touch()
  local dn = (tx ~= nil)
  if dn then
    if not G.tdown then G.tx0, G.ty0, G.tap = tx, ty, true end
    G.tx_last = tx
    if math.abs(tx - G.tx0) > 24 or math.abs(ty - G.ty0) > 24 then G.tap = false end
  elseif G.tdown then
    local dx = (G.tx_last or G.tx0 or 0) - (G.tx0 or 0)
    if G.tap then
      if on_tap(G.tx0 or 0, G.ty0 or 0) then app.redraw() end
    elseif math.abs(dx) >= 50 then
      if dx < 0 then G.page = G.page % PAGE_N + 1
      else G.page = (G.page - 2) % PAGE_N + 1 end
      app.redraw()
    end
    G.tap = false
  end
  G.tdown = dn

  -- 设置弹窗结果: app.dlg.result() 是"取走即清", 每帧可安全调用
  if app.dlg and app.dlg.result then
    local r = app.dlg.result()
    if r ~= nil then
      settings_result(r)
      app.redraw()
    end
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
