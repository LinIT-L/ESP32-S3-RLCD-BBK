--@lapp v1
--@app name=矢量地图
--@app cat=工具
--@app desc=离线矢量地图: 道路/等高线双图层, 任意缩放不糊, GPS 定位
--@app version=1.0
--@app icon=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP///+f///8B////AH///4OACAAYAAgBwwAIAP8ACADDAAgD/8AIAMMACAf/4AgAwwAID//wCADDAAgP//gIAMMACB/n+AgAwwAIH4H4CADDAAgfgfwIAMMACD8A/AgAwwAIPwD8CADDABgfgfwYAMP//5+B+P//wwAIH+f4CADDAAgP//gIAMMACA//8AgAwwAIB//gCADDAAgD/8AIAMMACAP/wAgAwwAIAf+ACADDAAgB/4AIAMMACAD/gAgAwwAIAP8ACADDAAgA/wAIAMMACAB/AAgAwwAIAH4ACADDABgAfgAYAMP/////////wwAIADwACADDAAgAPAAIAMMACAAYAAgAwwAIABgACADDAAgACAAIAMMACAAAAAgAwwAIAAAACADDgAgAAAAIAMH/////////gP////////8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
-- 图标自绘: blit_app_icon(x, y[, inv=true])
-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP///+f///8B////AH///4OACAAYAAgBwwAIAP8ACADDAAgD/8AIAMMACAf/4AgAwwAID//wCADDAAgP//gIAMMACB/n+AgAwwAIH4H4CADDAAgfgfwIAMMACD8A/AgAwwAIPwD8CADDABgfgfwYAMP//5+B+P//wwAIH+f4CADDAAgP//gIAMMACA//8AgAwwAIB//gCADDAAgD/8AIAMMACAP/wAgAwwAIAf+ACADDAAgB/4AIAMMACAD/gAgAwwAIAP8ACADDAAgA/wAIAMMACAB/AAgAwwAIAH4ACADDABgAfgAYAMP/////////wwAIADwACADDAAgAPAAIAMMACAAYAAgAwwAIABgACADDAAgACAAIAMMACAAAAAgAwwAIAAAACADDgAgAAAAIAMH/////////gP////////8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
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
-- map.lua — 矢量离线地图 · 导航记录仪 (Lua 单文件应用)
--
-- 数据: `.ltm` 紧凑二进制**矢量**包 (tools/map_vec.py 从 OSM Overpass + terrarium DEM 编译)。
--       只存折线点串, 不存图片 —— 任意缩放都清晰, 文字永远是固件点阵不会糊,
--       一份数据覆盖全部缩放档 (位图地图则每个 z 档都要单独存一张)。
--
-- ★ 数据**只从 TF 卡读**: /sdcard/Map/putian_road.ltm + putian_contour.ltm
--   (linit 定案: 没有 TF 卡就不允许加载地图)。地图包可从商店托管站联网下载
--   (https://bbk.linit.cn/bbk/maps.json → 下载并校验 crc 后落到 /sdcard/Map/)。
--
-- 为什么 400×300 1bpp 上手绘上千条折线还不卡:
--   ① **按缩放选 LOD**: 要素表按等级排好, 缩略图只遍历前一段 (城市级只画主干),
--      放大才逐级放行小路。设备端只是换个下标区间, 不做任何计算。
--   ② **要素 bbox 预剔除**: 每条要素自带 bbox, 屏外的连点都不解。
--   ③ **Bresenham 合并同主轴连续段**: 一条水平路只调 **1 次** app.hline, 不是 400 次 app.px。
--   ④ **屏幕空间抽稀 MINSTEP**: 点距 < ~2px 不落笔, 落笔时从上次落笔点直连 ⇒ 线不断。
--   ⑤ **整数域算坐标, 只在最后一步转浮点**: 本项目 Lua 是 LUA_32BITS=1, lua_Number 是
--      **float** (只 ~7 位有效数字); lon_e6=119011000 有 9 位, 直接进浮点立刻被截断。
--
-- 版面:
--   y  0..23  系统状态栏 (内核)
--   y 24..56  指标条: 开始时间/移动距离/爬升高度/当前高度/当前配速 + 卫星(右)
--   y 57..299 地图 (全出血), 上面浮着: 右侧 放大/缩小/等高 · 左侧 收藏夹 · 中下 开始
--   抽屉      从**右侧**拉出 (点左侧"收藏夹"触发), 第一行恒定"收藏当前位置"
--
-- 契约: on_enter / render / poll / action / on_exit
-- 按键: 方向=平移  确认=回到当前位置  返回=关抽屉/退出(走全局「退出程序?」确认)
-- ============================================================================

local W = app.W or 400
local H = app.H or 300

local A_UP    = (type(app.UP)      == "number") and app.UP      or 1
local A_DOWN  = (type(app.DOWN)    == "number") and app.DOWN    or 2
local A_LEFT  = (type(app.LEFT)    == "number") and app.LEFT    or 3
local A_RIGHT = (type(app.RIGHT)   == "number") and app.RIGHT   or 4
local A_OK    = (type(app.CONFIRM) == "number") and app.CONFIRM or 5
local A_BACK  = (type(app.BACK)    == "number") and app.BACK    or 6
local BK, WH = 0, 1

-- ---- 版面 ----
local MB_Y0, MB_Y1 = 24, 56          -- 指标条 (标签 12px + 数值 16px)
local LBL_Y, VAL_Y = 25, 37
local CELL_W       = 70              -- 5 格 × 70 = 350, 右侧留给卫星
local MAP_T, MAP_B = 57, 299          -- 页面内容区 (顶部页签已并入左右两列)
local MX           = W / 2
local MY           = (MAP_T + MAP_B + 1) / 2

-- ★ 左右两列**按键**承担全部功能 (linit: 顶部页签"其实都有对应的功能", 合并到两边;
--   状态栏只显示状态, 不加边框)。
--   两组各自**整组垂直居中**, 共用同一条垂直中线。
local RB_X, RB_W, RB_H, RB_GAP = 351, 46, 24, 5
local RB_N = 4                    -- 放大 + / 缩小 - / 等高 / 收藏
local LB_X, LB_W = 3, 46
local LB_H = 24
local LB_N = 4                    -- 地图 / 行程 / 卫星 / 设置
local CONT_MY = (MAP_T + MAP_B + 1) / 2
local RB_TOP = CONT_MY - (RB_N * RB_H + (RB_N - 1) * RB_GAP) / 2
local LB_TOP = CONT_MY - (LB_N * LB_H + (LB_N - 1) * RB_GAP) / 2
local function rb_y(i) return RB_TOP + (i - 1) * (RB_H + RB_GAP) end
local function lb_y(i) return LB_TOP + (i - 1) * (LB_H + RB_GAP) end
local LB_LABEL = { "地图", "行程", "卫星", "设置" }
-- ★ 「地图」键 = 地图模式开关 (linit 定案): 同一个地图页, **开=等高图 / 关=普通图**,
--   不再单独占一个「等高」按键。空出来的右列第 3 格给「定位」(触摸端原来只能靠确认键回中)。
local RB_LABEL = { "+", "-", "定位", "收藏" }       -- 与左列同宽同高, 左右对称

local BX_W, BX_H = 78, 28                        -- 中下 开始
local BX_X, BX_Y = (W - BX_W) / 2, 262

local DR_W      = 202                            -- 抽屉宽 (从右侧拉出)
local DR_ROW_H  = 26
local DR_Y0     = MAP_T

local ZMIN, ZMAX, ZDEF = 10, 18, 14

-- ---- 数据: 只认 TF 卡 ----
local TIER0    = "/sdcard"
local MAP_DIRS = { "/sdcard/Map", "/sdcard/maps" }
-- 老的单文件包 (莆田): 没有清单, 靠固定文件名兜底识别
local LEGACY_ROAD    = "putian_road.ltm"
local LEGACY_CONTOUR = "putian_contour.ltm"
local LEGACY_POI     = "putian_poi.ltm"

-- ============================================================================
-- 两种地图包形态 (并存)
-- ============================================================================
-- ① 单文件包 (莆田): putian_road.ltm / _contour.ltm / _poi.ltm —— 无清单, 兜底识别
-- ② 分块包 (福建省): <id>.json 索引 + <id>.bin 数据体, 块按 (off,size) 内嵌在 bin 里
--
-- ★★ 为什么省级必须是 "1 个 bin + 索引" 而不是 "一瓦片一个 .ltm"
--   ① 单文件装不下: 福建省 4.9°×4.9°, 而要素 bbox 是 i2(1e-5°) 时单文件跨度上限
--      只有 0.3277°; 就算 v2 把 bbox 放宽到 i4, 整省路网 18 MB 也远超
--      `app.fs.read` 的 512 KB 上限 —— 根本读不进来。
--   ② 一瓦片一文件 = 1171 个 HTTP 请求 (福建 607 路块 + 564 标块), 设备端一个个下
--      二十分钟都下不完, 每个请求还要重来一遍 TLS 握手。
--   ③ 所以: 全部块**顺序平铺**进一个 .bin, 索引记每块 (off,size), 设备端用
--      `app.fs.read_at(bin, off, size)` 精确读当前视口那几块 —— 一次请求下完整省,
--      内存只驻留几块。见 docs/矢量离线地图-方案与实现.md「省级包」。
-- 从这一档起改用分块; 更小的 z 只看概览 (一屏上千平方公里, 逐块读反而更慢)。
-- 包里清单也带 ztile, 以包为准 —— 这里只是没有清单时的兜底值。
local TILE_Z_DEF = 11

-- 同时在内存里的块数 / 总字节上限。块内是 Lua 字符串 (PSRAM):
-- 福建 610 块, 中位 13.8 KB, 最大 364 KB ⇒ 24 块典型 330 KB, 戴上限 3 MB 也安全。
local TILE_CACHE  = 24
local TILE_BYTES  = 3 * 1024 * 1024
-- 视口外多留一圈块, 平移时不至于走一步读一次
local TILE_MARGIN = 1

local MAPS_URL = "https://bbk.linit.cn/bbk/maps.json"   -- 地图包索引 (商店托管站)

local SCALES = { 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000 }

-- ============================================================================
-- 状态
-- ============================================================================
local M = {
    z = ZDEF, clon = 119011000, clat = 25433500,
    road = nil, contour = nil, poi = nil, map_dir = nil,
    road_on = true, cont_on = true, labels = true, poi_on = true,
    err = nil, err_kind = nil,
    fix = nil, gps_ok = false,
    mpp = 1, hw = 0, hh = 0,
    dr_x = W, dr_on = false, dr_sel = 1,        -- 抽屉
    asking = nil, pending_nav = nil,             -- 弹窗等待
    nav = nil,                                   -- 导航目标 {lat, lon, name}
    unit = 0,                                    -- 0=公里 1=英里 (NVS 持久化)
    page = 1,                                    -- 1地图 2行程 3卫星 4设置
    last_ms = 0, last_sec = -1, key = nil,
    -- ---- 地图包 / 分块 ----
    pkgs = nil, pkgi = 1, pkg = nil,             -- 已发现 / 当前
    ov = nil,                                    -- 包内概览 (z<ztile 用)
    tidx = nil, tiles = nil, torder = nil,       -- 块的索引与已加载块
    tbytes = 0, tmiss = 0, tneed = 0,
}

local T = { started = false, on = false, wall = nil,
            dist_dm = 0, asc_m = 0, base_alt = nil, p_lat = nil, p_lon = nil,
            t_ms = 0, max_mps100 = 0, track = {}, tr_n = 0 }

local TRACK_MAX = 160                            -- 轨迹环缓冲点数 (对齐 bikecomp)
local F = { list = {} }                          -- 收藏夹

local D = { st = "idle", files = nil, n = 0, idx = 0, mirror = nil, cur = nil }

-- ============================================================================
-- 小工具
-- ============================================================================
local function pad(n, w)
    local s = tostring(n)
    while #s < w do s = "0" .. s end
    return s
end

local function fmt_km(dm)                        -- 分米 → "3.42"
    local c = dm // 100
    return (c // 100) .. "." .. pad(c % 100, 2)
end

-- 距离显示 (随单位设置)。dm = 分米。
local function dist_num(dm, mi)
    if not mi then return fmt_km(dm) end
    local m = dm // 10
    local c = (m * 100 + 80467) // 160934
    return (c // 100) .. "." .. pad(c % 100, 2)
end

local function fmt_dist(dm, mi)
    return dist_num(dm, mi) .. (mi and "mi" or "km")
end

local function fmt_dist_old(dm, mi)
    if not mi then return fmt_km(dm) .. "km" end
    -- 英里 = 米 / 1609.344; 显示两位小数 ⇒ ×100/1609.344 → 先算"百分之一英里"
    local m = dm // 10
    local c = (m * 100 + 80467) // 160934          -- 四舍五入到 0.01 mi
    return (c // 100) .. "." .. pad(c % 100, 2) .. "mi"
end

-- 配速 (分/单位距离)。mps100 是 cm/s。
--   公制: 1000 m ÷ (mps100/100) = 100000/mps100 秒每公里
--   英制: 1609.344 m → 160934/mps100 秒每英里
local function fmt_pace(mps100, mi)
    if not mps100 or mps100 < 30 then return "--'--" end
    local spk = (mi and 160934 or 100000) // mps100
    if spk > 3599 then return "--'--" end
    return (spk // 60) .. "'" .. pad(spk % 60, 2)
end

local function dist_m(lat1, lon1, lat2, lon2)
    local dy = (lat2 - lat1) * 0.111320
    local cl = math.cos((lat1 / 1000000) * math.pi / 180)
    local dx = (lon2 - lon1) * 0.111320 * cl
    return math.floor(math.sqrt(dx * dx + dy * dy) + 0.5)
end

local function clampi(v, a, b) return v < a and a or (v > b and b or v) end

-- ============================================================================
-- .ltm 解析 (格式见 docs/矢量离线地图-方案与实现.md)
-- ============================================================================
-- ⚠⚠ 跨语言陷阱: 4 字节定长串 —— Python 侧写 "<4sBBH…", Lua 侧必须写 "<c4BBH…"。
--    两个写法**不等价**: Python 的 "c4" = 4 个独立的 c (占 4 项), 语义完全不同。
local HDR_FMT  = "<c4BBH" .. "iiii" .. "IIIIIIII" .. "xxxxxxxx"
local LOD_FMT  = "<BBHIIII"
local LOD_LEN  = 20
local FEAT_FMT = "<BBHIiiii"        -- v2: bbox 是 4×i4 (1e-5 度相对 min) ——
                                 -- i2 只有 ±32767, 单文件跨度上限 0.3277°, 装不下一个省
-- ⛔⛔ 要素表步长必须与格式串**同步**: `<BBHIiiii` = 1+1+2+4 + 4×4 = **24 B**。
--   格式升 v2 时把 bbox 从 i2 改成 i4, 表从 16 B 变 24 B; 步长若还写 16,
--   遍历会整体错位 → 表现为"要素数对但一条路都画不出/画出乱线"。
local FEAT_LEN = 24
local FEAT_HDR  = "<BBHI"
local FEAT_HDR_F = "<BBHIii"     -- 地标: poff(I4) + bbox 左上角(2×i4)
                                 -- ⛔ 别写 "<BBHII": bbox 是 **i2**, 写成 I4 会把 bx0+by0 当成一个数读
local B1, B2, B4, B8 = "<hh", "<hhhh", "<hhhhhhhh", "<hhhhhhhhhhhhhhhh"
local HEAD_FMT = "<ii"

-- ⛔ 下载校验**不能用 CRC32**: 本项目 Lua 是 LUA_32BITS=1, lua_Integer 只有 32 位,
--   而 CRC32 的值常常 > 2^31 (如 2794397018) —— 既拿不到精确整数, 也过不了 %08x 格式化
--   (会被拒 "number has no integer representation")。改用 **Fletcher-16**:
--   全程只用到 <65536 的数, 在 32 位整数和 float 下都精确。
--   再配一个**字节数**比对, 于是"下载被截断"和"内容被改坏"都能抓到。
local function fletcher16(s)
    local a, b = 0, 0
    for i = 1, #s do
        a = (a + s:byte(i)) % 255
        b = (b + a) % 255
    end
    return b * 256 + a
end

local function ltm_parse(raw)
    if not raw or #raw < 64 then return nil end
    local h = { string.unpack(HDR_FMT, raw) }
    if h[1] ~= "LTM1" then return nil end
    local d = {
        layer = h[3], nlod = h[4],
        minlat = h[5], minlon = h[6], maxlat = h[7], maxlon = h[8],
        off_lod = h[9], off_feat = h[10], off_pt = h[11], off_str = h[12],
        nfeat = h[14], npt = h[15], crc = h[16],
        -- v3: 头部 56..63 是原来的填充位 → 56..59 存**空间索引偏移** (0 = 没有)
        off_idx = string.unpack("<I", raw, 57),
        raw = raw, lods = {}, bytes = #raw,
    }
    for k = 0, d.nlod - 1 do
        local cmax, w, zmin, f0, f1 = string.unpack(LOD_FMT, raw, d.off_lod + k * 20 + 1)
        d.lods[k + 1] = { cmax = cmax, w = w, zmin = zmin, f0 = f0, f1 = f1 }
    end
    return d
end

local function pick_lod(d, z)
    local sel = d.lods[1]
    for i = 1, d.nlod do
        if z >= d.lods[i].zmin then sel = d.lods[i] end
    end
    return sel
end

local function load_map()
    M.road, M.contour, M.poi, M.map_dir = nil, nil, nil, nil
    for i = 1, #MAP_DIRS do
        local r = ltm_parse(app.fs.read(MAP_DIRS[i] .. "/" .. LEGACY_ROAD))
        if r then M.road, M.map_dir = r, MAP_DIRS[i] break end
    end
    for i = 1, #MAP_DIRS do
        local c = ltm_parse(app.fs.read(MAP_DIRS[i] .. "/" .. LEGACY_CONTOUR))
        if c then M.contour = c break end
    end
    for i = 1, #MAP_DIRS do
        local q = ltm_parse(app.fs.read(MAP_DIRS[i] .. "/" .. LEGACY_POI))
        if q then M.poi = q break end
    end
    M.cont_on = false      -- 默认**普通地图**; 点左列第 1 键切成等高地图
end

-- ============================================================================
-- 地图包发现 / 载入 / 分块
-- ============================================================================
-- ⚠ 清单只放**顶层标量** + 一个对象数组 —— 桥里的 app.json.num/str 只认顶层键
--   (没有 "a.b" 路径表达式), pick() 也只能解"对象数组"。
local function pkg_from_json(raw, dir, fn)
    if not raw or raw:sub(1, 1) ~= "{" then return nil end
    local bin = app.json.str(raw, "bin")
    if not bin then return nil end
    return {
        id   = app.json.str(raw, "id") or fn:sub(1, -6),
        name = app.json.str(raw, "name") or fn:sub(1, -6),
        dir  = dir,
        json = fn,
        bin  = bin,
        binpath = dir .. "/" .. bin,
        jsonpath = dir .. "/" .. fn,
        tile  = app.json.num(raw, "tile", 0),
        ztile = app.json.num(raw, "ztile", TILE_Z_DEF),
        minlon = app.json.num(raw, "minlon", 0),
        minlat = app.json.num(raw, "minlat", 0),
        maxlon = app.json.num(raw, "maxlon", 0),
        maxlat = app.json.num(raw, "maxlat", 0),
        ov_off = app.json.num(raw, "ov_off", 0),
        ov_size = app.json.num(raw, "ov_size", 0),
        count = app.json.num(raw, "count", 0),
        bytes = app.json.num(raw, "bytes", 0),
        tiled = app.json.num(raw, "tile", 0) > 0,
    }
end

-- 扫 /sdcard/Map 下所有 *.json 当包清单 (索引 <id>.json 与数据体 <id>.bin 同名)。
-- 不额外维护"已安装包清单"文件: 目录本身就是真相源, 少一个会过期的中间层。
local function scan_pkgs()
    local out = {}
    for i = 1, #MAP_DIRS do
        local dir = MAP_DIRS[i]
        if app.fs.exists(dir) then
            local names = app.fs.listdir(dir)
            for k = 1, 24 do
                local fn = names[k]
                if not fn then break end
                if #fn > 5 and fn:sub(-5) == ".json" then
                    local p = pkg_from_json(app.fs.read(dir .. "/" .. fn), dir, fn)
                    if p then out[#out + 1] = p end
                end
            end
        end
        -- 兜底: 老的单文件包 (莆田), 没有清单。
        -- ⚠ 判重必须按"**同一个目录里已经有老式包**", 不能按"这个目录已经有包":
        --   实际布局下 putian_road.ltm 与 fj.json/fj.bin 都在 /sdcard/Map/,
        --   按目录判重会把老包整个吞掉。
        if app.fs.exists(dir .. "/" .. LEGACY_ROAD) then
            local dup = false
            for j = 1, #out do
                if out[j].json == nil and out[j].dir == dir then dup = true end
            end
            if not dup then
                out[#out + 1] = { id = "putian", name = "莆田", dir = dir,
                                  json = nil, bin = nil,
                                  jsonpath = nil, binpath = nil,
                                  tile = 0, ztile = 99, tiled = false,
                                  minlon = 0, minlat = 0, maxlon = 0, maxlat = 0,
                                  count = 0, bytes = 0 }
            end
        end
    end
    return out
end

-- 块网格: 索引里的 x/y 就是 floor(经度/块宽) 与 floor(纬度/块宽), 与编译器一致。
-- ⚠ 块宽用整数微度 (150000), 别用 0.15 × 1e6 的浮点结果 —— 边界上会差一块。
local function pkg_te(p) return math.floor(p.tile * 1e6 + 0.5) end
local function tile_key(lon_e6, lat_e6, te)
    return math.floor(lon_e6 / te) .. "_" .. math.floor(lat_e6 / te)
end

-- 起一个包: 概览 + 块索引 (块本体**按需**读, 不预载)
local function pkg_open(p)
    p.tidx = nil
    M.tiles, M.torder, M.tbytes, M.tmiss = {}, {}, 0, 0
    if not p.tiled then return end
    local raw = app.fs.read(p.jsonpath)
    if not raw then return end
    local rows = app.json.pick(raw, "tiles", "x,y,o,s,po,ps", 1600)
    local t = {}
    local n = 0
    for i = 1, 1600 do
        local r = rows[i]
        if not r then break end
        if r.x and r.y then
            t[r.x .. "_" .. r.y] = r            -- 行内字段 r.o/r.s (道路) / r.po/r.ps (地标)
            n = n + 1
        end
    end
    p.tidx = t
    p.nrow = n
end

local function pkg_load(p)
    M.pkg = p
    M.ov, M.road, M.contour, M.poi = nil, nil, nil, nil
    if p and p.tiled then
        pkg_open(p)
        if p.ov_size > 0 then
            M.ov = ltm_parse(app.fs.read_at(p.binpath, p.ov_off, p.ov_size))
        end
        if not M.ov then M.err_kind, M.err = "nofile", "包内没有概览层" end
    else
        load_map()                              -- 老单文件包
    end
end

-- 读一块 (道路或地标)。返回数据集或 nil。
local function block_get(p, key, kind)
    local ent = M.tiles[key]
    if ent and ent[kind] then return ent[kind] end
    local row = p.tidx and p.tidx[key]
    if not row then return nil end
    local off, sz = row.o, row.s
    if kind == "poi" then off, sz = row.po, row.ps end
    if not off or not sz or sz <= 0 then return nil end
    local ds = ltm_parse(app.fs.read_at(p.binpath, off, sz))
    if not ds then return nil end
    if not ent then ent = { road = nil, poi = nil, bytes = 0 }; M.tiles[key] = ent end
    ent[kind] = ds
    ent.bytes = (ent.bytes or 0) + sz
    M.tbytes = M.tbytes + sz
    return ds
end

-- 淘汰: 按**离视口中心的距离**扔最远的, 不是纯 LRU ——
-- 平移时刚离开屏幕的块马上又要回来, LRU 会把"马上要用"的块踢掉。
local function tile_evict(keep_key, te)
    local function count()
        local n = 0
        for _ in pairs(M.tiles) do n = n + 1 end
        return n
    end
    while M.tbytes > TILE_BYTES or count() > TILE_CACHE do
        local worst, wk = -1, nil
        for k, e in pairs(M.tiles) do
            if k ~= keep_key then
                local tx, ty = k:match("^(%-?%d+)_(%-?%d+)$")
                if tx then
                    local dx = ((tonumber(tx) + 0.5) * te - M.clon) / 1e6
                    local dy = ((tonumber(ty) + 0.5) * te - M.clat) / 1e6
                    local d = dx * dx + dy * dy
                    if d > worst then worst, wk = d, k end
                end
            end
        end
        if not wk then break end
        M.tbytes = M.tbytes - (M.tiles[wk].bytes or 0)
        M.tiles[wk] = nil
    end
end

-- 让**当前视口**的块就位。z 不够大就什么都不做 (用概览)。
local function tile_sync()
    local p = M.pkg
    if not p or not p.tiled or M.z < p.ztile then return end
    local te = pkg_te(p)
    local x0 = math.floor((M.clon - M.hw) / te) - TILE_MARGIN
    local x1 = math.floor((M.clon + M.hw) / te) + TILE_MARGIN
    local y0 = math.floor((M.clat - M.hh) / te) - TILE_MARGIN
    local y1 = math.floor((M.clat + M.hh) / te) + TILE_MARGIN
    M.tneed = (x1 - x0 + 1) * (y1 - y0 + 1)
    M.tmiss = 0
    for ty = y0, y1 do
        for tx = x0, x1 do
            local key = tx .. "_" .. ty
            local row = p.tidx and p.tidx[key]
            if row then
                if row.s and row.s > 0 and not (M.tiles[key] and M.tiles[key].road) then
                    if not block_get(p, key, "road") then M.tmiss = M.tmiss + 1 end
                end
                if M.poi_on and row.ps and row.ps > 0
                   and not (M.tiles[key] and M.tiles[key].poi) then
                    block_get(p, key, "poi")
                end
                tile_evict(key, te)
            end
        end
    end
end

-- ============================================================================
-- 绘图图元
-- ============================================================================
local function hl(xa, xb, y)
    if xb < xa then xa, xb = xb, xa end
    if xa < 0 then xa = 0 end
    if xb > W - 1 then xb = W - 1 end
    if xa <= xb and y >= MAP_T and y <= MAP_B then app.hline(xa, xb, y, BK) end
end

local function vl(x, ya, yb)
    if yb < ya then ya, yb = yb, ya end
    if ya < MAP_T then ya = MAP_T end
    if yb > MAP_B then yb = MAP_B end
    if ya <= yb and x >= 0 and x <= W - 1 then app.vline(x, ya, yb, BK) end
end

local function seg(x0, y0, x1, y1)
    local dx, dy = x1 - x0, y1 - y0
    local adx = dx < 0 and -dx or dx
    local ady = dy < 0 and -dy or dy
    local sx = dx < 0 and -1 or 1
    local sy = dy < 0 and -1 or 1
    if adx >= ady then
        local d, x, run = 2 * ady - adx, x0, x0
        while x ~= x1 do
            if d > 0 then hl(run, x, y0); run = x + sx; y0 = y0 + sy; d = d - 2 * adx end
            d = d + 2 * ady
            x = x + sx
        end
        hl(run, x1, y0)
    else
        local d, y, run = 2 * adx - ady, y0, y0
        while y ~= y1 do
            if d > 0 then vl(x0, run, y); run = y + sy; x0 = x0 + sx; d = d - 2 * ady end
            d = d + 2 * adx
            y = y + sy
        end
        vl(x0, run, y1)
    end
end

-- 粗线沿**法向**偏移再画一遍 (一律 +1 y 会让竖直线根本不增粗, 只是被拉长)
local function seg_w(x0, y0, x1, y1, w)
    seg(x0, y0, x1, y1)
    if w >= 2 then
        if math.abs(x1 - x0) >= math.abs(y1 - y0) then seg(x0, y0 + 1, x1, y1 + 1)
        else seg(x0 + 1, y0, x1 + 1, y1) end
    end
end

-- ⛔ app.mixw 是 **16px/字**(app.mix 的宽度), 而 app.zhsm 是 **12px/字** ——
--   拿 mixw 去居中一个 zhsm 串, 每个汉字多算 4px, 整体就偏左。必须单独量。
-- ⚠ UTF-8 续字节是 0x80..0xBF, **不是** "所有 <0xC0 的字节"。
--   写成 `b < 0xC0` 会把续字节也算成字: "行程"(E8 A1 8C E7 A8 8B) 会被数成 4 个字而不是 2,
--   于是宽度多算一倍, 文字反而更偏左 (踩过一次, 实测墨迹起点偏 -8px)。
local function zw12(s)
    local n = 0
    for i = 1, #s do
        local b = s:byte(i)
        if b < 0x80 or b >= 0xC0 then n = n + 1 end   -- ASCII 或 前导字节 = 一个新字
    end
    return n * 12
end

-- 圆角矩形填充 (桥里只有 fill/rect/hline/vline, **没有圆角图元**, 自己画)
-- 做法: 上下各 rr 行按圆方程收进, 中间用整块 fill 铺满。
local function rrect(x0, y0, x1, y1, rr, c)
    if x1 < x0 then x0, x1 = x1, x0 end
    if y1 < y0 then y0, y1 = y1, y0 end
    local mw = (x1 - x0 + 1) / 2
    local mh = (y1 - y0 + 1) / 2
    if rr > mw then rr = math.floor(mw) end
    if rr > mh then rr = math.floor(mh) end
    if rr < 1 then app.fill(x0, y0, x1, y1, c); return end
    for dy = 0, rr - 1 do
        local dr = rr - dy - 0.5
        local t = rr * rr - dr * dr
        local inset = rr - math.floor((t > 0 and math.sqrt(t) or 0) + 0.5)
        app.hline(x0 + inset, x1 - inset, y0 + dy, c)
        app.hline(x0 + inset, x1 - inset, y1 - dy, c)
    end
    app.fill(x0, y0 + rr, x1, y1 - rr, c)
end

local function ring(cx, cy, r)
    local x, y, d = r, 0, 1 - r
    while x >= y do
        app.px(cx + x, cy + y, BK); app.px(cx - x, cy + y, BK)
        app.px(cx + x, cy - y, BK); app.px(cx - x, cy - y, BK)
        app.px(cx + y, cy + x, BK); app.px(cx - y, cy + x, BK)
        app.px(cx + y, cy - x, BK); app.px(cx - y, cy - x, BK)
        y = y + 1
        if d < 0 then d = d + 2 * y + 1 else x = x - 1; d = d + 2 * (y - x) + 1 end
    end
end

-- 超迷你卫星图标 (12×12): 本体 + 左右太阳板 + 天线; 无定位时画空心+叉
local function sat_icon(x, y, on)
    app.fill(x, y, x + 11, y + 11, WH)
    if not on then
        app.rect(x, y, x + 11, y + 11, BK)
        app.texts(x + 2, y - 1, "x", BK)
        return
    end
    app.fill(x + 4, y + 5, x + 7, y + 7, BK)
    app.fill(x + 0, y + 5, x + 3, y + 6, BK)
    app.fill(x + 8, y + 5, x + 11, y + 6, BK)
    app.px(x + 8, y + 3, BK); app.px(x + 9, y + 2, BK)
    app.px(x + 10, y + 1, BK); app.px(x + 11, y + 0, BK)
end

-- ============================================================================
-- 投影 (墨卡托局部线性化; 每度经度占多少像素与纬度无关)
-- ============================================================================
local KXE, KYE = 0, 0

local function setup_proj()
    local kx = 0.711105 * (2 ^ M.z)
    local ky = kx / math.cos(M.clat / 1e6 * math.pi / 180)
    KXE = kx / 1e6
    KYE = ky / 1e6
    M.mpp = 111320.0 * math.cos(M.clat / 1e6 * math.pi / 180) / ky
    M.hw = (W / 2) / KXE
    M.hh = ((MAP_B - MAP_T + 1) / 2) / KYE
end

local function sx_of(lon_e6) return MX + (lon_e6 - M.clon) * KXE end
local function sy_of(lat_e6) return MY - (lat_e6 - M.clat) * KYE end

-- ============================================================================
-- 图层渲染
-- ============================================================================
local MINSTEP = 3

-- ★ 一次调用画整条折线 (固件新图元 app.poly): 把抽稀后的屏幕点**打包成 i2 点串**,
--   由 C 侧跑 Bresenham + 行程合并 + 裁剪 —— 每条折线只剩 **1 次跨界调用**。
--   实测(z14): 原来 6221 次 hline/vline 画 17465 像素 (平均 2.8 px/次, run 太碎),
--   现在每条路 1 次调用, Bresenham 在 C 里跑。
-- ⚠ 用**真值判断**而不是 type(...)=="function": 模拟器 lupa 里桥函数在打二进制补丁前
--   是 userdata(可调用), type() 会是 "userdata" 而不是 "function" ⇒ 判成假。
--   老固件没有这个 API 时 app.poly == nil, 同样正确地退化为 Lua 逐段画, 不会报错。
local HAS_POLY = (app.poly ~= nil)
-- ⚠ 坐标用 **i4** 而不是 i2: 一条长路的远端可以离屏几千像素 (0.15° 的文件在 z18 就有 ~14000px),
--   再大一点就撑爆 i2 的 ±32767 (实测报 "integer overflow")。夹住会扭曲线段, 不能夹。
local PFMT = {}                       -- 预生成 pack 格式串 (别在热路径里 string.rep)
do
    local f = "<"
    for k = 1, 64 do
        f = f .. "i4"
        if k % 2 == 0 then PFMT[k] = f end
    end
end

local function poly_draw(pts, np, w)
    if np < 4 then return end                       -- 至少 2 个点才成线
    local parts = {}
    for i = 1, np, 64 do
        local e = i + 63
        if e > np then e = np end
        parts[#parts + 1] = string.pack(PFMT[e - i + 1], table.unpack(pts, i, e))
    end
    app.poly(table.concat(parts), w, MAP_T, MAP_B)
end

local function draw_feature(d, poff, npts, w)
    local raw, base = d.raw, d.off_pt + poff
    local lat, lon = string.unpack(HEAD_FMT, raw, base + 1)
    lat, lon = lat + d.minlat, lon + d.minlon     -- 点表是相对文件 min 的, 必须加回
    local ax, ay = sx_of(lon), sy_of(lat)
    local px, py = ax, ay
    local tot, bx, by, bd = 0, ax, ay, (ax - MX) ^ 2 + (ay - MY) ^ 2
    local pts, np = nil, 0
    if HAS_POLY then
        pts = { math.floor(ax + 0.5), math.floor(ay + 0.5) }
        np = 2
    end
    local k = 1
    while k < npts do
        local rem = npts - k
        local n = rem >= 8 and 8 or (rem >= 4 and 4 or (rem >= 2 and 2 or 1))
        local fmt = n == 8 and B8 or (n == 4 and B4 or (n == 2 and B2 or B1))
        local t = { string.unpack(fmt, raw, base + 8 + (k - 1) * 4 + 1) }
        for j = 1, n do
            lat = lat + t[(j - 1) * 2 + 1]
            lon = lon + t[(j - 1) * 2 + 2]
            local qx, qy = sx_of(lon), sy_of(lat)
            tot = tot + math.abs(qx - ax) + math.abs(qy - ay)
            if qx >= 0 and qx < W and qy >= MAP_T and qy <= MAP_B then
                local dd = (qx - MX) ^ 2 + (qy - MY) ^ 2
                if dd < bd then bd, bx, by = dd, qx, qy end
            end
            if math.abs(qx - px) + math.abs(qy - py) >= MINSTEP then
                if HAS_POLY then
                    pts[np + 1] = math.floor(qx + 0.5)
                    pts[np + 2] = math.floor(qy + 0.5)
                    np = np + 2
                else
                    seg_w(math.floor(px + 0.5), math.floor(py + 0.5),
                          math.floor(qx + 0.5), math.floor(qy + 0.5), w)
                end
                px, py = qx, qy
            end
            ax, ay = qx, qy
        end
        k = k + n
    end
    if px ~= ax or py ~= ay then
        if HAS_POLY then
            pts[np + 1] = math.floor(ax + 0.5)
            pts[np + 2] = math.floor(ay + 0.5)
            np = np + 2
        else
            seg_w(math.floor(px + 0.5), math.floor(py + 0.5),
                  math.floor(ax + 0.5), math.floor(ay + 0.5), w)
        end
    end
    if HAS_POLY then poly_draw(pts, np, w) end
    return tot, bx, by
end


-- ============================================================================
-- v3 粗网格空间索引 —— "别线性扫"
-- ============================================================================
-- 为什么（2026-10-05 实测）: 没有索引时 draw_layer 必须**逐个 unpack** 当前 LOD 的
-- 每个要素表项再判 bbox。z16 实测每帧扫 **9307** 条只画 **31** 条 (300:1)，
-- 光这一项就白烧 5~9 ms (Lua 里一次 string.unpack ≈ 0.5~1 µs)。
-- 主流离线格式都有这一层: MBTiles 瓦片金字塔 / PMTiles 目录 / Mapsforge 的
-- per-zoom index blocks / Organic Maps 的 bucket 网格 / OsmAnd 的 tile 索引。
--
-- 索引段布局（小端，紧跟字符串表之后；头部 56..59 = off_idx）:
--   u8 iver | u8 pad | u16 nx | u16 ny      ← 6 B  (off 0..5)
--   i4 ox, oy, cw, ch                        ← 16 B (off 6..21)
--   u32 nsid | u32 gcount                    ← 8 B  (off 22..29)
--   u32 cell_start[nx*ny+1]  前缀和: 第 c 格的条目区间 [c], [c+1]  (off 30)
--   u16 fid[nsid]            要素下标 (**只登记在 bbox 的最小格里**, 每格内升序)
--   u16 gid[gcount]          "跨格太多"的长要素 (溶解后的整条国道等), 每帧都扫
-- ⚠ Lua 串是 1 基 ⇒ 0 基偏移 o 处的字段要写 `string.unpack(fmt, raw, o+k+1)`
--
-- ★ 为什么"只存最小格": 若每格都登记, 查询时同一要素会被取到多次, 还得在 Lua 里
--   维护一张 nfeat 大小的去重表 (几千要素 = 几百 KB)。改成只进最小格 + 查询时把
--   格区间**向外扩 max_span-1=2 格**, 数学上等价且天然不重复。索引也因此更小
--   (每要素只占 1 条 u16)。
local function ds_index(d)
    if _G.LTM_NOIDX then return nil end        -- 等价性自检用: 强制退回线性扫
    if d.idx_ready then return d.idx end
    d.idx_ready = true
    local raw, o = d.raw, d.off_idx or 0
    if o <= 0 or o + 30 > #raw then return nil end
    local _, _, nx, ny = string.unpack("<BBHH", raw, o + 1)
    local ox, oy, cw, ch = string.unpack("<iiii", raw, o + 7)
    local nsid, gcount = string.unpack("<II", raw, o + 23)
    if nx < 1 or ny < 1 or cw <= 0 or ch <= 0 or nx > 128 or ny > 128 then return nil end
    local cs = o + 31                                  -- cell_start 起点 (Lua 1 基)
    d.idx = { nx = nx, ny = ny, ox = ox, oy = oy, cw = cw, ch = ch,
              nsid = nsid, gcount = gcount, cs = cs,
              fid = cs + 4 * (nx * ny + 1), gid = cs + 4 * (nx * ny + 1) + 2 * nsid }
    return d.idx
end

-- 画一条要素 (先判 bbox); want_labels 时把它记进 named 返回
local function layer_item(d, i, w, vx0, vx1, vy0, vy1, want_labels, min_len, named)
    local raw = d.raw
    local cls, npts, noff, poff, bx0, by0, bx1, by1 =
        string.unpack(FEAT_FMT, raw, d.off_feat + i * FEAT_LEN + 1)
    if d.minlon + bx1 * 10 < vx0 or d.minlon + bx0 * 10 > vx1
       or d.minlat + by1 * 10 < vy0 or d.minlat + by0 * 10 > vy1 then
        return named
    end
    local tot, ex, ey = draw_feature(d, poff, npts, w)
    if want_labels and noff ~= 0xFFFF and tot >= min_len then
        named = named or {}
        named[#named + 1] = { noff = noff, cls = cls, tot = tot, x = ex, y = ey }
    end
    return named
end

local function draw_layer(d, want_labels, min_len)
    if not d then return nil end
    local L = pick_lod(d, M.z)
    local vx0, vx1 = M.clon - M.hw, M.clon + M.hw
    local vy0, vy1 = M.clat - M.hh, M.clat + M.hh
    local named = nil
    local ix = ds_index(d)
    if ix then
        -- 查询格区间: 要素只登记在最小格 ⇒ 外扩 max_span-1 = 2 格
        local raw = d.raw
        local nx, ny, cw, ch = ix.nx, ix.ny, ix.cw, ix.ch
        local cx0 = clampi(math.floor((vx0 - ix.ox) / cw) - 2, 0, nx - 1)
        local cx1 = clampi(math.floor((vx1 - ix.ox) / cw), 0, nx - 1)
        local cy0 = clampi(math.floor((vy0 - ix.oy) / ch) - 2, 0, ny - 1)
        local cy1 = clampi(math.floor((vy1 - ix.oy) / ch), 0, ny - 1)
        local f0, f1, w = L.f0, L.f1, L.w
        for cy = cy0, cy1 do
            for cx = cx0, cx1 do
                local c = cy * nx + cx
                local a = string.unpack("<I", raw, ix.cs + c * 4)
                local b = string.unpack("<I", raw, ix.cs + (c + 1) * 4)
                for k = a, b - 1 do
                    local fi = string.unpack("<H", raw, ix.fid + k * 2)
                    if fi >= f0 and fi < f1 then
                        named = layer_item(d, fi, w, vx0, vx1, vy0, vy1,
                                           want_labels, min_len, named)
                    end
                end
            end
        end
        for k = 0, ix.gcount - 1 do
            local fi = string.unpack("<H", raw, ix.gid + k * 2)
            if fi >= f0 and fi < f1 then
                named = layer_item(d, fi, w, vx0, vx1, vy0, vy1,
                                   want_labels, min_len, named)
            end
        end
        return named
    end
    -- 老文件 / 文件太大没做索引 → 退回线性扫
    local raw = d.raw
    for i = L.f0, L.f1 - 1 do
        named = layer_item(d, i, L.w, vx0, vx1, vy0, vy1,
                           want_labels, min_len, named)
    end
    return named
end

-- ============================================================================
-- 标注层: **路名 / 地标 / 收藏 / 导航目标 统一收集, 统一避让**
-- ============================================================================
-- 为什么必须统一: 分开放会互相压 (地标名盖住路名、目标名盖住地标)。prio 越小越先摆。
local LBL = {}

-- small=true 用 12px 汉字字模 (app.zhsm): 地标名用小的、路名用 16px 的, 拉开层次
local function add_label(x, y, txt, prio, small)
    LBL[#LBL + 1] = { x = x, y = y, s = txt, p = prio, sm = small }
end

-- 地标标记: 5×5 实心菱形 (和"线"一眼区分开)
local function mark_poi(x, y)
    app.px(x, y - 2, BK); app.px(x, y + 2, BK)
    app.px(x - 1, y - 1, BK); app.px(x + 1, y - 1, BK)
    app.px(x - 1, y + 1, BK); app.px(x + 1, y + 1, BK)
    app.px(x - 2, y, BK); app.px(x + 2, y, BK)
    app.px(x - 1, y, BK); app.px(x + 1, y, BK); app.px(x, y, BK)
    app.px(x, y - 1, BK); app.px(x, y + 1, BK)
end

-- 收藏标记: 5×5 空心方块
local function mark_fav(x, y)
    app.hline(x - 2, x + 2, y - 2, BK); app.hline(x - 2, x + 2, y + 2, BK)
    app.vline(x - 2, y - 2, y + 2, BK); app.vline(x + 2, y - 2, y + 2, BK)
    app.px(x, y, BK)
end

-- 目标标记: 7×7 实心圆 + 外圈 (最醒目)
local function mark_target(x, y)
    ring(x, y, 6)
    app.fill(x - 2, y - 2, x + 2, y + 2, BK)
end

local function mark(x, y, kind)
    if x < 4 or x > W - 5 or y < MAP_T + 4 or y > MAP_B - 4 then return false end
    if kind == "poi" then mark_poi(x, y)
    elseif kind == "fav" then mark_fav(x, y)
    else mark_target(x, y) end
    return true
end

-- 把路名也塞进统一标注表 (同名去重 + 只留可见总长最长的)
-- ⚠ 分块包下 named 里混着**多个数据集**的要素 (各自的字符串表/偏移互不相干),
--   所以必须**先解出名字再用名字去重** —— 直接拿 noff 当键会把"甲文件偏移 12 的名字"
--   和"乙文件偏移 12 的名字"错当成同一条路。
local function collect_road_labels(d, named)
    if not named then return end
    local best = {}
    for i = 1, #named do
        local it = named[i]
        local dd = it.d or d
        if dd then
            local e = dd.raw:find("\0", dd.off_str + it.noff + 1, true)
            local nm = e and dd.raw:sub(dd.off_str + it.noff + 1, e - 1)
            if nm and #nm > 0 then
                local b = best[nm]
                if b == nil or it.tot > b.tot then
                    best[nm] = { tot = it.tot, x = it.x, y = it.y, cls = it.cls }
                end
            end
        end
    end
    local names = {}
    for nm, it in pairs(best) do names[#names + 1] = nm end
    table.sort(names)                          -- ★ 固定顺序: 否则"画出来一样、标注不一样"
    for i = 1, #names do
        local nm, it = names[i], best[names[i]]
        add_label(it.x, it.y, nm, 10 + it.cls)
    end
end

-- 当前该拿哪些数据集画路 (z 够大用块; 否则概览/老单文件)
local function road_sets()
    local r = {}
    if M.pkg and M.pkg.tiled and M.z >= (M.pkg.ztile or TILE_Z_DEF) and M.tiles then
        for _, e in pairs(M.tiles) do
            if e.road then r[#r + 1] = e.road end
        end
    end
    if #r == 0 then
        if M.ov then r[1] = M.ov
        elseif M.road then r[1] = M.road end
    end
    return r
end

-- 统一摆名字: 优先级高的先摆, 摆不下的丢 (白底黑字压在图上)
local function place_labels(maxn)
    if #LBL == 0 then return 0 end
    -- ⛔ `table.sort` **不是稳定排序**。只按 prio 比会把"同优先级"的先后交给实现决定,
    --   于是"要素遍历顺序变了(比如加了空间索引) → 摆出来的标注也变" —— 看起来像
    --   渲染 bug, 其实只是并列名次没定序。这里补上 (y, x) 做**全序**键。
    table.sort(LBL, function(a, b)
        if a.p ~= b.p then return a.p < b.p end
        if a.y ~= b.y then return a.y < b.y end
        if a.x ~= b.x then return a.x < b.x end
        return (a.s or "") < (b.s or "")
    end)
    local used, n = {}, 0
    for i = 1, #LBL do
        if n >= maxn then break end
        local it = LBL[i]
        -- ⛔ 量宽度要与**绘制用同一套字面**: 12px 汉字用 zw12, 16px 混排用 app.mixw
        local tw = it.sm and zw12(it.s) or app.mixw(it.s)
        local th = it.sm and 12 or 16
        local tx = math.floor(it.x + 0.5)
        local ty = math.floor(it.y - th / 2 + 0.5)
        -- 地图页要让开左右两列浮动按键: 名字被按钮压住 = 等于没标
        local over_btn = false
        if M.page == 1 then
            local function hit(bx0, by0, bx1, by1)
                return tx < bx1 and tx + tw > bx0 and ty < by1 and ty + th > by0
            end
            if hit(LB_X - 2, LB_TOP - 3, LB_X + LB_W + 2, lb_y(LB_N) + LB_H + 2) then over_btn = true end
            if hit(RB_X - 2, RB_TOP - 3, W - 1, rb_y(RB_N) + RB_H + 2) then over_btn = true end
            if hit(BX_X, BX_Y, BX_X + BX_W, BX_Y + BX_H) then over_btn = true end   -- 开始键
        end
        if not over_btn and tx >= 2 and tx + tw <= W - 2
           and ty >= MAP_T + 2 and ty + th <= MAP_B - 1 then
            local clash = false
            for j = 1, #used do
                local u = used[j]
                if math.abs(tx - u[1]) < (tw + u[3]) / 2 + 8
                   and math.abs(ty - u[2]) < th then clash = true; break end
            end
            if not clash then
                used[#used + 1] = { tx, ty, tw }
                n = n + 1
                app.fill(tx - 1, ty - 1, tx + tw, ty + th, WH)        -- 白底
                if it.sm then app.zhsm(tx, ty, it.s, false)           -- 汉字专用字模
                else app.mix(tx, ty, it.s, false) end
            end
        end
    end
    return n
end

-- 一个地标数据集: 按 LOD 只画当前档, 标记 + 收集名字
-- ★ 分块包的地标是**跟着块**走的 (每个 0.15° 块十几~几十个), 不是全省一个文件:
--   全省 10064 个中文名 ⇒ 字符串表 120 KB, 而 nameoff 只有 u16 (0xFFFF) ⇒ 打包直接
--   抛 "字符串表超 64KB"; 而且每帧遍历 1 万个要素也太慢。分块后两件事一起解决。
local function draw_poi_ds(d)
    if not d or M.z < 11 then return 0 end     -- 缩略视图太挤, 不标
    local L = pick_lod(d, M.z)
    local raw = d.raw
    local vx0, vx1 = M.clon - M.hw, M.clon + M.hw
    local vy0, vy1 = M.clat - M.hh, M.clat + M.hh
    local n = 0
    for i = L.f0, L.f1 - 1 do
        local cls, npts, noff, poff, bx0, by0 =
            string.unpack(FEAT_HDR_F, raw, d.off_feat + i * FEAT_LEN + 1)
        -- 地标是**单点**要素: 它自己的 bbox 就是那个点 (bx0==bx1, by0==by1)
        local lon, lat = d.minlon + bx0 * 10, d.minlat + by0 * 10
        if not (lon < vx0 or lon > vx1 or lat < vy0 or lat > vy1) then
            local x = math.floor(sx_of(lon) + 0.5)
            local y = math.floor(sy_of(lat) + 0.5)
            if mark(x, y, "poi") then
                n = n + 1
                if noff ~= 0xFFFF and M.labels then
                    local e = raw:find("\0", d.off_str + noff + 1, true)
                    local nm = e and raw:sub(d.off_str + noff + 1, e - 1)
                    if nm then add_label(x + 5, y, nm, 2 + cls, true) end
                end
            end
        end
    end
    return n
end

local function draw_poi()
    if not M.poi_on or M.z < 11 then return end
    local n = 0
    if M.pkg and M.pkg.tiled and M.z >= (M.pkg.ztile or TILE_Z_DEF) then
        for _, e in pairs(M.tiles) do
            if e.poi then n = n + draw_poi_ds(e.poi) end
        end
    end
    if M.poi then n = n + draw_poi_ds(M.poi) end      -- 老单文件包
    M.poi_n = n
end

-- 收藏的点 (本地数据, 不占地图文件)
local function draw_favs()
    for i = 1, #F.list do
        local f = F.list[i]
        local x = math.floor(sx_of(f.lon) + 0.5)
        local y = math.floor(sy_of(f.lat) + 0.5)
        if mark(x, y, "fav") and M.labels then add_label(x + 5, y, f.name, 1, false) end
    end
end

-- 导航目标点: 最醒目 (标记 + 名字 + 距离)
local function draw_target()
    local nv = M.nav
    if not nv then return end
    local x = math.floor(sx_of(nv.lon) + 0.5)
    local y = math.floor(sy_of(nv.lat) + 0.5)
    if mark(x, y, "nav") and M.labels then
        local txt = nv.name
        if M.fix and M.fix.valid then
            local dm = dist_m(M.fix.lat_e6, M.fix.lon_e6, nv.lat, nv.lon)
            txt = txt .. " " .. fmt_dist(dm * 10, M.unit == 1)
        end
        add_label(x + 8, y - 8, txt, 0, false)
    end
end

local function nearest_road(lat_e6, lon_e6)
    local sets = road_sets()
    if #sets == 0 then return nil end
    local best, bestname = nil, nil
    local cl = math.cos((lat_e6 / 1000000) * math.pi / 180)
    for s = 1, #sets do
        local d = sets[s]
        local raw = d.raw
        for i = 0, d.nfeat - 1 do
            local cls, npts, noff, poff = string.unpack(FEAT_HDR, raw, d.off_feat + i * FEAT_LEN + 1)
            if noff ~= 0xFFFF and npts >= 2 then
                local base = d.off_pt + poff
                local la, lo = string.unpack(HEAD_FMT, raw, base + 1)
                la, lo = la + d.minlat, lo + d.minlon
                local k = 1
                while k < npts do
                    local rem = npts - k
                    local n = rem >= 8 and 8 or (rem >= 4 and 4 or (rem >= 2 and 2 or 1))
                    local fmt = n == 8 and B8 or (n == 4 and B4 or (n == 2 and B2 or B1))
                    local t = { string.unpack(fmt, raw, base + 8 + (k - 1) * 4 + 1) }
                    for j = 1, n do
                        la = la + t[(j - 1) * 2 + 1]
                        lo = lo + t[(j - 1) * 2 + 2]
                        local dy = (la - lat_e6) * 0.111320
                        local dx = (lo - lon_e6) * 0.111320 * cl
                        local dd = dx * dx + dy * dy
                        if best == nil or dd < best then best, bestname = dd, { d = d, noff = noff } end
                    end
                    k = k + n
                end
            end
        end
    end
    if not best or best > 160000 or not bestname then return nil end   -- 400 m 内才算
    local d = bestname.d
    local e = d.raw:find("\0", d.off_str + bestname.noff + 1, true)
    local nm = e and d.raw:sub(d.off_str + bestname.noff + 1, e - 1)
    if not nm or #nm == 0 then return nil end
    return nm
end

-- ============================================================================
-- 指标条 / 按钮 / 抽屉
-- ============================================================================
local function draw_metric_bar()
    app.fill(0, MB_Y0, W - 1, MB_Y1 - 1, WH)     -- 状态栏只显示状态: **不画边框/分隔线**

    local fix = M.fix
    local hh, mm = nil, nil
    if app.sys and app.sys.clock then hh, mm = app.sys.clock() end
    local wall = T.wall
    if not wall and hh then wall = pad(hh, 2) .. ":" .. pad(mm, 2) end

    local cells = {
        { "开始时间", wall or "--:--" },
        { "移动距离", fmt_dist(T.dist_dm, M.unit == 1) },
        { "爬升高度", "+" .. tostring(T.asc_m) .. "m" },
        { "当前高度", (fix and fix.valid and (tostring(fix.alt_m or 0) .. "m")) or "--" },
        { "当前配速", fmt_pace(fix and fix.mps100, M.unit == 1) },
    }
    for i = 1, 5 do
        local x = (i - 1) * CELL_W
        -- 标签(12px/字) 与 数值(8px/字) 各自在 70px 格内**水平居中**
        app.zhsm(x + (CELL_W - zw12(cells[i][1])) / 2, LBL_Y, cells[i][1], false)
        local v = cells[i][2]
        app.textsb(x + (CELL_W - #v * 8) / 2, VAL_Y, v, false)
        if i > 1 then app.vline(x - 3, MB_Y0 + 5, MB_Y1 - 7, BK) end
    end

    -- 卫星: 超迷你图标 + 数量 (状态栏右侧)
    local ok = fix and fix.valid
    local sats = (fix and fix.sats) or 0
    local s = tostring(sats)
    if #s < 2 then s = " " .. s end
    app.textsb(W - 4 - app.textw(s), VAL_Y, s, false)
    sat_icon(W - 4 - app.textw(s) - 16, VAL_Y + 2, ok and true or false)
end

local R_BTN = 4                                  -- 按键圆角半径
local function btn_frame(x, y, w, h, on)
    local x1, y1 = x + w - 1, y + h - 1
    if on then
        rrect(x, y, x1, y1, R_BTN, BK)                        -- 实心
    else
        rrect(x, y, x1, y1, R_BTN, BK)                        -- 外框
        rrect(x + 1, y + 1, x1 - 1, y1 - 1, R_BTN - 1, WH)    -- 挖白 → 1px 圆角边
    end
end

local function draw_buttons()
    local con = M.cont_on and M.contour ~= nil
    -- 右侧 4 键: 放大 / 缩小 / 等高 / 收藏 (整组垂直居中, 与左列同一中线)
    app.fill(RB_X - 2, RB_TOP - 3, W - 1, rb_y(RB_N) + RB_H + 2, WH)
    for i = 1, RB_N do
        local on = false
        if i == 4 then on = M.dr_on end
        btn_frame(RB_X, rb_y(i), RB_W, RB_H, on)
        local lab = RB_LABEL[i]
        if #lab == 1 then                                          -- "+" / "-": 16px 字, +4
            app.textsb(RB_X + RB_W / 2 - 4, rb_y(i) + 4, lab, on)   -- 8x16 字在 24px 键里: +4
        else
            app.zhsm(RB_X + (RB_W - zw12(lab)) / 2, rb_y(i) + 6, lab, on)
        end
    end

    -- 左侧 5 键: 地图 / 行程 / 卫星 / 设置 / 收藏夹 (整组垂直居中)
    app.fill(LB_X - 2, LB_TOP - 3, LB_X + LB_W + 2, lb_y(LB_N) + LB_H + 2, WH)
    for i = 1, LB_N do
        local on = (M.page == i)
        btn_frame(LB_X, lb_y(i), LB_W, LB_H, on)
        local lab = LB_LABEL[i]
        if i == 1 and M.cont_on and M.contour then lab = "等高" end     -- 第 1 键=地图模式
        app.zhsm(LB_X + (LB_W - zw12(lab)) / 2, lb_y(i) + 6, lab, on)
    end

    local lbl = "开始"
    if T.started then lbl = T.on and "暂停" or "继续" end
    btn_frame(BX_X, BX_Y, BX_W, BX_H, T.on)
    app.zhsm(BX_X + (BX_W - zw12(lbl)) / 2, BX_Y + (BX_H - 12) / 2, lbl, T.on)

    if M.nav and M.fix and M.fix.valid then                 -- 导航目标提示条
        local dm = dist_m(M.fix.lat_e6, M.fix.lon_e6, M.nav.lat, M.nav.lon)
        local txt = "距 " .. M.nav.name .. " " .. fmt_dist(dm * 10, M.unit == 1)
        local tw = app.mixw(txt)
        app.fill(2, MAP_B - 22, tw + 10, MAP_B - 3, WH)
        app.rect(2, MAP_B - 22, tw + 10, MAP_B - 3, BK)
        app.mix(7, MAP_B - 20, txt, false)
    end
end

local function draw_scalebar()
    local mpp = M.mpp > 0 and M.mpp or 1
    local best, bw = SCALES[1], SCALES[1] / mpp
    for i = 1, #SCALES do
        local px = SCALES[i] / mpp
        if px >= 34 and px <= 110 then best, bw = SCALES[i], px; break end
        if px < 34 then best, bw = SCALES[i], px end
    end
    bw = math.floor(bw + 0.5)
    if bw < 10 then bw = 10 end
    if bw > 130 then bw = 130 end
    local x1 = RB_X - 5
    local x0 = x1 - bw
    local y = MAP_B - 5
    app.fill(x0 - 2, y - 13, x1 + 1, y + 2, WH)
    app.hline(x0, x1, y, BK)
    app.vline(x0, y - 3, y, BK)
    app.vline(x1, y - 3, y, BK)
    local s = best >= 1000 and string.format("%gkm", best / 1000) or (best .. "m")
    app.texts(math.floor((x0 + x1) / 2 - app.textw(s) / 2 + 0.5), y - 12, s, false)
    app.texts(4, y - 12, "z" .. M.z, false)
end

local function draw_gps_dot()
    local f = M.fix
    if not f or not f.valid then return end
    local x = math.floor(sx_of(f.lon_e6) + 0.5)
    local y = math.floor(sy_of(f.lat_e6) + 0.5)
    if x < 3 or x > W - 4 or y < MAP_T + 3 or y > MAP_B - 3 then return end
    ring(x, y, 5)
    app.vline(x, y - 8, y + 8, BK)
    app.hline(x - 8, x + 8, y, BK)
    app.fill(x - 1, y - 1, x + 1, y + 1, BK)
end

local function drawer_rows() return 1 + #F.list end       -- 收藏当前位置 + 已收藏
local function drawer_last() return drawer_rows() + 1 end  -- 末行: 下载地图包
local function dr_row_y(i) return DR_Y0 + 36 + (i - 1) * DR_ROW_H end

local function draw_drawer()
    local x0 = M.dr_x
    if x0 >= W then return end
    local x1 = W - 1
    app.fill(x0, DR_Y0, x1, MAP_B, WH)
    app.vline(x0, DR_Y0, MAP_B, BK)
    app.hline(x0, x1, DR_Y0, BK)

    app.mix(x0 + 8, DR_Y0 + 8, "收藏夹", false)
    app.hline(x0 + 4, x1 - 4, DR_Y0 + 30, BK)

    local n = drawer_rows()
    for i = 1, n do
        local y = dr_row_y(i)
        local sel = (M.dr_sel == i)
        if sel then app.fill(x0 + 4, y, x1 - 4, y + DR_ROW_H - 3, BK) end
        if i == 1 then app.mix(x0 + 10, y + 5, "收藏当前位置", sel)
        else app.mix(x0 + 10, y + 5, F.list[i - 1].name, sel) end
    end
    local y = dr_row_y(n + 1)
    app.hline(x0 + 4, x1 - 4, y - 2, BK)
    if M.dr_sel == n + 1 then app.fill(x0 + 4, y, x1 - 4, y + DR_ROW_H - 3, BK) end
    app.mix(x0 + 10, y + 5, "下载地图包", M.dr_sel == n + 1)
    app.mix(x0 + 8, MAP_B - 17, "长按项 = 删除", false)   -- 汉字必须走 mix, textsb 是纯 ASCII
end


-- ============================================================================
-- GPX 轨迹落盘 (参考 IceNav-v3 的 lib/gpx: 用标准 GPX 1.1, 别的软件能直接导入)
-- ============================================================================
-- ⛔ 不要用 string.format("%.6f", e6/1e6): LUA_32BITS 下 float 只有 ~7 位有效数字,
--   25.4335 这种值会掉到 0.1 m 级误差。直接从**整数微度**拼小数点才是精确的。
local function deg(e6)
    local sign = ""
    if e6 < 0 then sign, e6 = "-", -e6 end
    return sign .. (e6 // 1000000) .. "." .. pad(e6 % 1000000, 6)
end

local function gpx_save()
    if T.tr_n < 2 then
        app.dlg.toast("没有轨迹可保存", 1200)
        return false
    end
    local f = M.fix
    local yy, mo, dd = 2026, 1, 1
    if f and f.has_date then yy, mo, dd = f.yy or yy, f.mo or mo, f.dd or dd end
    local hh, mm = app.sys.clock()
    local stamp = string.format("%04d%02d%02d_%02d%02d", yy, mo, dd, hh, mm)
    local o = {}
    local function w(x) o[#o + 1] = x end
    w('<?xml version="1.0" encoding="UTF-8"?>')
    w('<gpx version="1.1" creator="LinTOS map.lua" xmlns="http://www.topografix.com/GPX/1/1">')
    w(' <trk><name>LinTOS ' .. stamp .. '</name>')
    w('  <trkseg>')
    local n = T.tr_n < TRACK_MAX and T.tr_n or TRACK_MAX
    for i = 1, n do
        local pt = T.track[i]
        if pt then
            w(string.format('   <trkpt lat="%s" lon="%s"><ele>%d</ele></trkpt>',
                            deg(pt[1]), deg(pt[2]), pt[3] or 0))
        end
    end
    w('  </trkseg>')
    w(' </trk></gpx>')
    local body = table.concat(o, "\n") .. "\n"
    for i = 1, #MAP_DIRS do
        if app.fs.exists(MAP_DIRS[i]) then
            local path = MAP_DIRS[i] .. "/track_" .. stamp .. ".gpx"
            if app.fs.write(path, body) then
                app.dlg.toast("已存 " .. stamp .. ".gpx", 2000)   -- 提示框宽度有限, 别把整名塞进去
                return true
            end
        end
    end
    app.dlg.toast("写入 TF 卡失败", 1400)
    return false
end

-- ============================================================================
-- 收藏夹 (NVS 单键, 行格式: lat_e6,lon_e6,name)
-- ============================================================================
local FAV_KEY = "fav1"

local function fav_load()
    F.list = {}
    local s = app.nvs and app.nvs.get_str and app.nvs.get_str(FAV_KEY)
    if s and #s > 0 then
        for line in s:gmatch("[^\n]+") do
            local la, lo, nm = line:match("^(%-?%d+),(%-?%d+),(.*)$")
            if la then
                F.list[#F.list + 1] = { lat = tonumber(la), lon = tonumber(lo), name = nm }
            end
        end
    end
end

local function fav_save()
    local out = {}
    for i = 1, #F.list do
        local f = F.list[i]
        out[#out + 1] = f.lat .. "," .. f.lon .. "," .. f.name
    end
    if app.nvs and app.nvs.set_str then app.nvs.set_str(FAV_KEY, table.concat(out, "\n")) end
end

local function fav_add_here()
    local fix = M.fix
    if not fix or not fix.valid then app.dlg.toast("还没定位, 无法收藏", 1200); return end
    if #F.list >= 20 then app.dlg.toast("收藏夹已满 (20)", 1200); return end
    local nm = nearest_road(fix.lat_e6, fix.lon_e6) or ("点" .. (#F.list + 1))
    F.list[#F.list + 1] = { lat = fix.lat_e6, lon = fix.lon_e6, name = nm }
    fav_save()
    app.dlg.toast("已收藏: " .. nm, 1200)
end

-- ============================================================================
-- 地图包联网下载 —— **多源 + 自动回退 + 换源记忆**
-- ============================================================================
-- ★ 源清单来自**固件**: `app.net.mirrors()`（os_sources.c，固件内唯一真相源，
--   应用商店 am_store_net.c 用的就是同一份）。所以"加源 / 换源 / 调顺序"只改固件一处，
--   商店与地图应用自动同步 —— 正是"核心能力进固件、脚本只调用"那条原则。
--   拿不到（老固件）就退回内置兜底，绝不因为桥缺一个 API 就整个下不了。
local BOOT_MIRRORS = { "https://bbk.linit.cn/bbk" }
local MIRROR_KEY = "mirror"            -- NVS: 上次成功的源下标 (1 起)

local function mir_label(u)
    local h = tostring(u):match("^https?://([^/]+)") or tostring(u)
    if h:find("gitee") then return "Gitee" end
    if h:find("github") or h:find("jsdelivr") or h:find("ghproxy") then return "GitHub" end
    return "主站"
end

-- 源清单 = 索引里带的 mirrors[]（最新）＋ 固件内置清单, 去重
local function mirrors_list()
    if M.mirrors then return M.mirrors end
    local t = {}
    local function add(u)
        if type(u) ~= "string" or #u == 0 then return end
        for i = 1, #t do if t[i] == u then return end end
        t[#t + 1] = u
    end
    if M.idx_mirrors then for i = 1, #M.idx_mirrors do add(M.idx_mirrors[i]) end end
    if app.net and app.net.mirrors then
        local m = app.net.mirrors()
        for i = 1, 8 do if m and m[i] then add(m[i]) else break end end
    end
    if #t == 0 then for i = 1, #BOOT_MIRRORS do add(BOOT_MIRRORS[i]) end end
    M.mirrors = t
    return t
end

-- ⛔ 必须**前置声明**: dl_after_index / dl_poll 都要调它, 而它的定义在它们之后。
--   Lua 的 local 只对声明之后可见 —— 不前置的话那两个函数看到的是**全局**同名变量
--   (= nil), 一点"联网下载"就崩 "attempt to call a nil value (global 'dl_next_file')"。
--   这条下载分支以前从没被走到过, 所以这个空指向一直没暴露。
local dl_next_file

-- 当前步的源试完 → 换下一个; 全试完才认输
local function dl_retry_or_fail(what)
    D.left = (D.left or 0) - 1
    if D.left <= 0 then
        D.st = "idle"
        app.dlg.toast(what .. ": 所有下载源都不可用", 2000)
        return false
    end
    D.mi = D.mi % #D.mirrors + 1
    return true
end

local function dl_start_index()
    D.st, D.left = "index", #D.mirrors
    if not app.net.fetch_start(D.mirrors[D.mi] .. "/maps.json", 12000) then
        if dl_retry_or_fail("索引") then dl_start_index() end
    end
end

-- 落地目录: 优先 /sdcard/Map
local function map_dir()
    for i = 1, #MAP_DIRS do
        if app.fs.exists(MAP_DIRS[i]) then return MAP_DIRS[i] end
    end
    return nil
end

-- ★ 两条下载路径, 按大小自动分派
--   小文件 (索引/单文件包, < 900 KB): 整包收进内存 → 能同时核**字节数 + Fletcher-16**,
--     而且 LTM1/{ 首字符检查挡住"站点 301 到 HTML 首页"那类假成功。
--   大文件 (省级 .bin, 18 MB): `fetch_start(url, ms, path)` **边收边写 TF** ——
--     内存里只有一个 8 KB 搬运缓冲。此时没法算全文件校验和 (也不该把 18 MB 收进内存),
--     所以靠**固件侧的 Content-Length 比对** + 落盘字节数比对, 短包一定被拒。
local DL_STREAM_MIN = 900 * 1024

local function dl_start_file()
    D.cur = D.files[D.fi]
    D.st, D.left = "dl", #D.mirrors
    local dir = map_dir()
    if not dir then D.st = "idle"; app.dlg.toast("找不到 Map 目录", 1400); return end
    local base = tostring(D.cur.path):match("([^/]+)$") or tostring(D.cur.path)
    local size = tonumber(tostring(D.cur.size)) or -1
    D.dst = dir .. "/" .. base
    D.stream = (size >= DL_STREAM_MIN)
    D.got = 0
    local url = D.mirrors[D.mi] .. "/" .. D.cur.path
    local ok
    if D.stream then ok = app.net.fetch_start(url, 900000, D.dst)
    else              ok = app.net.fetch_start(url, 30000) end
    if not ok then
        if dl_retry_or_fail("下载") then dl_start_file() end
    end
end

local function dl_start()
    if M.err_kind == "notf" then app.dlg.toast("请先插 TF 卡", 1200); return end
    if not (app.net and app.net.fetch_start) then
        app.dlg.toast("本机不支持联网下载", 1400); return
    end
    if not app.net.is_connected() then
        app.dlg.toast("未联网, 请先连 WiFi", 1400); return
    end
    local ms = mirrors_list()
    if #ms == 0 then app.dlg.toast("没有可用的下载源", 1400); return end
    -- ⛔ D.mirrors 必须在这里赋值: dl_start_index / dl_start_file / dl_retry_or_fail
    --   全都在用它, 但老代码只把清单存进了 M.mirrors → D.mirrors 一直是 nil ⇒
    --   一点"联网下载"就崩 "attempt to get length of a nil value"。
    --   这条分支以前从没被真正走到过 (有图时不会进这个页面), 所以一直没暴露。
    D.mirrors = ms
    D.mi = clampi(M.mi_pref or 1, 1, #ms)
    D.fi, D.files, D.n = 0, nil, 0
    D.idx = 0
    dl_start_index()
end

local function dl_after_index(body)
    local rows = app.json.pick(body, "files", "path,size,f16,t", 40)
    local n = 0
    for i = 1, 30 do if rows[i] then n = i else break end end
    if n == 0 then
        D.st = "idle"
        app.dlg.toast("没有可下载的地图包", 1400)
        return
    end
    -- 索引里的 mirrors[] 是最新的一份: 记下来, 下次就不用先试死链了
    local mm = app.json.pick(body, "mirrors", "u", 8)
    local got = {}
    for i = 1, 8 do if mm[i] and mm[i].u then got[#got + 1] = mm[i].u else break end end
    if #got > 0 then M.idx_mirrors, M.mirrors = got, nil end
    M.mi_pref = D.mi
    if app.nvs and app.nvs.set_int then app.nvs.set_int(MIRROR_KEY, D.mi) end
    D.files, D.n, D.fi = rows, n, 0
    dl_next_file()
end

function dl_next_file()
    D.fi = D.fi + 1
    if not D.files or D.fi > D.n then
        D.st = "idle"
        D.done = true          -- 真正的重载放到 poll 里做 (那时 map_reload 已定义)
        return
    end
    dl_start_file()
end

local function dl_poll()
    if D.st == "idle" then return end
    local a, body, code, n = app.net.fetch_poll()
    if a == "wait" then
        if D.stream and app.net.fetch_got then D.got = app.net.fetch_got() end
        return
    end

    -- ---- 失败 → 换下一个源重试**当前这一步** ----
    -- ⚠ 判断顺序: 流式成功时 body 是 **nil**, 所以必须先看 a, 不能先看 body。
    --   写成 `if a~="ok" or not body` 会把"18MB 下完了"当成失败, 无限重试。
    local step_err = (a ~= "ok")
    if not step_err and D.st ~= "index" and not D.stream then
        step_err = (not body) or (#body == 0)
    end
    if step_err then
        if dl_retry_or_fail(D.st == "index" and "索引" or "下载") then
            if D.st == "index" then dl_start_index() else dl_start_file() end
        end
        return
    end

    if D.st == "index" then
        -- 站点对**不存在的路径**会 301 → WordPress 首页 HTML; 设备端跟随重定向,
        -- 于是"拿到 HTML 当 JSON 解"。首字符不是 '{' 就当这个源没这文件, 换源。
        if not body or body:sub(1, 1) ~= "{" then
            if dl_retry_or_fail("索引") then dl_start_index() end
            return
        end
        dl_after_index(body)
        return
    end

    local f = D.cur
    local w_sz = tonumber(tostring(f.size)) or -1
    if D.stream then
        -- 已落到 D.dst; 只核对字节数 (Content-Length 那一层固件已挡过)
        if w_sz >= 0 and n ~= w_sz then
            app.fs.remove(D.dst)
            if dl_retry_or_fail("校验") then dl_start_file() end
            return
        end
    else
        if not body or #body < 5 or (body:sub(1, 4) ~= "LTM1" and body:sub(1, 1) ~= "{") then
            if dl_retry_or_fail("下载") then dl_start_file() end
            return
        end
        local w_f = tonumber(tostring(f.f16)) or -1
        if (w_sz >= 0 and #body ~= w_sz) or (w_f >= 0 and w_f ~= fletcher16(body)) then
            if dl_retry_or_fail("校验") then dl_start_file() end
            return
        end
        local ok = app.fs.write(D.dst, body)
        if not ok then
            D.st = "idle"
            app.dlg.toast("写 TF 卡失败", 1400)
            return
        end
    end
    dl_next_file()
end


-- ============================================================================
-- 页面: 地图 / 行程 / 卫星 / 设置  (参考 IceNav-v3 的多屏结构, 只保留本地图用得上的)
-- ============================================================================
local PX = 56                                    -- 页面内容左边界: 让开左侧按键列

local function fmt_hms(ms)
    local s = (ms or 0) // 1000
    return pad(s // 3600, 2) .. ":" .. pad((s // 60) % 60, 2) .. ":" .. pad(s % 60, 2)
end

local function fmt_kmh(mps100, mi)
    local v = (mps100 or 0) / 100 * 3.6
    if mi then v = v * 0.621371 end
    return string.format("%.1f", v)
end

-- ---- 行程页 ----
local function page_trip()
    local st = "未开始"
    if T.started then st = T.on and "记录中" or "已暂停" end
    app.mix(PX, MAP_T + 6, "本次记录", false)
    local sw = app.mixw(st)
    app.mix(W - 8 - sw, MAP_T + 6, st, false)
    app.hline(4, W - 5, MAP_T + 26, BK)

    local dn = dist_num(T.dist_dm, M.unit == 1)
    app.big(PX, MAP_T + 34, dn, 3, false)
    app.textsb(PX + #dn * 24 + 6, MAP_T + 62, M.unit == 1 and "mi" or "km", false)

    local y = MAP_T + 86
    local function row(lbl, val)
        app.zhsm(PX, y + 2, lbl, false)
        app.textsb(PX + 84, y, val, false)
        y = y + 20
    end
    row("用时", fmt_hms(T.t_ms))
    row("爬升", "+" .. T.asc_m .. "m")
    row("均速", fmt_kmh((T.t_ms > 1000) and ((T.dist_dm * 36) // (T.t_ms // 1000)) or 0, M.unit == 1)
                .. (M.unit == 1 and "mph" or "km/h"))
    row("最高", fmt_kmh(T.max_mps100, M.unit == 1) .. (M.unit == 1 and "mph" or "km/h"))

    -- 轨迹面包屑
    local bx0, by0, bx1, by1 = PX - 4, y + 6, W - 9, MAP_B - 22
    app.rect(bx0, by0, bx1, by1, BK)
    local n = T.tr_n < TRACK_MAX and T.tr_n or TRACK_MAX
    if n >= 2 then
        local la0, la1, lo0, lo1
        for i = 1, n do
            local pt = T.track[i]
            if pt then
                if not la0 or pt[1] < la0 then la0 = pt[1] end
                if not la1 or pt[1] > la1 then la1 = pt[1] end
                if not lo0 or pt[2] < lo0 then lo0 = pt[2] end
                if not lo1 or pt[2] > lo1 then lo1 = pt[2] end
            end
        end
        local dla = (la1 - la0) > 0 and (la1 - la0) or 1
        local dlo = (lo1 - lo0) > 0 and (lo1 - lo0) or 1
        local iw, ih = (bx1 - bx0) - 8, (by1 - by0) - 8
        local px, py
        for i = 1, n do
            local pt = T.track[i]
            if pt then
                local x = bx0 + 4 + ((pt[2] - lo0) * iw) // dlo
                local yy = by1 - 4 - ((pt[1] - la0) * ih) // dla
                if px then
                    local steps = math.max(math.abs(x - px), math.abs(yy - py))
                    steps = steps > 0 and steps or 1
                    for k = 1, steps do
                        app.px(px + (x - px) * k // steps, py + (yy - py) * k // steps, BK)
                    end
                else
                    app.px(x, yy, BK)
                end
                px, py = x, yy
            end
        end
    else
        app.mixc(W / 2, (by0 + by1) // 2 - 8, "等待轨迹", false)
    end
    app.mixc(W / 2, MAP_B - 17, "点数 " .. T.tr_n .. "  (每 12m 打一点)", false)
end

-- ---- 卫星 / GPS 信息页 (上机排查就看这页) ----
-- ⚠ 值里有汉字 ("有效"/"颗") ⇒ **必须用 app.mix**; app.textsb 是纯 ASCII 字体, 汉字变 '?'。
local function page_sat()
    local fix = M.fix
    app.mix(PX, MAP_T + 6, "GPS 卫星", false)
    app.hline(4, W - 5, MAP_T + 26, BK)
    if not fix then
        app.mixc(W / 2, MAP_T + 90, "无 GPS 数据", false)
        return
    end
    local y = MAP_T + 34
    local function row(lbl, val)
        app.mix(PX, y, lbl, false)
        app.mix(PX + 108, y, val, false)
        y = y + 20
    end
    row("定位", (fix.valid and "有效" or "无效"))
    row("卫星", tostring(fix.sats or 0) .. " 颗")
    row("质量", tostring(fix.quality or 0) .. " / " .. tostring(fix.dim or 0) .. "D")
    row("HDOP", string.format("%.1f", (fix.hdop100 or 0) / 100))
    row("海拔", tostring(fix.alt_m or 0) .. " m")
    row("航向", string.format("%.1f", (fix.crs10 or 0) / 10) .. " 度")
    row("坐标", deg(fix.lat_e6) .. " " .. deg(fix.lon_e6))
    row("句子", tostring(fix.sentences or 0) .. " 校验错 " .. tostring(fix.badcs or 0))
    app.mixc(W / 2, MAP_B - 16, "句子=0 查线/波特率, 校验错多=波特率不对", false)
end

-- ---- 设置页 ----
-- 当前包摘要 (设置页与"未找到地图"页共用)
local function pkg_summary()
    local p = M.pkg
    if not p then return nil end
    if not p.tiled then
        local d = M.road
        return d and string.format("%s 单文件 %d 条 %.0fKB", p.name, d.nfeat, d.bytes / 1024)
                    or (p.name .. " (无数据)")
    end
    local kb = (p.bytes or 0) / 1024
    local s = string.format("%s 分块 %d 块", p.name, p.count or 0)
    if kb > 1024 then s = s .. string.format(" %.1fMB", kb / 1024)
    else s = s .. string.format(" %.0fKB", kb) end
    return s
end

local SET_ROWS = {
    { "单位", function() M.unit = (M.unit == 1) and 0 or 1
                 if app.nvs and app.nvs.set_int then app.nvs.set_int("unit", M.unit) end end,
      function() return M.unit == 1 and "英里" or "公里" end },
    -- 地图包: 装了多个就轮流切 (坐标不在当前包范围内时也可以手动换)
    -- ⚠ 这里**不能**直接调 map_reload —— 它在文件后段定义, 这里引用会拿到 nil。
    --   记一个意图, 由末尾的 poll() 执行 (那时所有 local 都已绑定)。
    { "地图包", function()
          local ps = M.pkgs or {}
          if #ps < 2 then app.dlg.toast("只装了一个地图包", 1200); return end
          M.pkgi = (M.pkgi % #ps) + 1
          M.pkg_switch = ps[M.pkgi].id
      end,
      function()
          local ps = M.pkgs or {}
          return (M.pkg and M.pkg.name or "无") .. " (" .. #ps .. ")"
      end },
    { "路名标注", function() M.labels = not M.labels end,
      function() return M.labels and "开" or "关" end },
    { "收藏夹", function() F.list = {}; fav_save() end,
      function() return tostring(#F.list) .. " 项 (点按清空)" end },
    { "保存 GPX 轨迹", function() gpx_save() end,
      function() return T.tr_n .. " 点" end },
    { "地标显示", function() M.poi_on = not M.poi_on end,
      function() return M.poi_on and "开" or "关" end },
    -- 下载源: 点一下换成下一个 (顺序就是尝试顺序); 固件里的清单有几条就能切几条
    { "下载源", function()
          local ms = mirrors_list()
          if #ms < 1 then return end
          M.mi_pref = ((M.mi_pref or 1) % #ms) + 1
          if app.nvs and app.nvs.set_int then app.nvs.set_int(MIRROR_KEY, M.mi_pref) end
          app.dlg.toast("下载源: " .. mir_label(ms[M.mi_pref]), 1200)
      end,
      function()
          local ms = mirrors_list()
          local i = clampi(M.mi_pref or 1, 1, #ms)
          return (ms[i] and mir_label(ms[i]) or "?") .. " (" .. i .. "/" .. #ms .. ")"
      end },
}
-- 7 行设置 + 3 行包信息, 一屏放得下。
-- ⚠ 行高从 26 收到 22: 7 行时 y 到 223, 信息区最后一行的底边 285 < MAP_B(299) ✓。
--   行高 26 时信息区会掉到 313, **跑出屏幕** (设置页就看不见包信息了)。
local SET_H = 22

local function set_row_y(i) return MAP_T + 34 + (i - 1) * SET_H end

local function page_set()
    app.mix(PX, MAP_T + 6, "设置", false)
    app.hline(4, W - 5, MAP_T + 26, BK)
    for i = 1, #SET_ROWS do
        local y = set_row_y(i)
        app.mix(PX, y + 3, SET_ROWS[i][1], false)
        local v = SET_ROWS[i][3]()
        local vw = app.mixw(v)
        app.mix(W - 14 - vw, y + 3, v, false)
    end
    -- 地图包信息 (压在设置行下面, 这段只留 3 行, 不能超 MAP_B)
    local y = set_row_y(#SET_ROWS + 1)
    app.hline(PX, W - 5, y - 2, BK)
    local s = pkg_summary()
    if not s then
        app.mix(PX, y + 4, "未加载地图包 (只从 TF 卡读)", false)
        return
    end
    app.mix(PX, y + 2, s, false)
    if M.pkg and M.pkg.tiled then
        local n = 0
        for _ in pairs(M.tiles or {}) do n = n + 1 end
        app.mix(PX, y + 18, string.format("视口 %d 块 / 已载 %d 块 %.0fKB",
                 M.tneed or 0, n, (M.tbytes or 0) / 1024), false)
        local z = M.z
        app.mix(PX, y + 34, string.format("z%d %s  /sdcard/Map/", z,
                 z >= (M.pkg.ztile or TILE_Z_DEF) and "分块" or "概览"), false)
    else
        local d = M.road
        app.mix(PX, y + 18, d and ("道路 " .. d.nfeat .. " 条 / " .. d.npt .. " 点")
                           or "无数据", false)
        local l3 = d and string.format("%.0fKB", d.bytes / 1024) or "-"
        if M.contour then l3 = l3 .. string.format("  等高 %d 条", M.contour.nfeat) end
        app.mix(PX, y + 34, l3 .. "  /sdcard/Map/", false)
    end
end

-- ============================================================================
-- render
-- ============================================================================
function render()
    app.clear()
    if M.err_kind then
        app.mixc(W / 2, 92, M.err_kind == "notf" and "未检测到 TF 卡" or "未找到地图数据", false)
        app.mixc(W / 2, 120, M.err, false)
        app.mixc(W / 2, 148, "把地图包放进 TF 卡的 Map 文件夹", false)
        app.texts(W / 2 - 88, 168, "/sdcard/Map/fj.json + fj.bin", false)
        -- ⛔ 这一行有汉字 ⇒ 必须用 app.mixc (16px 混排), 不能用 app.texts
        --   (ASCII 字体画汉字是一串 '?', lua_check 的契约闸门会拦)
        app.mixc(W / 2, 184, "或 putian_road.ltm (单文件包)", false)
        if M.err_kind == "nofile" then
            local busy = D.st ~= "idle"
            btn_frame(BX_X, 212, BX_W, BX_H, busy)
            app.zhsm(BX_X + (BX_W - zw12("联网下载")) / 2, 212 + (BX_H - 12) / 2, "联网下载", busy)
        end
        app.mixc(W / 2, 258, "或从应用商店的外网托管站下载", false)
        return
    end

    setup_proj()
    -- ★ 2026-10-09: 行程/卫星/设置 已改弹窗, 渲染路径只剩地图 (M.page 恒为 1)。
    --   (保留 page_* 函数本体不删 —— 弹窗的取值逻辑仍复用它们的数据源。)
    if false then page_trip()
    else
    if M.cont_on and M.contour then draw_layer(M.contour, false, 1e9) end
    -- ★ 先让当前视口的块就位, 再画 —— 顺序不能反 (否则第一帧永远是空的)
    tile_sync()
    local named = {}
    if M.road_on then
        local minlen = M.z <= 12 and 30 or 56
        local sets = road_sets()
        for s = 1, #sets do
            local d = sets[s]
            local nm = draw_layer(d, M.labels, minlen)
            if nm then
                for i = 1, #nm do
                    nm[i].d = d                 -- 标注要回自己那个数据集取名字
                    named[#named + 1] = nm[i]
                end
            end
        end
    end

    -- 导航连线 (目标点本身由 draw_target 统一画, 这里只画线)
    if M.nav and M.fix and M.fix.valid then
        seg_w(math.floor(sx_of(M.fix.lon_e6) + 0.5), math.floor(sy_of(M.fix.lat_e6) + 0.5),
              math.floor(sx_of(M.nav.lon) + 0.5), math.floor(sy_of(M.nav.lat) + 0.5), 1)
    end

    -- ★ 三种"点"标记 + 统一摆名字: 地标 / 收藏 / 导航目标 / 路名 一起避让
    LBL = {}
    draw_poi()
    draw_favs()
    draw_target()
    draw_gps_dot()
    if M.labels then
        collect_road_labels(nil, named)
        M.lbl_n = place_labels(M.z <= 11 and 10 or (M.z <= 13 and 16 or 22))
    end
    draw_scalebar()
    draw_buttons()
    if M.dr_on or M.dr_x < W then draw_drawer() end
    end

    -- ★ 2026-10-09 (linit 定案): **地图就是地图** —— 配速/当前高度/移动距离这条常驻
    --   指标条已从地图页摘掉, 这些指标归「码表」应用 (apps/lua/apps/app/bikecomp.lua)。
    --   draw_metric_bar() 函数体保留: 行程/卫星弹窗与轨迹记录仍在用它取同一份数据。
    --   想临时看指标 → 点左列「行程」弹窗, 不占地图画面。
    if D.st ~= "idle" then
        local msg = "获取地图包索引..."
        if D.st == "dl" then
            if D.stream then
                -- 省级包是 18 MB 的单文件, 必须让用户看到"在动" ——
                -- 否则 90 秒不动的界面会被当成死机。
                local mb = tonumber(tostring(D.cur and D.cur.size)) or 0
                local got = D.got or 0
                msg = string.format("下载 %.1f/%.1fMB", got / 1048576, mb / 1048576)
                local pc = (mb > 0) and clampi(got * 100 // mb, 0, 100) or 0
                msg = msg .. string.format("  %d%%", pc)
            else
                msg = string.format("下载中 %d/%d", D.fi, D.n)
            end
        end
        local tw = app.mixw(msg)
        app.fill(MX - tw / 2 - 8, MY - 16, MX + tw / 2 + 8, MY + 4, WH)
        app.rect(MX - tw / 2 - 8, MY - 16, MX + tw / 2 + 8, MY + 4, BK)
        app.mixc(MX, MY - 12, msg, false)
    end
end

-- ============================================================================
-- 行程累计
-- ============================================================================
local function push_track(lat, lon, alt)
    T.tr_n = T.tr_n + 1
    local i = ((T.tr_n - 1) % TRACK_MAX) + 1
    T.track[i] = { lat, lon, alt or 0 }
end

local function trip_step(fix, dt_ms)
    if not T.on or not fix or not fix.valid then return end
    local lat, lon = fix.lat_e6, fix.lon_e6
    local moved = false
    if T.p_lat then
        local dm = dist_m(T.p_lat, T.p_lon, lat, lon)
        if dm <= 200 then
            T.dist_dm = T.dist_dm + dm * 10                     -- >200m 当跳点
            if dm >= 1 then moved = true end
        end
    end
    T.p_lat, T.p_lon = lat, lon
    if moved then T.t_ms = T.t_ms + dt_ms end
    local sp = fix.mps100 or 0
    if sp > T.max_mps100 then T.max_mps100 = sp end
    -- 轨迹打点: 必须与**上一个轨迹点**比距离 (与上一次 poll 比的话单帧位移太小, 永远打不上点)
    if T.tr_lat then
        local dp = dist_m(T.tr_lat, T.tr_lon, lat, lon)
        if dp >= 12 and dp <= 200 then push_track(lat, lon, fix.alt_m); T.tr_lat, T.tr_lon = lat, lon end
    else
        push_track(lat, lon, fix.alt_m); T.tr_lat, T.tr_lon = lat, lon
    end

    -- 爬升: **迟滞法**。GPS/气压高度噪声 ±2~3m, 逐点累加会虚高一大截。
    local alt = fix.alt_m or 0
    if T.base_alt == nil then T.base_alt = alt
    elseif alt > T.base_alt + 3 then
        T.asc_m = T.asc_m + (alt - T.base_alt)
        T.base_alt = alt
    elseif alt < T.base_alt then T.base_alt = alt end
end

-- ============================================================================
-- 交互
-- ============================================================================
-- 包的可用范围: 概览/单文件的 bbox。**clamp_view 必须用包的范围** ——
-- 用 M.road (老字段) 的话分块包下范围是 nil, 平移就会跑出省界。
local function view_bounds()
    local p = M.pkg
    if p and p.tiled and (p.maxlon > p.minlon) then
        return p.minlon * 1e6, p.minlat * 1e6, p.maxlon * 1e6, p.maxlat * 1e6
    end
    local d = M.ov or M.road or M.contour
    if not d then return nil end
    local minlon, maxlon = d.minlon, d.maxlon
    local minlat, maxlat = d.minlat, d.maxlat
    if M.contour and M.contour ~= d then
        if M.contour.minlon < minlon then minlon = M.contour.minlon end
        if M.contour.maxlon > maxlon then maxlon = M.contour.maxlon end
        if M.contour.minlat < minlat then minlat = M.contour.minlat end
        if M.contour.maxlat > maxlat then maxlat = M.contour.maxlat end
    end
    return minlon, minlat, maxlon, maxlat
end

local function clamp_view()
    local minlon, minlat, maxlon, maxlat = view_bounds()
    if not minlon then return end
    local lox, hix = minlon + M.hw, maxlon - M.hw
    if lox > hix then M.clon = math.floor((minlon + maxlon) / 2 + 0.5)
    else M.clon = math.floor((M.clon < lox and lox or (M.clon > hix and hix or M.clon)) + 0.5) end
    local loy, hiy = minlat + M.hh, maxlat - M.hh
    if loy > hiy then M.clat = math.floor((minlat + maxlat) / 2 + 0.5)
    else M.clat = math.floor((M.clat < loy and loy or (M.clat > hiy and hiy or M.clat)) + 0.5) end
end

-- ============================================================================
-- 选包 / 重载 (必须在 view_bounds + setup_proj + clamp_view **之后**定义:
--   Lua 的 local 只对声明之后可见 —— 放在前面会拿到 nil, 直接崩)
-- ============================================================================
-- 选包优先级: NVS 记住的 id > **包含当前坐标的** > 第一个
local function pkg_pick_best(lat_e6, lon_e6, want)
    local ps = M.pkgs
    if not ps or #ps == 0 then return nil end
    if want then
        for i = 1, #ps do if ps[i].id == want then return ps[i] end end
    end
    if lat_e6 and lon_e6 then
        for i = 1, #ps do
            local p = ps[i]
            if p.maxlon > p.minlon
               and lon_e6 >= p.minlon * 1e6 and lon_e6 <= p.maxlon * 1e6
               and lat_e6 >= p.minlat * 1e6 and lat_e6 <= p.maxlat * 1e6 then
                return p
            end
        end
    end
    return ps[1]
end

-- 重扫 TF 卡 → 载入选中的包 → (可选)把视口对准包范围
local function map_reload(keepview)
    M.pkgs = scan_pkgs()
    local lat, lon
    if keepview then lat, lon = M.clat, M.clon
    elseif M.fix and M.fix.valid then lat, lon = M.fix.lat_e6, M.fix.lon_e6 end
    local want = app.nvs and app.nvs.get_str and app.nvs.get_str("mappkg") or nil
    local p = pkg_pick_best(lat, lon, want)
    M.tiles, M.torder, M.tbytes, M.tmiss = {}, {}, 0, 0
    if not p then
        M.pkg, M.ov, M.road, M.contour, M.poi, M.map_dir = nil, nil, nil, nil, nil, nil
        return false
    end
    pkg_load(p)
    for i = 1, #(M.pkgs or {}) do if M.pkgs[i] == p then M.pkgi = i end end
    -- ⚠ 必须清掉错误态: 原来是 on_enter 里清的, 而"下载完成 → 重载"这条路径
    --   不经过 on_enter ⇒ 包已经就绪了界面还卡在"未找到地图数据"。
    M.err_kind, M.err = nil, nil
    local a, b, c, e = view_bounds()
    if a and not keepview then
        M.clon = math.floor((a + c) / 2 + 0.5)
        M.clat = math.floor((b + e) / 2 + 0.5)
    end
    setup_proj()
    clamp_view()
    return true
end

local function zoom_by(d)
    local z = clampi(M.z + d, ZMIN, ZMAX)
    if z == M.z then return end
    M.z = z
    setup_proj()
    clamp_view()
    app.redraw()
end

local function recenter()
    local f = M.fix
    if f and f.valid then
        M.clon, M.clat, M.z = f.lon_e6, f.lat_e6, 16
        setup_proj()
    else
        app.dlg.toast((M.fix and (M.fix.sentences or 0) > 0) and "正在搜索卫星" or "GPS 无数据", 1100)
    end
end

local function open_drawer(v)
    M.dr_on = v
    if v then M.dr_sel = 1 end
    app.redraw()
end

local function hit_rect(x, y, rx, ry, rw, rh)
    return x >= rx and x <= rx + rw - 1 and y >= ry and y <= ry + rh - 1
end

local function start_nav_to(i)
    local fv = F.list[i]
    if not fv then return end
    M.pending_nav = fv
    M.asking = "nav"
    app.dlg.confirm("开始导航到 " .. fv.name .. "?")
end

local function drawer_press(ty)
    local n = drawer_rows()
    for i = 1, n do
        if ty >= dr_row_y(i) and ty < dr_row_y(i) + DR_ROW_H then
            if i == 1 then fav_add_here() else start_nav_to(i - 1) end
            app.redraw()
            return
        end
    end
    local y = dr_row_y(n + 1)
    if ty >= y and ty < y + DR_ROW_H then dl_start(); app.redraw() end
end

local DRAG = { on = false, btn = nil, x0 = 0, y0 = 0, lon0 = 0, lat0 = 0,
               moved = false, t0 = 0, long = false }

-- ★ 2026-10-09: 行程 / 卫星 / 设置 从"整页"改成**弹窗** (linit 要求)。
--   起因: 点"卫星"切到 M.page=3 后**退不出来** —— action() 里 BACK 只处理了 M.page==4,
--   2/3 落到 `return false` 交给内核弹"退出程序?", 取消后仍停在卫星页。
--   改用 app.dlg.list (系统弹窗): 自带 返回/确定 关闭, 结构上不可能再卡住。
-- ★ 2026-10-09 (linit 要求): GPS **三态**显示 —— 还没收到星时显示"搜索中", 不再直接报"无效"。
--   区分依据: 「句子」(已解析的 NMEA 句数) > 0 ⇒ 链路(接线/波特率)正常, 只是还在等收星。
--   冷启动(backup 唤醒会丢星历)要 30 秒~数分钟, 室内可能很久 ⇒ 这个阶段显示"搜索中"才不误导。
--   只有句子 = 0(一个字节都没解析出来)才是真故障, 此时也应去查线/波特率。
local function gps_state_text()
    local f = M.fix
    if not f then return "无数据" end
    if f.valid then return "已定位" end
    if (f.sentences or 0) > 0 then return "搜索中" end
    return "无数据"
end

local function open_sat_dlg()
    local fix = M.fix
    if not fix then app.dlg.toast("正在搜索卫星, 请稍候", 1400); return end
    M.dlg_kind = nil
    app.dlg.list("GPS 卫星", {
        "定位   " .. gps_state_text(),
        "卫星   " .. tostring(fix.sats or 0) .. " 颗",
        "质量   " .. tostring(fix.quality or 0) .. " / " .. tostring(fix.dim or 0) .. "D",
        "HDOP   " .. string.format("%.1f", (fix.hdop100 or 0) / 100),
        "海拔   " .. tostring(fix.alt_m or 0) .. " m",
        "航向   " .. string.format("%.1f", (fix.crs10 or 0) / 10) .. " 度",
        "坐标   " .. deg(fix.lat_e6) .. " " .. deg(fix.lon_e6),
        "句子   " .. tostring(fix.sentences or 0) .. " 校验错 " .. tostring(fix.badcs or 0),
    }, 1)
end

local function open_trip_dlg()
    -- 字段名对齐真实状态表 T (见文件上方 `local T = {...}`) 与既有格式化函数
    local st = "未开始"
    if T.started then st = T.on and "记录中" or "已暂停" end
    local mi = (M.unit == 1)
    local avg100 = (T.t_ms > 1000) and ((T.dist_dm * 36) // (T.t_ms // 1000)) or 0
    M.dlg_kind = nil
    app.dlg.list("本次记录", {
        "状态   " .. st,
        "距离   " .. fmt_dist(T.dist_dm, mi) .. (mi and " mi" or " km"),
        "用时   " .. fmt_hms(T.t_ms),
        "爬升   +" .. tostring(T.asc_m or 0) .. "m",
        "均速   " .. fmt_kmh(avg100, mi) .. (mi and " mph" or " km/h"),
        "最高   " .. fmt_kmh(T.max_mps100 or 0, mi),
        "配速   " .. fmt_pace(avg100, mi),
    }, 1)
end

local function open_set_dlg()
    local it = {}
    for i = 1, #SET_ROWS do it[i] = SET_ROWS[i][1] .. "   " .. SET_ROWS[i][3]() end
    M.dlg_kind = "set"
    app.dlg.list("设置", it, 1)
end

-- 按下 → 抬起 时判定 (短按 = 触发, 长按 = 删除)
local function press_at(px, py, held)
    -- 左侧 5 键: 地图 / 行程 / 卫星 / 设置 / 收藏夹
    -- (必须在"设置页按行"之前判, 否则进了设置页就点不到"地图"出不来)
    for i = 1, LB_N do
        if hit_rect(px, py, LB_X, lb_y(i), LB_W, LB_H) then
            if i == 1 then
                M.cont_on = not M.cont_on            -- 地图键 → 切 普通/等高
                app.dlg.toast(M.cont_on and "等高地图" or "普通地图", 800)
            elseif i == 2 then open_trip_dlg()       -- ★ 2026-10-09: 行程/卫星/设置 一律弹窗
            elseif i == 3 then open_sat_dlg()
            elseif i == 4 then open_set_dlg()
            else open_drawer(true) end               -- 收藏夹
            app.redraw()
            return
        end
    end
    if M.page == 4 then                       -- 设置页: 按行
        for i = 1, #SET_ROWS do
            local y = set_row_y(i)
            if py >= y and py < y + SET_H then
                SET_ROWS[i][2]()
                app.redraw()
                return
            end
        end
        return
    end
    if M.page ~= 1 then return end            -- 行程/卫星页没有可点控件
    if M.dr_on then
        local n = drawer_rows()
        for i = 2, n do                             -- i=1 是"收藏当前位置", 长按不删
            if py >= dr_row_y(i) and py < dr_row_y(i) + DR_ROW_H then
                if held >= 700 then
                    local fv = F.list[i - 1]
                    if fv then
                        table.remove(F.list, i - 1)
                        fav_save()
                        M.dr_sel = clampi(M.dr_sel, 1, drawer_last())
                        app.dlg.toast("已删除收藏", 1000)
                    end
                    app.redraw()
                    return
                end
                break
            end
        end
        drawer_press(py)
        return
    end
    for i = 1, RB_N do
        if hit_rect(px, py, RB_X, rb_y(i), RB_W, RB_H) then
            if i == 1 then zoom_by(1)
            elseif i == 2 then zoom_by(-1)
            elseif i == 3 then recenter()          -- 回到当前位置 (原来触摸端没有这个入口)
            else open_drawer(true) end
            app.redraw()
            return
        end
    end
    if hit_rect(px, py, BX_X, BX_Y, BX_W, BX_H) then
        if held >= 800 and T.started then
            T.on, T.started, T.wall = false, false, nil
            gpx_save()                                   -- 结束即落盘 (参考 lib/gpx)
            T.dist_dm, T.asc_m, T.base_alt = 0, 0, nil
        elseif not T.started then
            T.started, T.on = true, true
            T.dist_dm, T.asc_m, T.base_alt = 0, 0, nil
            T.p_lat, T.p_lon = nil, nil
            local hh, mm = app.sys.clock()
            T.wall = pad(hh, 2) .. ":" .. pad(mm, 2)
            app.dlg.toast("开始记录", 900)
        else
            T.on = not T.on
        end
        app.redraw()
        return
    end
    if M.err_kind == "nofile" and hit_rect(px, py, BX_X, 212, BX_W, BX_H) then
        dl_start()
        app.redraw()
    end
end

local function widget_at(x, y)
    for i = 1, LB_N do                        -- 左侧 5 键在任何页面都算控件
        if hit_rect(x, y, LB_X, lb_y(i), LB_W, LB_H) then return true end
    end
    if M.page ~= 1 then return true end       -- 非地图页不吃拖动
    if M.dr_on then return true end
    for i = 1, RB_N do
        if hit_rect(x, y, RB_X, rb_y(i), RB_W, RB_H) then return true end
    end
    for i = 1, LB_N do
        if hit_rect(x, y, LB_X, lb_y(i), LB_W, LB_H) then return true end
    end
    if hit_rect(x, y, BX_X, BX_Y, BX_W, BX_H) then return true end
    if M.err_kind == "nofile" and hit_rect(x, y, BX_X, 212, BX_W, BX_H) then return true end
    return false
end

local function poll_touch()
    local tx, ty = app.touch()
    local now = app.millis() or 0
    if not tx then
        if DRAG.on and DRAG.btn then
            press_at(DRAG.x0, DRAG.y0, now - DRAG.t0)
        end
        DRAG.on, DRAG.btn, DRAG.moved = false, nil, false
        return
    end
    if not DRAG.on then
        DRAG.on, DRAG.moved = true, false
        DRAG.x0, DRAG.y0, DRAG.t0 = tx, ty, now
        DRAG.lon0, DRAG.lat0 = M.clon, M.clat
        DRAG.btn = widget_at(tx, ty) and true or nil
        return
    end
    if DRAG.btn or M.page ~= 1 then return end
    local dx, dy = tx - DRAG.x0, ty - DRAG.y0
    if not DRAG.moved and math.abs(dx) + math.abs(dy) < 4 then return end
    DRAG.moved = true
    M.clon = DRAG.lon0 - math.floor(dx / KXE + (dx >= 0 and 0.5 or -0.5))
    M.clat = DRAG.lat0 + math.floor(dy / KYE + (dy >= 0 and 0.5 or -0.5))
    clamp_view()
    app.redraw()
end

function poll()
    -- 弹窗优先: 结果一到就处理; 弹窗期间不碰地图输入 (固件里触摸被弹窗层吃掉)
    if M.asking then
        local r = app.dlg.result()
        if r ~= nil then
            local what, p = M.asking, M.pending_nav
            M.asking, M.pending_nav = nil, nil
            if what == "nav" and r == 0 and p then
                M.nav = { lat = p.lat, lon = p.lon, name = p.name }
                M.dr_on = false
                app.dlg.toast("开始导航: " .. p.name, 1200)
            end
            app.redraw()
        end
        return
    end

    local now = app.millis() or 0
    local dt = now - (M.last_ms or now)
    M.last_ms = now
    if dt < 0 or dt > 5000 then dt = 0 end
    if M.gps_ok then
        local f = app.gps.fix()
        if f then M.fix = f end
    end
    trip_step(M.fix, dt)

    -- 到达判定 (参考 IceNav navigation.hpp 的 isFinished): 30 m 内算到达
    if M.nav and M.fix and M.fix.valid then
        if dist_m(M.fix.lat_e6, M.fix.lon_e6, M.nav.lat, M.nav.lon) <= 30 then
            app.dlg.toast("已到达: " .. M.nav.name, 1800)
            M.nav = nil
            app.redraw()
        end
    end

    -- 设置页点了"地图包" → 在这里真正切换 (这里所有 local 都已绑定)
    if M.pkg_switch then
        local want = M.pkg_switch
        M.pkg_switch = nil
        if app.nvs and app.nvs.set_str then app.nvs.set_str("mappkg", want) end
        if map_reload(false) then
            for i = 1, #(M.pkgs or {}) do
                if M.pkgs[i] == M.pkg then M.pkgi = i end
            end
            app.dlg.toast("地图包: " .. (M.pkg and M.pkg.name or want), 1200)
            app.redraw()
        else
            app.dlg.toast("这个包装不起来", 1600)
        end
    end
    if D.st ~= "idle" then dl_poll() end
    -- 下载收尾: 重新扫包 + 载入 (放在这里是因为 map_reload 定义在文件后段,
    -- 而 dl_* 在前面 —— 直接调用会拿到 nil 的 upvalue)
    if D.done then
        D.done, D.files = nil, nil
        if map_reload(false) then
            app.dlg.toast("地图包已就绪", 1400)
        else
            app.dlg.toast("下载完成但没认出地图包", 1800)
        end
    end

    local want = M.dr_on and (W - DR_W) or W
    if M.dr_x ~= want then
        local step = 42
        if M.dr_x < want then M.dr_x = math.min(want, M.dr_x + step)
        else M.dr_x = math.max(want, M.dr_x - step) end
        app.redraw()
    end

    poll_touch()

    if M.key then
        local k = M.key
        M.key = nil
        setup_proj()
        if M.dr_on then
            if k == A_UP then M.dr_sel = clampi(M.dr_sel - 1, 1, drawer_last())
            elseif k == A_DOWN then M.dr_sel = clampi(M.dr_sel + 1, 1, drawer_last())
            elseif k == A_OK then
                if M.dr_sel == 1 then fav_add_here() else drawer_press(dr_row_y(M.dr_sel) + 2) end
            end
        else
            local dx, dy = M.hw * 0.30, M.hh * 0.30
            if k == A_LEFT then M.clon = M.clon - dx
            elseif k == A_RIGHT then M.clon = M.clon + dx
            elseif k == A_UP then M.clat = M.clat + dy
            elseif k == A_DOWN then M.clat = M.clat - dy end
            clamp_view()
        end
        app.redraw()
    end

    local sec = now // 1000
    if sec ~= M.last_sec then
        M.last_sec = sec
        if T.on or M.nav or M.fix then app.redraw() end
    end
end

function action(a)
    if a == A_BACK then
        if M.asking then return true end
        if M.dr_on then open_drawer(false); return true end
        -- ★ 2026-10-09: 任何非地图页都先回地图 (原来只处理了 M.page==4,
        --   点"卫星"后按返回落到下面 `return false` → 内核弹"退出程序?", 取消后仍出不去)
        if M.page ~= 1 then M.page = 1; app.redraw(); return true end
        return false                                    -- 地图页: 交内核弹「退出程序?」
    elseif a == A_LEFT or a == A_RIGHT then
        -- ★ 2026-10-09: 页签已改成弹窗, 左右键不再切页 (只吞掉, 不做事)
        return true
    elseif a == A_UP or a == A_DOWN then
        M.key = a                              -- 抽屉里=选行; 地图页=上下平移
        return true
    elseif a == A_OK then
        if M.dr_on then M.key = A_OK; return true end
        recenter()
        app.redraw()
        return true
    end
    return false
end

function on_enter()
    M.last_ms = app.millis() or 0
    fav_load()
    if app.nvs and app.nvs.get_int then
        M.unit = app.nvs.get_int("unit") or 0
        M.mi_pref = app.nvs.get_int("mirror") or 1      -- 上次成功的下载源
    end
    if not app.fs.exists(TIER0) then
        M.err_kind = "notf"
        M.err = "请插入 TF 卡后重新打开"
        return
    end
    if not map_reload(false) then
        M.err_kind = "nofile"
        M.err = app.fs.exists(MAP_DIRS[1]) and "Map 文件夹里没有地图包"
                                            or "TF 卡上没有 Map 文件夹"
        return
    end
    if app.gps and app.gps.start then M.gps_ok = app.gps.start() end
end

function on_exit()
    -- ★ 2026-10-09: 退出时把 GPS 停掉并送回待机。
    -- 否则: ① 解析任务常驻; ② 模块保持 ~10mA 连续跟踪 (待机只需 46µA) —— 直接吃电池。
    if app.gps and app.gps.stop then app.gps.stop() end
    M.road, M.contour, M.ov, M.poi = nil, nil, nil, nil
    M.tiles, M.torder = nil, nil
    M.tbytes = 0
    if D.st ~= "idle" and app.net and app.net.fetch_poll then app.net.fetch_poll() end
    D.st = "idle"
end
