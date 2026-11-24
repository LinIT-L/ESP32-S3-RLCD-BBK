--@lapp v1
--@app name=矢量地图
--@app cat=工具
--@app desc=离线矢量地图: 道路/等高线双图层, 任意缩放不糊, GPS 定位
--@app version=1
--@app build=202610300638
--@app icon=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP///+f///8B////AH///4OACAAYAAgBwwAIAP8ACADDAAgD/8AIAMMACAf/4AgAwwAID//wCADDAAgP//gIAMMACB/n+AgAwwAIH4H4CADDAAgfgfwIAMMACD8A/AgAwwAIPwD8CADDABgfgfwYAMP//5+B+P//wwAIH+f4CADDAAgP//gIAMMACA//8AgAwwAIB//gCADDAAgD/8AIAMMACAP/wAgAwwAIAf+ACADDAAgB/4AIAMMACAD/gAgAwwAIAP8ACADDAAgA/wAIAMMACAB/AAgAwwAIAH4ACADDABgAfgAYAMP/////////wwAIADwACADDAAgAPAAIAMMACAAYAAgAwwAIABgACADDAAgACAAIAMMACAAAAAgAwwAIAAAACADDgAgAAAAIAMH/////////gP////////8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
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
-- ★ 2026-10-27 linit 定案: 顶部指标条 = **每项"名字一行 + 数值在名字正下方", 全部固定 16px**。
--   5 格 × 80px = 400 正好满屏宽; 名字/数值各自在格内水平居中。
--   (12px+8px 那种"标签小字+数值小字"的做法作废。)
local MB_Y0, MB_Y1 = 0, 36
local LBL_Y, VAL_Y = 1, 19
local CELL_W       = 80              -- 5 格 × 80 = 400 (整屏)
-- 内容区: 上面让出指标条(0..36), 下面让出软键栏(275..299)
local MAP_T, MAP_B = 37, 273
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
local BX_X, BX_Y = (W - BX_W) / 2, 238        -- ★ 上移: 底部 275 起是软键栏

local DR_W      = 202                            -- 抽屉宽 (从右侧拉出)
local DR_ROW_H  = 26
local DR_Y0     = MAP_T

local ZMIN, ZMAX, ZDEF = 10, 18, 14

-- ---- 数据: 只认 TF 卡 ----
local TIER0    = "/sdcard"
local MAP_DIRS = { "/sdcard/Map", "/sdcard/maps" }
-- 老的单文件包: 没有清单, 靠固定文件名兜底识别
local LEGACY_ROAD    = "putian_road.ltm"
local LEGACY_CONTOUR = "putian_contour.ltm"
local LEGACY_POI     = "putian_poi.ltm"

-- ============================================================================
-- 两种地图包形态 (并存)
-- ============================================================================
-- ① 单文件包: <id>_road.ltm / _contour.ltm / _poi.ltm —— 无清单, 兜底识别
-- ② 分块包: <id>.json 索引 + <id>.bin 数据体, 块按 (off,size) 内嵌在 bin 里
--
-- ★★ 为什么省级必须是 "1 个 bin + 索引" 而不是 "一瓦片一个 .ltm"
--   ① 单文件装不下: 示例省 4.9°×4.9°, 而要素 bbox 是 i2(1e-5°) 时单文件跨度上限
--      只有 0.3277°; 就算 v2 把 bbox 放宽到 i4, 整省路网 18 MB 也远超
--      `app.fs.read` 的 512 KB 上限 —— 根本读不进来。
--   ② 一瓦片一文件 = 1171 个 HTTP 请求 (607 路块 + 564 标块), 设备端一个个下
--      二十分钟都下不完, 每个请求还要重来一遍 TLS 握手。
--   ③ 所以: 全部块**顺序平铺**进一个 .bin, 索引记每块 (off,size), 设备端用
--      `app.fs.read_at(bin, off, size)` 精确读当前视口那几块 —— 一次请求下完整省,
--      内存只驻留几块。见 docs/矢量离线地图-方案与实现.md「省级包」。
-- 从这一档起改用分块; 更小的 z 只看概览 (一屏上千平方公里, 逐块读反而更慢)。
-- 包里清单也带 ztile, 以包为准 —— 这里只是没有清单时的兜底值。
local TILE_Z_DEF = 11

-- 同时在内存里的块数 / 总字节上限。块内是 Lua 字符串 (PSRAM):
-- 示例省 610 块, 中位 13.8 KB, 最大 364 KB ⇒ 24 块典型 330 KB, 戴上限 3 MB 也安全。
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

-- ★ B9: 图层开关落盘 —— 单键位掩码, 与 on_enter 的解析严格一一对应。
--   bit0 道路 / bit1 等高线 / bit2 地标 / bit3 文字。默认 (无键) = 13 = 道路+地标+文字。
--   ⚠ 必须写成**全局**函数: 本文件顶层 local 已顶到 Lua 的 200 上限 (见 draw_softbar 上方注释)。
function lay_save()
    if not (app.nvs and app.nvs.set_int) then return end
    local v = 0
    if M.road_on then v = v + 1 end
    if M.cont_on then v = v + 2 end
    if M.poi_on  then v = v + 4 end
    if M.labels  then v = v + 8 end
    app.nvs.set_int("lay", v)
end

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
        -- 兜底: 老的单文件包, 没有清单。
        -- ⚠ 判重必须按"**同一个目录里已经有老式包**", 不能按"这个目录已经有包":
        --   实际布局下 putian_road.ltm 与 fj.json/fj.bin 都在 /sdcard/Map/,
        --   按目录判重会把老包整个吞掉。
        if app.fs.exists(dir .. "/" .. LEGACY_ROAD) then
            local dup = false
            for j = 1, #out do
                if out[j].json == nil and out[j].dir == dir then dup = true end
            end
            if not dup then
                out[#out + 1] = { id = "putian", name = "本地地图", dir = dir,
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
        -- ★ 名字一行、数值在名字正下方, 两者都 16px (app.mix = 汉字16宽/ASCII8宽)
        local lb = cells[i][1]
        app.mix(x + (CELL_W - app.mixw(lb)) / 2, LBL_Y, lb, false)
        local v = cells[i][2]
        app.mix(x + (CELL_W - app.mixw(v)) / 2, VAL_Y, v, false)
        if i > 1 then app.vline(x, MB_Y0 + 4, MB_Y1 - 5, BK) end
    end
    -- ⛔ 卫星图标/数量不再挤在这条里 (5 格已占满 400px) —— 地图页放到底部软键栏的凹口
end

-- ============================================================================
-- 底部软键栏 (盖子线 + 左右两根斜坡 + 谷线) —— ★ 2026-10-27 linit 定案:
--   **全应用所有页面都用同一条**, 左侧一枚软键 + 右侧一枚软键, 中间凹口放状态字。
--   几何照 sdrv_draw_softbar / planeradar: 盖子线 275, 谷线 296, 软键 16px@279。
-- ============================================================================
-- ⚠ 本文件顶层局部变量已经顶到 Lua 的 200 上限 ⇒ 这里**不能再新增 local**
--   (实测: 多加 2 个就 "too many local variables")。所以这个函数按全局定义,
--   与文件里既有的 `function dl_next_file()` 同一做法。
function draw_softbar(lbl_l, lbl_r, mid)
    local SOFT_LID, SOFT_VALLEY, SOFT_KEYY, SOFT_MARGIN = 275, 296, 279, 10
    app.fill(0, SOFT_LID - 1, W - 1, H - 1, WH)       -- 先清底, 免得旧地图线透上来
    local wl, wr = app.mixw(lbl_l), app.mixw(lbl_r)
    app.mix(SOFT_MARGIN, SOFT_KEYY, lbl_l, false)
    app.mix(W - SOFT_MARGIN - wr, SOFT_KEYY, lbl_r, false)
    local Lend   = SOFT_MARGIN + wl + 10              -- 左斜坡起点 = 软键右侧 10px
    local Rstart = W - SOFT_MARGIN - wr - 10          -- 右斜坡起点 = 软键左侧 10px
    app.hline(0, Lend, SOFT_LID, BK)                  -- 盖子线 (贴屏幕左右边框)
    app.hline(Rstart, W - 1, SOFT_LID, BK)
    local slope = SOFT_VALLEY - SOFT_LID
    for d = 0, slope do
        app.px(Lend + d, SOFT_LID + d, BK)            -- ↓ 左斜坡
        app.px(Rstart - d, SOFT_LID + d, BK)          -- ↓ 右斜坡 (双面)
    end
    if Lend + slope < Rstart - slope then
        app.hline(Lend + slope, Rstart - slope, SOFT_VALLEY, BK)   -- 谷线
    end
    if mid and mid ~= "" then
        local wm = app.mixw(mid)
        if wm <= (Rstart - 12) - (Lend + 12) then
            app.mix(math.floor((W - wm) / 2), SOFT_KEYY, mid, false)
        end
    end
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
        -- ★ 2026-10-27 linit: 第 1 键改成"地图/GPS"开关 ——
        --   在地图页显示 "GPS"(点它回地球/卫星数据页), 在别的页显示 "地图"(点它去当前位置的地图)。
        --   等高地图模式改成长按切换(不再占用第 1 键的名字)。
        if i == 1 then lab = (M.page == 1) and "GPS" or "地图" end
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

    -- ★ B3 精度圈: 半径 = HDOP × 5.0m (UERE 经验值, **不是实测标定**) / 米每像素。
    --   M.mpp 由 setup_proj 按当前 z 与纬度算好 ⇒ 缩放时圈自动变小/变大。
    --   HDOP 缺失(0)或离谱(>20)时不画, 免得一个巨圈盖住整个屏幕。
    local hd = f.hdop100 or 0
    if hd > 0 and hd <= 2000 then
        local mpp = M.mpp
        if not mpp or mpp <= 0 then mpp = 1 end
        local r = math.floor((hd / 100) * 5.0 / mpp + 0.5)
        if r < 8 then r = 8 elseif r > 55 then r = 55 end
        ring(x, y, r)
    end

    ring(x, y, 5)
    app.vline(x, y - 8, y + 8, BK)
    app.hline(x - 8, x + 8, y, BK)
    app.fill(x - 1, y - 1, x + 1, y + 1, BK)

    -- ★ B3 朝向箭头. ⛔ 不能用"航向是否有效"来判断: gps_fix_t 里 course_deg
    --   没有有效位, 解析失败时**保留上一次的值**(gps_nmea.c 只在 ok_cog 时更新),
    --   低速时那就是一条陈旧航向。所以用**速度门限**挡: <1.5m/s 一律不画。
    local sp = f.mps100 or 0
    if sp >= 150 then
        local a = (f.crs10 or 0) / 10 * math.pi / 180
        local sa, ca = math.sin(a), math.cos(a)
        local tx = x + math.floor(sa * 9 + 0.5)
        local ty = y - math.floor(ca * 9 + 0.5)
        seg_w(x, y, tx, ty, 1)                                  -- 主杆
        local w1, w2 = a + math.pi - 0.5236, a + math.pi + 0.5236 -- 两片尾翼 (后 ±30°)
        seg_w(tx, ty, tx + math.floor(math.sin(w1) * 5 + 0.5),
                      ty - math.floor(math.cos(w1) * 5 + 0.5), 1)
        seg_w(tx, ty, tx + math.floor(math.sin(w2) * 5 + 0.5),
                      ty - math.floor(math.cos(w2) * 5 + 0.5), 1)
    end
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

-- ---- 卫星页 v3: 3D 自转地球(海岸线+灰阶点阵) + 卫星随球面转 + 中文信息 (2026-10-24) ----
-- 数据源: Natural Earth 110m land → 海岸线 1506 点 + 陆地密度点阵(离海越远越密)。
-- 点阵按 stride 取 1/3 入表(≈3000 点), 控内存与帧耗时。
local _COAST_B64 =
    "BQDPwfLgD8Hl4A3ATOEDwWLhz8Hy4AcAXe6D4djuWOET773gSOxZ4NjqgeD+7IPhXe6D4QUAp9BK46DRTOMJ0Q/jGtA646fQSuMJAEPlSOQi5c/jPOSu4wXju+Oz4gLk2eMx5Prj2eSP5BjlQ+VI5HUAG+nx5sbnsOaH52zmvOcl5lnmdeXb52DkPeg242zkEeLU4QniOuOR4Y/hYuGG4RLhkOKm4K/o0t9C6X7fkOwT4Efv8N/Z9J7gZ/QK4RTy9uAG8mbhKPmn4t755uKR+SXj9/lt4/r7KeQa/f7jUf1L5On/BOQGA7PkOgRV5D4Fq+SVCnrkfwzK5DsNPuUZD77kTRVK5v8XdeUFGavl6Rp35Tcb9eR9Gonk+xpk5Isa7+NLG8bj2hy15FQe3uRWIL/l4yHF5V8iI+YHI8XlaiWt5fQmu+UrKGTmeinb5WAsRObPLrnlIjAI5qQ0I+bDNH3msjXZ5dU43OUkOknlRDw45R0/buThQv7jIUI64+FA8uLlPzjiWkB24TxBPeExPxThaz5h4CxCQd9QRuneUEbY3LC52NywueneDroi3+K7+96dvTzfQcKn3hnIyN4vyPfeAcRP30rE9N+8wlLgJ8U64M7GnuCXxQDhU8Me4UPCheEjwvfh48TE4e7GIOLkxo7iL8v64rXQ5OJ+0zXjItTR4vzVouLk2MHi8dcN44DXouNe2j7jz9xc4yXdqOMt4CfjouBv47/iJeOv5bDjOuXE5KnlYeWK5bPlZOfA5qbpL+cb6fHmBwCJ5ffql+ai6vXkUurW4lzrOeTh6unkfOuJ5ffqBQB0G5Xs2hqO7O4aAu2NG9bsdBuV7AYAzDgR8O05CPDHOR/vDTn97og47O/MOBHwCQCWQwTwEUTZ73dDD++cQ9/u+UK47iVCyO0cQfLtQUFh7pZDBPANADVE4PEMRTTxvEVG8YVFs/A5RbXwdES37zlE4O9SRGnw5kOR8D5EZvFwQ4PyGUQ58jVE4PEMAI4TtPquE935ZxPd+WYSQva9EQD2NBE99uUQYvdVESn4XRGq+aMSTfo3E0z7jhO0+jcAFDig+sk4JvovOZj4JToJ+Hg6Rvfh" ..
    "OkD30jvR9f07BfW5O6TzmDph8Sg5wPCnODLxGTjX8PA2JvGFNuLx9DUX8v01kvJzNTry1TUm8x81X/JzNELzTTOy80cxavNOMMPy1S668houTvLvLKTyMS2n80YszPVyLKH1UCz+9Z8suvVLLHr2lyyA958sNPeXLer3Ni9Q+A0wl/liMFX5PjCE+RkxcfqjMZr6ojIn+gYzGvvKM0X7fjOZ+7Qzp/vaNDf7UTVe+381LfvuNCT6xjYV+S83mfmsN9X7FDig+gUAnDAK/DowAPzdMJ78vjG4/JwwCvwHAG4qWv1EK3n9My27/MAslfwpKVP9bSmy/W4qWv0IAF473P2wOoj98DnB/aY6DP7pOt79Mjtf/oI7Uf5eO9z9BQD3Msv+GzN+/vYxrf4OMuT+9zLL/hcAZjSN/4I06/7qNK/+CTZW/3o4fv6tOaD9fzkd/d063vvHOQv8ijgF/bc3W/xZNtb8wTW3/Cs2JP3hNeX9NjSe/vIzZf6PM+b+OjQj/6czI//8MqL/tjPb/2Y0jf8TAOwwjgBRMBgA8i4YAD4vc/8uMML/dy9C/xww6v3AL/D98C9C/nUvN/5BL/n+/y7b/gsv1/2hLub9ri6j/mUu6P7kLjkAOS+DAOwwjgAFAEUycQAKMqb/xDFlAPkx2QBFMnEACQBWKbf95yi1/RIoWv45JSQCFCYNApAoCgBoKLn/cynO/lYpt/0OAAwutwB8LloABS5OAF8tb/4OK9v+nSrS/9YqyQBtK7kAgSsOASQsNgGZLbQCji4dAtMtQwEMLrcADQBeMUkDbjHPAkwxcwInMdkC+DCnAvwwLgKGMGgCSTAPA6AvzwI9MGUDAzGDA/0w0ANeMUkDBgC6H2wCYx9VAjMfpAJPH9YD8x/wArofbAIFAG4wBAQMMIYDzi/LA3gwYwRuMAQEBQBKLqQDxS1FA68ucQTBLh8ESi6kAwYABjHABCIxUQTVMGsEwDD1A4sw6AQGMcAEDwBkLzoHwS84B9wvrQaGLzkGjS+ZBWswYgV4MOYEBTBLBR8vagVDL60F5y7ZBdQuZAb9LkMGKC87B2QvOgcJAKbjwwet5K0HUOVFB2Xk" ..
    "Mwcc5OAG6uIqB7/jSwdS46wHpuPDBwUAGitMB3IqOwdvKpEHRyvYBxorTAcJAODg5QgG4+wHoOHCB+Ph+QdA4XAIDODYCM/ejgjd3w8J4ODlCAUAVi/nCCsvlQjrLjQJdi/iCVYv5wgFAJg0Vw1sNPgMtDPjDOwzTg2YNFcNBQCZAxkQxwNOD3EDMw8wA/8PmQMZEBQAEjeCDsk2ug2aNYUNCzUSDcQ0hA0rMz0NkDPzDE0zSQzcMkYM9TKgDI0yAg3OM9cNADXhDWg1kg6rNWMOdzbuDqQ22A85NyoQbDdODxI3gg4FALwDdxCbAyoQVgOCEKsDzRC8A3cQCwA3OEERxDhWEdo45hDuN2gQUTesEBs3PhCsNj0QnjagEDo38xB1N8sRNzhBEQUAIucvEsfnJBJx5/UR2eZBEiLnLxIFAMHP8xLqzhMT3M3VE+DOphPBz/MSDwAT6s0T0Ol1ExzrPRP96vQSQ+sFE2/rkhJF6zoS1upJEtDqpxJc6lASB+qbEtnomBKY6dATW+onFBPqzRMGACnMHBWJzCQVa8yyFMLMYhQHzN0UKcwcFQ0AHTjTE4E4IhPtN0MTsDeyEg84BhLDN0ISgTf1EYo35xNPN0oUWDfSFLU3ABWNNy8VHTjTEwcAWf1qFBr8PhRr/KYUN/wMFV/9jRXK/U8VWf1qFAUAO8RQFtfDKRaVw3IWksR/FjvEUBYWANP+6BZp/nsWPP+IFsj+3RUv/9cVLwCtFKgAmhSRAAkU8/2EE779mBOr/hcU8f1PFFr+bhQ2/uYUy/7cFNn+FhUc/mcVB/7KFdH9mxWZ/S8WC/7nFtP+6BYHALzephm24OUYid8KGZjeoRju3dIYdN6uGbzephkMAFX69hk++rUZr/pxGbb4zhgc9/wYfvcoGaT2WRlR94oZf/ahGVv38Rn2960ZVfr2GQ0Ao7sCGt68IxqivcYZm7yQGXC8GRlUuosZM7rTGby5uxnpuYwZsLliGbC58Bqru0Eao7sCGgUAM7qxG7C5qxuwufAborrXGzO6sRsBAaHcJhuh3MAaJt0OG+HdQBqS3t4amN5MG7rfNhtA4AQb/N+dGkLgaBpy" ..
    "3/EZf94AGuTdThmc2zoYBNsHF5bb9hby200W3d+LFezf0BTJ4AEUTOGIFNHgWxUa4hUWVOH4FsrhYRd94VgYKONkGNHk2hfv5AgXk+W9FsbmkhcE6EEW3OcCFpvpVxU46s8UQOpfFI3ooBMQ5p8TOuRKEpbmOxPv5goTkObHEtHmEBL45+wRXOhdEqTo8BF45gMRLOYKESjmXxHV5rERxuWiEWPkzxCr5EQQNeP9D+Xj/Q8d4+sPveI2D3/ibg+u4gAPVuKKDi3iSw8v4uAO8OHvDmvi4w064EgMuuCACprg2QkV4BsKm99eCybfwQsq3dgLJt1xC8ncYAtX25sLRdoPC/fZsgrF2cQIY9qMBx3bFgcM3E4Hi9yIB7zcNAj/3WoIQ901Bs7eQAZr3/cFQ99WBDDgbwPr4MED/OFgA73iVAT549wENuS7BOXjdgT+44sDQOTaAxzkSQSu5MAEXeUfBKfm8APU5zAEf+cSBKHn4wOt6VUC7OpAAvTrpAF77K4AUez4/wLt6P8G7YT/Uu3G/3XuZf+W7vP+YvDh/hjy/f1v8iL9R/J8/OXw5vqp8AX5AvBu95nvB/dj7Uf25+zN9PvqkPIK6mLyLenB8tHplvHd6ODwpufV8LnnHPB95/nvkOb2757mku8252DviuYC72PmaO635TXumuXq7VzmjO055jPt/uQv7GHljetT5FbrQ+T56rnilut34v3sC+Oq7XTix+314sbuZOOj7pjjcu9V44zvNuMP7/fiHu9m46rwQeN88RjkVvOY5Ej4FuQ4+U/iR/rY4DH9Q+Ca/TPgJv7X4Pb+X+Af/2Pglv/f4YEBduFAA+zgfQNn4NICit7hA4XeVQTT3TIFXdxxBQPbVAZI2h0GktclB8vWyweV1uUIKdRPC83TLQwq02wMNdPIC0HVDwkF1eoILtSqCSLUKQoO09QKaNMpC+DSjAsu0ukM4tCFDWjPvw+az8gRS8/SEujPxBId0GYSBNAkEzjO2xMPznEUj82cFKDLtBaJxsgXGsazFy7Gbhe8xBwXKsXwF9bDLxcfxP4WwcO3Fh3C3xWhv0AVZMJ9FqjCBBe7wOsWxcBLFwLA" ..
    "XBccvwYYuL+rGDPB6RjowCgZMsFPGZC/LRlVvqcZwb8CGtjA1BncvrQawcB5G9bC4Buuyuoakc1CG/LNiBvgziQbZs9oG3PPHBuN0EQbgtO4GvbShhoN1Y4aeNVSGuXVhRp/1a8av9XRGonW4Bpf2G0ajNl6GoDZuBrZ2coadNqoGnPaSRox2/saUdphG1nazxvP2hgcQNxrG+fbOhuh3CYbFQBn05AcNdNhHBDVgBy91f0b79UnHKjVjRxk1owc1NZjHDLXuxuD2C4b39cmG/3X2xq908YaKtJUGxbUfRvy0Y4bvdGzG6XS2xtc0fQb9dFnHGfTkBwhADDelBx/3lUc2N/PHITgaBx14CYcmuFrHA7j3RvI4/Qb1+UHGx/l2BrX5x4aCOdkGfDl7xlu5eIZYuWpGb3mwxib5nsYIuXnGCfmMRhC5JMYxeJEGaXhFxlQ4TkZkuGDGRzjkRmD43UaKOFpG9LeVRvj3NIbw9w4HBDdkRx53tQcMN6UHAoAzNjYHPbZ0Byz2YMcStpYHDja/hs02eAb9tdTHMTYZxxW2KgczNjYHAYAmNttHLvaJhx+2rAcFtv1HKXc2hyY220cCQDy0OQb68+yG8/OExyWz8gcNM8FHRTS+xzh0rQcbtFUHPLQ5BsGAK04hB1eODodSDYlHYE1Zh07Nr4drTiEHQYAhtn4HdLZyh2o2Uwd9teEHe/X0h2G2fgdCgC71cQdttZ8HSrUEh2F0zAdVdRcHQXSYh3s0uAdZdV7HdbU2x271cQdCwB6FqAb+BSkGxoUIRzWFScduxVUHeUXyR3lGuYd2BYHHaYVRRy6FfIbehagGwkABNseHindiR1P4JMd0eBEHfDcHB3m2zwdU9vQHRDa+x0E2x4eBQCc0lUeQdLlHQPQvB160UcenNJVHosBySkRHpUsoR28KvocIDCBHCYwzhyaMb0cOzKIHGkySBwuMh4cSTOnG6kzEByjNu0bWzZKHN82dRxmOjQcwTusGxw+rxtjPjwb3j4gG5BBLhs+QtUaukL1GmlCNRuVQmIbpERMG1BG8BpQRmIZTUU9GRFGmhgDRlYY2EMVGIlCZBf6QakX4j9j" ..
    "F0o/wBa/P4IWsj/wFSo/mRVUP24VpT46FYI+yBTtPbAUPz3tE7c8ohXnPC0W7z/iFz9AbxiMPqYXOj4hGDg9/xc+PFgXkDwbFxc79hYeOz4XgjpOF4w3EBfJNGEV+DUAFaY2KxU6N2gUtjbtEv41FxKvNPQQKjS5EKwz6BDRMYgPkjJeDm0ytA1pMW8NRDFZDo4xaQ63MOMO9DBzD4swmQ9JLzIPuS/KD4Qv/w8cLlAP6S0iD3MuoQ7ML6EO3C9tDosuow2fL2EMXi/8C7EvpwuILwcLWi6XCUUt5ghHK1wIJCvyB+0q7AfqKlwIZCp8CFwptwdGKXIHiCr4BbYqPwWoKo8EFClcAwwp4ANuKCcEEyjDBGMn7wRyJz0FGic9BcImnAM4KCgCtyiBAJsnFAEZJ4YCeiZGA2omCwOUJngE9CWdBkElIwbLJEQG2CQdB7Yj5QhaI+kIQyOICPohZgjKId8HGyB4BmAfNgYyHwwESh4dA7kcPwZfHFgIhxsoCO0Z7wkGGMwJbBYOChEWmgpgFVgKIBTjCpQTxwu9ErYL2ROrCe0TKQonFBQKOxRiCRkVbAkEFlAKNRZ4CV0XtwiXFugHiRZmB5cVuwZ3FGYGYRQYBgQTeAX8EPAEqRCNBkoPUQgJD0EJhw32CqQNhgtADc0KqgypC+ENBglnDpgIpA5FB1cPOAbsENcEsBCWBG0RFAT3E7IE8RMoBKYSpgG6D//+UA8s/igPeP3QD8v77g9D+mkPePmXDUb46Q299rkM8/WUDMX0Bgsz8xIKvvLRCMbyqgdm8i4HqvIeB6Lz8QVr9ZIFXfebBPH4mgTV+VkFz/uoBAj+cAOR/6wDdQFSA90BTgKqAbEBcwI8/9cBfPzjASX71gI0+kAEg/nBBB75wQWS+U4GsfkTB1/5jQhc+kEKRPyxC178uAxN/VMNr/34DSf/vQ2TAE0OtwOXDlYEag4KBDMNdQfTC9UHGwzdB5gMagjUDEsLDwwaDFQMMQ0ZDBAOiA0gDlEOswwbDswKUg45CmoPbAsaEBcNahD7Dv8PxQ8FEEoQZBBUDqwRSA92EqgNExIxDp8RPA1UEa0MtRECDQAS" ..
    "AwwyEs8KohBBCwkQTAqvD70J/w9DCeUPiQmtD1YJnA/XCLoPZAm2DggJ0A4LCToOyQg5DpUHuQ+iB0wQIgXeEdEEuhHrBDkROAexD5cGzA+pBjIPSgbXDgUGpQ95A1URjQLZEDYB1BAwAV0QUQAFEOT/Ww8LACIPKf9TDub9Cw50/W4OhvxnDlX8zxDi/BcRQv/2EHb/MhGJ//kRP/68EjX+BBNe/wATP/9yE53/RxOGAJUTpADnE38BKhTXAb0ULAPpFHADGhUsA7AVVgNPFiIEjRYBBDkWQwQOFsUDqxVGBBkV5ARHFYQFABXiBm0VrgdDFU8IjxVuCG0WbAlHFosJzhYeCR8XYAtzF/cKohfvCGEXVAi4F2oIrxjsCW8ZVgnJGaoIrBlZCCkZ+QaDGLAG9hdXB3gXkwbwFjQG6hUOBaAVDAQ7F0YDxxY3AuMW8wE1GB0EMRnEBX0afgdGG5cJvxsBC88bOQyFG7kLaxsmDCwbQw76GgoQWhoREBca/g7IGfYMBxqZDb4Zpg0pGXUO8RhEDk4Zhg5yGXcPNBnMD0wZiA+WGXEQ+BkrEc8ZZREUGvkQyRoREqkaShJxGswRLRobEgsa/BTmGkcV4RrlFKQa+BboGmoXrBrcF+4acxcoG6cXSRvDGpkaBhvOGiUaIRsNGr8bCBt0HFscbhwRHOUbbxx/G1gc9hrHHLka2BvoGUoc2RlRHXgaFx2xGkYd8xrAHDMbEB2XG44c6RtBHSwcKh1zHJAdPhxpHd4b1B3LG6YdExxOHjsc1h8HHHMfxRzqIeIcmSEWHQ0iWB1cJ9sdwyhaHskpER4TAC8TIBCvE7oPXROyDxYTKg84E64OBxVxDgwVNw+VFKMPrBT4D2IV/w/8FHQQoRQSEIIUtxCnE20RuBSuEbgUTRIuEyASPBJtES8TIBAGAKgJaR4ZCFgeXghyHiEIkR7wCKUeqAlpHgYA6tiYHhHZbx7q1p4eS9e8HsvW+h7q2JgeBQAMKZce2CZwHuEn/x4pKb8eDCmXHgcAIQciH2oI2B5vB7AesAYBHjcG/R0UBB0fIQciHwUA8QlpH7UKRh/+CAQfyQZgH/EJaR8G" ..
    "AAon0B4ZJeAeniNiH3olvR8jJyofCifQHgoAAt4eH3ve/h6I3I4euNuaHk3bwx6d2wIfOdpQH/7auR/n274fAt4eHxgAPuV3INfnRyCS5dYfbObXHzLkLB/14fwef+LwHjri3h6N4q0e2OApHpPh/h2I4MIdC93fHbfdJh6G3W4ezt5KHqTdnR7D3v8eC95ZHwfgbh/I3XQfOdz9HwXheSA+5XcgOABq9aAg2/dRIIrzHCBh9+0f8/azH9f5/x87+8EfK/hSHxP5TR9O+MMeUfhUHsn4Ex6I9+8dQfi6HVj4ZR3t91wdb/gGHcv2oxxK9zIcUfZBHFv36xuB95obzvaHGwb25xso9qMbtPVvG0X3ZRtz8JIZ6e/MGEbvfBhu7y4YDu96FybtxhfV69sY6+o/GiHsURuk6jEbxOqqG+3rkRsx6v0boOpbHB3pgB0+5bYdHOQVHuzlOh5e43weVeYDH3zmKB9u5UwfsefEH4fn8R9R7DQgnO7mH7ztRyDq8KMgavWgIA=="
local _DOTS_B64 =
    "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" ..
    "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" ..
    "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" ..
    "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" ..
    "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" ..
    "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" ..
    "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" ..
    "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"
local _ICON_B64 = "AD4ACAgISRxJaSJLVUFVC8HoVUFVaSJLSRxJCAgIAD4AAAAA"

local COAST, DOTS, ICON

local function b64dec(s)
    local A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    local t, acc, nb, n = {}, 0, 0, 0
    for i = 1, #s do
        local ch = s:sub(i, i)
        if ch == "=" then break end
        local v = A:find(ch, 1, true)
        if v then
            acc = acc * 64 + (v - 1); nb = nb + 6
            if nb >= 8 then
                nb = nb - 8; n = n + 1
                t[n] = math.floor(acc / (2 ^ nb)) % 256
                acc = acc % (2 ^ nb)
            end
        end
    end
    return t, n
end

local function geo_prepare()
    if COAST then return end
    -- 海岸线
    local t, n = b64dec(_COAST_B64)
    local PX, CL, SL, LS, LN = {}, {}, {}, {}, {}
    local i = 1
    while i + 1 <= n do
        local cnt = t[i] + t[i + 1] * 256; i = i + 2
        if cnt == 0 or cnt > 4096 or i + cnt * 4 > n + 1 then break end
        LS[#LS + 1] = #PX + 1
        LN[#LN + 1] = cnt
        for _ = 1, cnt do
            local lon = t[i] + t[i + 1] * 256; i = i + 2
            local lat = t[i] + t[i + 1] * 256; i = i + 2
            if lon >= 32768 then lon = lon - 65536 end
            if lat >= 32768 then lat = lat - 65536 end
            lon = lon / 100 * math.pi / 180
            lat = lat / 100 * math.pi / 180
            PX[#PX + 1] = lon
            CL[#CL + 1] = math.cos(lat)
            SL[#SL + 1] = math.sin(lat)
        end
    end
    COAST = { px = PX, cl = CL, sl = SL, ls = LS, ln = LN }

    -- 点阵 (int16 对, 每 3 个取 1 个)
    local d, dn = b64dec(_DOTS_B64)
    local DX, DCL, DSL = {}, {}, {}
    local k = 1
    while k + 3 <= dn do
        if ((k - 1) / 4) % 3 == 0 then
            local lon = d[k] + d[k + 1] * 256
            local lat = d[k + 2] + d[k + 3] * 256
            if lon >= 32768 then lon = lon - 65536 end
            if lat >= 32768 then lat = lat - 65536 end
            lon = lon / 100 * math.pi / 180
            lat = lat / 100 * math.pi / 180
            DX[#DX + 1] = lon
            DCL[#DCL + 1] = math.cos(lat)
            DSL[#DSL + 1] = math.sin(lat)
        end
        k = k + 4
    end
    DOTS = { px = DX, cl = DCL, sl = DSL }

    -- 图标 24x12 (3 字节/行)
    local ic = b64dec(_ICON_B64)
    ICON = ic
end

local SAT_NAME = { "GPS", "GLO", "GAL", "BDS", "QZS", "NIC", "MIX" }

-- ★ 2026-10-27: sat_demo() 已删除 —— 演示数据整块取消(linit: 不需要显示演示数据)。
--   原来它生成的假卫星会被"空数据自动回退"逻辑用上, 是"卫星状态异常"的根源之一。

local function icon_prepare()
    if M._iconpts then return end
    local p, t = {}, ICON
    for row = 0, 11 do
        for bx = 0, 2 do
            local byte = t[row * 3 + bx + 1] or 0
            for k2 = 0, 7 do
                if math.floor(byte / (2 ^ (7 - k2))) % 2 == 1 then
                    p[#p + 1] = bx * 8 + k2      -- dx
                    p[#p + 1] = row              -- dy
                end
            end
        end
    end
    M._iconpts = p
end

-- 正面: 完整 24x12 图标 (白边 + 黑笔画); 背面: 3x3 小点 (看得见但不抢戏)
local function icon_draw(x, y, behind)
    if behind then
        app.fill(x + 10, y + 4, x + 12, y + 6, BK)
        return
    end
    icon_prepare()
    app.fill(x - 2, y - 2, x + 25, y + 13, WH)   -- 2px 白底, 压在地球上也认得出
    local p = M._iconpts
    for i = 1, #p, 2 do
        app.px(x + p[i], y + p[i + 1], BK)
    end
end

local function page_sat()
    app.fill(0, 0, W - 1, MAP_B, WH)

    local fix  = M.fix
    local sats = M.sats or {}
    -- ★ 2026-10-27 linit「不需要显示演示数据」: 彻底取消演示回退。
    --   没有卫星就是没有 —— 页面老实显示"正在搜星", 让真实 GPS 状态暴露出来。
    --   (原来 #sats==0 会静默换一批会转的假卫星, 只在角落写一行小字, 极易被当成真实状态。)
    local no_sat = (#sats == 0)
    geo_prepare()

    local EX, EY, EW = math.floor((W - 244) / 2), 4, 244
    local CX, CY, CR = EX + EW / 2, EY + EW / 2, EW / 2

    -- ★ 连续自转 60 秒/圈: 用固定起点做基准, 只增不减 ⇒ 永远不"跳回去"
    local t = (app.millis and app.millis() or 0)
    if not M.spin0 then M.spin0 = t end
    local lon0 = ((t - M.spin0) / 60000) * 2 * math.pi

    local la0 = 25 * math.pi / 180
    local sla, cla = math.sin(la0), math.cos(la0)

    -- 我的位置 (GPS 定位; 无定位用默认点) → 投影到球面; 在球的背面就不画链路
    local mylat, mylon = 25.43, 119.01
    if fix and fix.lat_e6 and fix.lat_e6 ~= 0 then
        mylat, mylon = fix.lat_e6 / 1e6, fix.lon_e6 / 1e6
    end
    local mlat = math.rad(mylat)
    local dml = math.rad(mylon) - lon0
    local mcl, msl = math.cos(mlat), math.sin(mlat)
    local mz = sla * msl + cla * mcl * math.cos(dml)
    local MYX = math.floor(CX + CR * mcl * math.sin(dml) + 0.5)
    local MYY = math.floor(CY - CR * (cla * msl - sla * mcl * math.cos(dml)) + 0.5)
    local MYPOS = mz > 0

    ring(CX, CY, CR); ring(CX, CY, CR - 1)
    app.px(CX, CY, BK); app.px(CX - 1, CY, BK); app.px(CX + 1, CY, BK)
    app.px(CX, CY - 1, BK); app.px(CX, CY + 1, BK)      -- 天顶标记

    -- 灰阶点阵 (只画正面)
    do
        local P, CL, SL = DOTS.px, DOTS.cl, DOTS.sl
        -- 步长 2 (再抽一半): 240px 球上肉眼差别很小, 但每帧少一半 Lua→C 调用
        for i = 1, #P, 2 do
            local dl = P[i] - lon0
            local z = sla * SL[i] + cla * CL[i] * math.cos(dl)
            if z > 0 then
                local x = math.floor(CX + CR * CL[i] * math.sin(dl) + 0.5)
                local y = math.floor(CY - CR * (cla * SL[i] - sla * CL[i] * math.cos(dl)) + 0.5)
                app.px(x, y, BK)
            end
        end
    end
    -- 海岸线
    do
        local P, CL, SL, LS, LN = COAST.px, COAST.cl, COAST.sl, COAST.ls, COAST.ln
        for li = 1, #LS do
            local s0, cnt = LS[li], LN[li]
            local pvx, pvy, pvis, plon
            for k = 0, cnt - 1 do
                local idx = s0 + k
                local lon = P[idx]
                -- ★ 跨 180° 经线的两点在屏幕上会横穿整个地球(每转一圈闪过一条长线)
                --   真实大陆轮廓在这一处是断开的 ⇒ 间距 > 3.0 rad 就不连线
                local seam = (plon ~= nil) and math.abs(lon - plon) > 3.0
                local dl = lon - lon0
                local z = sla * SL[idx] + cla * CL[idx] * math.cos(dl)
                local x = math.floor(CX + CR * CL[idx] * math.sin(dl) + 0.5)
                local y = math.floor(CY - CR * (cla * SL[idx] - sla * CL[idx] * math.cos(dl)) + 0.5)
                if z > 0 and pvis and not seam then seg(pvx, pvy, x, y) end
                pvx, pvy, pvis, plon = x, y, (z > 0), lon
            end
        end
    end

    -- 我的位置: 实心方块 + 十字 (所有链路都汇聚到这里)
    if MYPOS then
        app.fill(MYX - 2, MYY - 2, MYX + 2, MYY + 2, BK)
        app.fill(MYX - 1, MYY - 1, MYX + 1, MYY + 1, WH)
        app.px(MYX, MYY, BK)
    end

    -- ================= 卫星: 天球极坐标 (标准 GNSS 星空图) =================
    -- ★ 为什么改: 之前让卫星"跟着地球转", 结果球缘的遮挡会把同一条带上的星
    --   一起啃掉 ⇒ 一下消失一大片; 而且同一条带的星并排走 ⇒ 看着像"一排"。
    --   卫星相对接收机的位置本来就是固定的(它们的方位/仰角来自 GSV),
    --   地球转不转跟它们无关 ⇒ 直接按 el/az 画在天球上, 永不消失、分布还真实。
    --     圆心 = 天顶(el=90°), 圆周 = 地平(el=0°), 方位角绕圆心(北=上)
    local used_n, snr_best = 0, -1
    local lbx, lby = {}, {}
    -- ★★★ 2026-10-27 linit「卫星要跟地球一起转」:
    --   用 az/el 在**观测点局部坐标**里算出方向向量 → 转成地心坐标 → 取球面点
    --   (等效星下点), 再用**和地球同一个 lon0** 投影 ⇒ 卫星贴在地球上一起转。
    --   上一版放弃跟转是因为两个副作用, 这里各给了解法:
    --     ① 转到背面整片消失 → 不隐藏, 压到**球缘**当"侧面"标记(数量恒定, 不会一下没一片)
    --     ② 同仰角带排成一条线 → 用真实 az 参与运算, 分布是自然的, 不再按 PRN 排带
    local mcl2, msl2 = math.cos(mlat), math.sin(mlat)
    local mlon = math.rad(mylon)
    local mlc, mls = math.cos(mlon), math.sin(mlon)
    local atan2 = math.atan2 or math.atan
    for i = 1, #sats do
        local sat = sats[i]
        if sat.used then used_n = used_n + 1 end
        if (sat.snr or -1) > snr_best then snr_best = sat.snr or -1 end

        local el = clampi(sat.el or 0, 0, 90)
        local az = math.rad(sat.az or 0)
        local ce, se = math.cos(math.rad(el)), math.sin(math.rad(el))
        local E, N, U = ce * math.sin(az), ce * math.cos(az), se      -- 东/北/天 分量
        local dx = -mls * E - msl2 * mlc * N + mcl2 * mlc * U
        local dy =  mlc * E - msl2 * mls * N + mcl2 * mls * U
        local dz =            mcl2 * N       + msl2 * U
        local slat = math.asin(clampi(dz, -1, 1))
        local slon = atan2(dy, dx)
        local dls  = slon - lon0
        local cl2  = math.cos(slat)
        local zt   = sla * math.sin(slat) + cla * cl2 * math.cos(dls)
        local x = CX + CR * cl2 * math.sin(dls)
        local y = CY - CR * (cla * math.sin(slat) - sla * cl2 * math.cos(dls))
        if zt <= 0 then
            -- 背面: 投影点往球缘推, 当作"侧视"标记 —— 不再整片消失
            local vx, vy = x - CX, y - CY
            local m = math.sqrt(vx * vx + vy * vy)
            if m < 1 then m = 1 end
            x = CX + vx / m * (CR - 2)
            y = CY + vy / m * (CR - 2)
        end
        x = math.floor(x + 0.5); y = math.floor(y + 0.5)

        icon_draw(x - 12, y - 6, false)              -- 永远画满, 不再有"消失"分支
        -- 只有参与解算的星才贴名字(并做防重叠), 否则 18 个名字必糊
        if sat.used then
            local nm = SAT_NAME[(sat.sys or 6) + 1]
            local lx, ly = x - 12, y + 9
            local clash = false
            for j = 1, #lbx do
                if math.abs(lx - lbx[j]) < 28 and math.abs(ly - lby[j]) < 20 then clash = true; break end
            end
            if not clash then
                lbx[#lbx + 1] = lx; lby[#lby + 1] = ly
                app.mix(lx, ly, nm, BK)
            end
        end
    end

    -- 左右中文信息
    local hh, mm = 0, 0
    if fix then hh, mm = fix.hh or 0, fix.mm or 0 end
    local function item(x, y, name, val)
        app.mix(x, y, name, false)
        app.mix(x, y + 18, val or "-", BK)
    end
    local function ritem(x, y, name, val)
        app.mix(x, y, name, false)
        local v = val or "-"
        app.mix(W - 6 - #v * 8, y + 18, v, BK)
    end
    -- 左列: 卫星(可见) → 已用 → 精度 → 海拔  (越靠上越重要)
    item(6, 30, "卫星", tostring(#sats))
    item(6, 80, "已用", tostring(used_n))
    item(6, 130, "精度", fix and string.format("%.1f", (fix.hdop100 or 0) / 100) or "-")
    item(6, 180, "海拔", fix and (tostring(fix.alt_m or 0) .. "m") or "-")
    -- 右列: 时间 → 速度 → 航向 → 信噪(含条)
    ritem(W - 38, 30, "时间", string.format("%02d:%02d", hh, mm))
    ritem(W - 38, 80, "速度", fix and string.format("%.1f", (fix.kmh10 or 0) / 10) or "-")
    ritem(W - 38, 130, "航向", fix and string.format("%d", (fix.crs10 or 0) / 10) or "-")
    ritem(W - 38, 180, "信噪", snr_best >= 0 and tostring(snr_best) or "-")

    -- 信噪: 一根连续条 (1px 外框 + 左端填充, 50 dB 满格), 不分段
    do
        local ratio = 0
        if snr_best > 0 then ratio = math.min(1, snr_best / 50) end
        local x0, x1, y0, y1 = W - 46, W - 6, 218, 223   -- 跟"信噪"一起挪到最下面那行
        app.hline(x0, x1, y0, BK); app.hline(x0, x1, y1, BK)
        app.vline(x0, y0, y1, BK); app.vline(x1, y0, y1, BK)
        local w = math.floor((x1 - x0 - 2) * ratio + 0.5)
        if w > 0 then app.fill(x0 + 1, y0 + 1, x0 + w, y1 - 1, BK) end
    end

    -- ★ 2026-10-27: 底栏(盖子线+左右斜坡+谷线)统一由 render() 的 draw_softbar 画,
    --   这里只负责算凹口里轮播的状态文字 (M.sat_mid)。
    do
        -- 轮播: 先报"模块通不通 / 用什么协议 / 什么速率", 再报数据
        local msgs = {}
        local fx = nil
        if app.gps and app.gps.fix then fx = app.gps.fix() end
        if fx == nil or (fx.rx or 0) == 0 then
            msgs[#msgs + 1] = "GPS 未连接 查 TX/RX 与供电"
        elseif fx.link then
            msgs[#msgs + 1] = string.format("GPS 已连接 %d %s", fx.baud or 0, fx.proto or "-")
            msgs[#msgs + 1] = string.format("字节 %d 句 %d 错 %d",
                                            fx.rx or 0, fx.sentences or 0, fx.badcs or 0)
        else
            msgs[#msgs + 1] = string.format("GPS 断流 %d 秒", math.floor((fx.age_rx or 0) / 1000))
            msgs[#msgs + 1] = string.format("字节 %d 句 %d 错 %d",
                                            fx.rx or 0, fx.sentences or 0, fx.badcs or 0)
        end
        if no_sat then msgs[#msgs + 1] = "正在搜星: 暂未收到卫星数据" end
        msgs[#msgs + 1] = string.format("可见 %d 已用 %d", #sats, used_n)
        msgs[#msgs + 1] = string.format("精度 %.1f 海拔 %dm",
                                        (fix and fix.hdop100 or 0) / 100, fix and fix.alt_m or 0)
        local slot = 2600
        local idx = math.floor(t / slot) % #msgs + 1
        M.sat_mid = msgs[idx] or ""
    end
    -- 记录本帧耗时: poll 用它决定下一次重绘间隔, 避免"渲染没画完 DMA 就把帧读走"
    M.sat_cost = (app.millis() or t) - t
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
    -- ══ B9 图层开关: 4 项互相独立, 各自落 NVS (见 lay_save) ══
    -- 关「文字标注」只停 add_label, 地标菱形照画; 关「地标」只让 draw_poi 早退,
    -- GPS 点/收藏/导航目标都不受影响 (render 里三条路径互相独立)。
    { "道路显示", function() M.road_on = not M.road_on; lay_save() end,
      function() return M.road_on and "开" or "关" end },
    { "等高线", function()
          if not M.contour then app.dlg.toast("当前地图包不含等高线", 1300); return end
          M.cont_on = not M.cont_on
          lay_save()
      end,
      function()
          if not M.contour then return "无数据" end
          return M.cont_on and "开" or "关"
      end },
    -- ── B2: 已存轨迹列表 (选一条 → 地图叠加查看; 按返回退出) ──
    -- ⚠ 取值函数**每帧**都会被 page_set 调用 ⇒ 这里绝不能扫目录, 只回静态文案。
    { "历史轨迹", function() track_list() end,
      function() return "查看" end },
    -- ── B10: 地图包管理 (列表 / 切换 / 删除) ──
    { "管理地图包", function()
          M.pm_sel, M.pm_top = 1, 1
          M.set_cat = 0
          M.page = 5
          app.redraw()
      end,
      function() return M.pkg and M.pkg.name or "无" end },
    -- ★ 2026-10-27: 「卫星信息」从这里删掉 —— 它不是设置项, 左侧第 3 键就是同一页,
    --   放在设置里只会让人以为要在这儿点才能看卫星。
}

-- ★★★ 2026-10-27 linit「设置所有界面要二级菜单, 没必要显示的都不显示」:
--   一级只列分类, 点进去才是具体项。分类只保留 4 组, 每组两三项, 一屏放得下不用滚。
--   SET_GROUPS 里存的是 SET_ROWS 的下标(保持原数组索引, 逻辑不动)。
local SET_GROUPS = {
    -- ★ B9: 「显示」组扩到 4 项 (道路 8 / 等高线 9 / 地标 6 / 文字 3) —— 4 项 + 返回 = 5 行
    { "显示",   { 8, 9, 6, 3 } },   -- 道路 / 等高线 / 地标 / 文字标注
    { "单位",   { 1 } },      -- 公里 / 英里
    -- ★ B10: 「地图包」组加「管理地图包」(11); 3 项 + 返回 = 4 行
    { "地图包", { 2, 7, 11 } },   -- 地图包切换 / 下载源 / 管理
    -- ★ B2: 「数据」组加「历史轨迹」(10); 3 项 + 返回 = 4 行
    { "数据",   { 4, 5, 10 } },   -- 清空收藏 / 导出 GPX / 历史轨迹
}
-- 7 行设置 + 3 行包信息, 一屏放得下。
-- ⚠ 行高从 26 收到 22: 7 行时 y 到 223, 信息区最后一行的底边 285 < MAP_B(299) ✓。
--   行高 26 时信息区会掉到 313, **跑出屏幕** (设置页就看不见包信息了)。
local SET_H = 26   -- ★ 二级菜单每页最多 4 行, 行高放大到 26 —— 手指好点, 也不浪费屏

local function set_row_y(i) return MAP_T + 34 + (i - 1) * SET_H end

-- 当前层要显示的行: nil = 一级(只列分类); 否则返回该组的 SET_ROWS 下标表
local function set_cur_rows()
    local g = SET_GROUPS[M.set_cat or 0]
    if not g then return nil end
    return g[2]
end

local function page_set()
    local lvl = M.set_cat or 0
    app.mix(PX, MAP_T + 6, (lvl > 0) and SET_GROUPS[lvl][1] or "设置", false)
    app.hline(4, W - 5, MAP_T + 26, BK)
    local rows = set_cur_rows()
    if not rows then
        -- ===== 一级: 只列分类 (不再把 7 项全铺出来) =====
        for i = 1, #SET_GROUPS do
            local y = set_row_y(i)
            app.mix(PX, y + 3, SET_GROUPS[i][1], false)
            -- 只有一个子项的分类(如"单位")直接显示当前值; 多项的显示"n 项"
            local gsub = SET_GROUPS[i][2]
            local v = (#gsub == 1) and SET_ROWS[gsub[1]][3]() or (tostring(#gsub) .. " 项")
            local vw = app.mixw(v)
            app.mix(W - 14 - vw, y + 3, v, false)
        end
        return
    end
    -- ===== 二级: 具体项 + 底部"返回" =====
    for k = 1, #rows do
        local r = SET_ROWS[rows[k]]
        local y = set_row_y(k)
        app.mix(PX, y + 3, r[1], false)
        local v = r[3]()
        local vw = app.mixw(v)
        app.mix(W - 14 - vw, y + 3, v, false)
    end
    local yb = set_row_y(#rows + 1)
    app.hline(PX, W - 5, yb - 2, BK)
    app.mix(PX, yb + 3, "返回", false)
    -- 只有「地图包」这一组才显示包信息 —— 其它组显示它纯属干扰
    if lvl == 3 then
        local y = yb + SET_H + 6
        local s = pkg_summary()
        if not s then
            app.mix(PX, y, "未加载地图包 (只从 TF 卡读)", false)
            return
        end
        app.mix(PX, y, s, false)
        if M.pkg and M.pkg.tiled then
            local n = 0
            for _ in pairs(M.tiles or {}) do n = n + 1 end
            app.mix(PX, y + 16, string.format("视口 %d 块 / 已载 %d 块 %.0fKB",
                     M.tneed or 0, n, (M.tbytes or 0) / 1024), false)
        else
            local d = M.road
            app.mix(PX, y + 16, d and ("道路 " .. d.nfeat .. " 条 / " .. d.npt .. " 点")
                               or "无数据", false)
        end
    end
end

-- ============================================================================
-- render
-- ============================================================================
function render()
    app.clear()
    -- ★ 错误页只在"地图页"(page 1) 显示: 否则没地图包时连卫星页/设置页都被它顶掉
    if M.err_kind and M.page == 1 then
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
        app.mixc(W / 2, 246, "或从应用商店的外网托管站下载", false)
        draw_metric_bar()
        draw_softbar("设置", "GPS", "")
        return
    end

    setup_proj()
    if M.page == 2 then page_trip()
    elseif M.page == 3 then page_sat()
    elseif M.page == 4 then page_set()
    elseif M.page == 5 then page_pkg()      -- ★ B10 地图包管理
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

    -- ★ B2 历史轨迹: 压在路网之上、点位标记之下 (查看态才有)
    if M.hist then track_draw() end

    -- ★ 三种"点"标记 + 统一摆名字: 地标 / 收藏 / 导航目标 / 路名 一起避让
    LBL = {}
    draw_poi()
    draw_favs()
    draw_target()
    if M.labels then
        collect_road_labels(nil, named)
        M.lbl_n = place_labels(M.z <= 11 and 10 or (M.z <= 13 and 16 or 22))
    end
    -- ★ B3: GPS 点必须**最后**画 —— 标注的白底方框会把它整个盖住 (实测: 箭头/精度圈
    --   正好压在路名下面时完全看不见)。放最后 = 位置标记永远在最上层。
    draw_gps_dot()
    draw_scalebar()
    draw_buttons()
    if M.dr_on or M.dr_x < W then draw_drawer() end
    end

    -- ★ 2026-10-27 linit: 顶部指标条(16px, 名字一行/数值在下方) + 底部软键栏,
    --   **所有页面都显示**。卫星页整屏自绘(它本身就是 GPS 数据页), 顶部不放指标条,
    --   但底栏一样有 → 左「设置」右「地图」。
    if M.page ~= 3 and M.page ~= 5 then draw_metric_bar() end
    if M.page == 3 then
        draw_softbar("设置", "地图", M.sat_mid or "")
    elseif M.page == 5 then
        draw_softbar("设置", "切换", "")       -- ★ B10: 左回设置, 右执行切换
    elseif M.page == 4 then
        draw_softbar("行程", "GPS", "")
    elseif M.page == 2 then
        draw_softbar("设置", "GPS", "")
    else
        local sats = (M.fix and M.fix.sats) or 0
        draw_softbar("设置", "GPS", string.format("z%d  %d星", M.z, sats))
    end
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
-- B2 历史轨迹: 列表 / 解析 / 查看
-- ============================================================================
-- 数据来源: gpx_save() 落盘的 /sdcard/Map/track_YYYYMMDD_HHMM.gpx
--   单点一行 64 B, 本应用最多 160 点 ⇒ 一个文件约 10.5 KB, 远低于 fs.read 的 512 KB 上限,
--   所以"整文件一次读完"就够, 不需要分段读。
-- ⛔ 外部导入的 GPX 可能很大 ⇒ 三重护栏: 文件 ≤128 KB、点数等间隔抽样到 ≤512、只读不改。
-- ⛔ 坐标必须**字符串切分**转整数微度: lua_Number 是 float32, tonumber("119.011000")*1e6
--   会漂 ~1 m; 与上面的 deg() 正好互为逆运算。
-- ⚠ 本段函数必须写成**全局**: 顶层 local 已顶到 200 上限 (见 draw_softbar 上方注释)。
--   位置在 clamp_view 之后 —— 它们要用 setup_proj / sx_of / poly_draw 这些**局部**函数。
function e6_of(s)
    if not s or s == "" then return nil end
    local sign = 1
    if s:sub(1, 1) == "-" then sign = -1; s = s:sub(2) end
    local ip, fp = s:match("^(%d+)%.(%d*)$")
    if not ip then
        ip, fp = s:match("^(%d+)$"), ""
    end
    if not ip then return nil end
    local a = tonumber(ip)
    local b = tonumber((fp .. "000000"):sub(1, 6))
    if not a or not b then return nil end
    return sign * (a * 1000000 + b)
end

-- 从 GPX 正文取点/微度 (返回 平坦数组 + 点数; 每点占 2 个元素)
function gpx_points(body, maxn)
    if not body or #body < 32 then return nil, 0 end
    local cnt = 0
    for _ in body:gmatch('lat="[%d%.%-]+"') do cnt = cnt + 1 end
    if cnt < 2 then return nil, 0 end
    local step = 1
    if cnt > maxn then step = math.ceil(cnt / maxn) end
    local pts, np, k = {}, 0, 0
    local ta, tb
    for la, lo in body:gmatch('lat="([%d%.%-]+)"%s+lon="([%d%.%-]+)"') do
        local a, b = e6_of(la), e6_of(lo)
        if a and b then
            ta, tb = a, b
            if k % step == 0 and np < maxn then
                np = np + 1
                pts[2 * np - 1], pts[2 * np] = a, b
            end
        end
        k = k + 1
    end
    -- 等间隔抽样会丢掉终点 —— 末点一定要留, 否则轨迹看着"没走到"
    if ta and (np == 0 or pts[2 * np - 1] ~= ta or pts[2 * np] ~= tb) then
        if np < maxn then np = np + 1 end
        pts[2 * np - 1], pts[2 * np] = ta, tb
    end
    return pts, np
end

-- 只算总长 (列表页用); 超限文件直接放弃, 不读进来
function track_dist(path)
    local sz = app.fs.size(path)
    if not sz or sz <= 0 or sz > 131072 then return nil end
    local body = app.fs.read(path)
    if not body then return nil end
    local pa, pb, total = nil, nil, 0
    for la, lo in body:gmatch('lat="([%d%.%-]+)"%s+lon="([%d%.%-]+)"') do
        local a, b = e6_of(la), e6_of(lo)
        if a and b then
            if pa then total = total + dist_m(pa, pb, a, b) end
            pa, pb = a, b
        end
    end
    return total
end

-- 列出已存轨迹 → 系统弹窗选一条 (项必须纯 ASCII: 弹窗每项只有 30 字节, 截在汉字中间会越界读)
function track_list()
    local dir = map_dir()
    if not dir then app.dlg.toast("找不到 Map 目录", 1300); return end
    local names = app.fs.listdir(dir)
    if not names then app.dlg.toast("读不到目录", 1300); return end
    local cand, nc = {}, 0
    for i = 1, #names do
        local fn = names[i]
        if fn and fn:match("^track_%d%d%d%d%d%d%d%d_%d%d%d%d%.gpx$") then
            nc = nc + 1
            cand[nc] = fn
            if nc >= 64 then break end
        end
    end
    if nc == 0 then app.dlg.toast("还没有存过轨迹", 1300); return end
    table.sort(cand, function(a, b) return a > b end)      -- 文件名即时间 ⇒ 倒序 = 新的在前
    local items, n = {}, 0
    D.tracks = {}
    for i = 1, nc do
        if n >= 8 then break end                                  -- 最多 8 条, 控总读取量
        local fn = cand[i]
        local sz = app.fs.size(dir .. "/" .. fn) or 0
        local dt = "?"
        if sz > 0 and sz <= 32768 then                             -- 只给正常大小的文件算距离
            local m = track_dist(dir .. "/" .. fn)
            if m then dt = fmt_km(m * 10) .. "km" end
        end
        n = n + 1
        D.tracks[n] = fn
        items[n] = string.format("%s %s:%s %s %dK", fn:sub(11, 14), fn:sub(16, 17),
                                 fn:sub(18, 19), dt, math.floor(sz / 1024 + 0.5))
    end
    M.asking = "track"
    app.dlg.list("历史轨迹", items, 1)
end

-- 载入一条轨迹 → 自动选 z 让整条尽量铺满 → 进入查看态
function track_open(fn)
    local dir = map_dir()
    if not dir then return end
    local path = dir .. "/" .. fn
    local sz = app.fs.size(path)
    if not sz then app.dlg.toast("文件读不到", 1300); return end
    if sz > 131072 then app.dlg.toast("轨迹文件过大, 未加载", 1600); return end
    local body = app.fs.read(path)
    if not body then app.dlg.toast("读不出轨迹", 1300); return end
    local pts, np = gpx_points(body, 512)
    if not pts or np < 2 then app.dlg.toast("轨迹点不足", 1300); return end

    local minla, maxla, minlo, maxlo = pts[1], pts[1], pts[2], pts[2]
    local total = 0
    for i = 2, np do
        local a, b = pts[2 * i - 1], pts[2 * i]
        if a < minla then minla = a elseif a > maxla then maxla = a end
        if b < minlo then minlo = b elseif b > maxlo then maxlo = b end
        total = total + dist_m(pts[2 * i - 3], pts[2 * i - 2], a, b)
    end
    local spanlo, spanla = maxlo - minlo, maxla - minla
    if spanlo < 1000 then spanlo = 1000 end            -- 1000 微度 ≈ 100 m, 免得一点轨迹推到 z18
    if spanla < 1000 then spanla = 1000 end
    local clat = math.floor((minla + maxla) / 2 + 0.5)
    local coslat = math.cos(clat / 1e6 * math.pi / 180)
    local z = ZMIN
    for t = ZMAX, ZMIN, -1 do                          -- 从最大档往下试, 不放 log 换底
        local kx = 0.711105 * (2 ^ t) / 1000000
        if spanlo * kx <= 300 and spanla * (kx / coslat) <= 190 then z = t; break end
    end
    M.hist = { pts = pts, n = np, dist = fmt_km(total * 10) .. "km",
               tag = fn:sub(11, 14) .. " " .. fn:sub(16, 17) .. ":" .. fn:sub(18, 19) }
    M.z = z
    M.clon = math.floor((minlo + maxlo) / 2 + 0.5)
    M.clat = clat
    setup_proj()
    clamp_view()
    M.page = 1
    app.dlg.toast("历史 " .. M.hist.tag .. "  " .. M.hist.dist, 1500)
    app.redraw()
end

-- 画历史轨迹 (地图页叠加): 3px 抽稀 → 一次 app.poly; 起点空心方 / 终点实心方
function track_draw()
    local h = M.hist
    if not h then return end
    local pts, np, px, py = {}, 0, nil, nil
    for i = 1, h.n do
        local x = math.floor(sx_of(h.pts[2 * i]) + 0.5)
        local y = math.floor(sy_of(h.pts[2 * i - 1]) + 0.5)
        if px then
            local dx, dy = x - px, y - py
            if dx * dx + dy * dy >= 9 then
                np = np + 2
                pts[np - 1], pts[np] = x, y
                px, py = x, y
            end
        else
            np = 2
            pts[1], pts[2] = x, y
            px, py = x, y
        end
    end
    if np >= 4 then poly_draw(pts, np, 2) end
    local x0 = math.floor(sx_of(h.pts[2]) + 0.5)
    local y0 = math.floor(sy_of(h.pts[1]) + 0.5)
    app.rect(x0 - 3, y0 - 3, x0 + 3, y0 + 3, BK)                 -- 起点: 空心
    local xn = math.floor(sx_of(h.pts[2 * h.n]) + 0.5)
    local yn = math.floor(sy_of(h.pts[2 * h.n - 1]) + 0.5)
    app.fill(xn - 2, yn - 2, xn + 2, yn + 2, BK)                 -- 终点: 实心
    local txt = "历史 " .. h.tag .. "  " .. h.dist
    local tw = app.mixw(txt)
    app.fill(2, MAP_T + 2, tw + 8, MAP_T + 19, WH)
    app.mix(6, MAP_T + 3, txt, false)
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
    -- ★ B10: 换包前先留一份旧状态 —— 新包载不起来时整组回滚, 原地图不能坏。
    -- ⚠ old 是**函数内** local, 不占顶层 200 的名额。
    local old = { pkg = M.pkg, ov = M.ov, road = M.road, contour = M.contour,
                  poi = M.poi, tiles = M.tiles, tbytes = M.tbytes }
    M.tiles, M.torder, M.tbytes, M.tmiss = {}, {}, 0, 0
    M.hist = nil          -- ★ B2: 换包后历史轨迹可能已不在新包范围内, 直接退出查看态
    if not p then
        M.pkg, M.ov, M.road, M.contour, M.poi, M.map_dir = nil, nil, nil, nil, nil, nil
        return false
    end
    pkg_load(p)
    if p.tiled and not M.ov then
        -- 概览层读不出来 = 这个包不可用 (文件被删/被截断/清单损坏) ⇒ 恢复原图, 视口不动
        M.pkg, M.ov, M.road, M.contour, M.poi = old.pkg, old.ov, old.road, old.contour, old.poi
        M.tiles, M.tbytes = old.tiles, old.tbytes
        -- ⛔ 只有"连旧包都没有"才置错误态: 否则回滚成功后首页会被错误页盖住,
        --   用户明明还能看旧图却看到"未找到地图数据"。
        if not old.pkg then
            M.err_kind, M.err = "nofile", "包内没有概览层"
        end
        return false
    end
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
        app.dlg.toast("GPS 未定位", 900)
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

-- 按下 → 抬起 时判定 (短按 = 触发, 长按 = 删除)
-- ★ 2026-10-27 linit: 没有地图数据时**不再整屏拦死** —— 打开应用照样落卫星页看 GPS;
--   只有用户**主动切回"地图"页**时才弹二级弹窗提醒下载(确定=下载, 取消=留在地图页看错误提示)。
local function ask_download_if_no_map()
    if M.map_ok then return end
    M.asking = "dlmap"
    app.dlg.confirm("当前没有地图数据, 是否联网下载?")
end

local function press_at(px, py, held)
    -- 左侧 5 键: 地图 / 行程 / 卫星 / 设置 / 收藏夹
    -- (必须在"设置页按行"之前判, 否则进了设置页就点不到"地图"出不来)
    for i = 1, LB_N do
        if hit_rect(px, py, LB_X, lb_y(i), LB_W, LB_H) then
            if i == 1 and M.page == 1 then
                -- ★ 2026-10-27 linit: 短按 = 回 GPS 地球/卫星数据页; 长按 = 普通/等高地图
                if held >= 700 then
                    M.cont_on = not M.cont_on
                    app.dlg.toast(M.cont_on and "等高地图" or "普通地图", 800)
                else
                    M.page = 3
                end
            else
                M.page = i
                M.set_cat = 0          -- 切换页面时设置页回到一级
                open_drawer(false)
                if i == 1 then
                    -- 进地图页: 先定位到当前位置(有定位才动), 没地图数据就提醒下载
                    if M.fix and M.fix.valid then recenter() end
                    ask_download_if_no_map()
                end
            end
            app.redraw()
            return
        end
    end
    if py >= 275 then                         -- ★ 底栏两键 (全页共用; 275 = 盖子线 y)
        if M.page == 5 then                   -- ★ B10: 左回设置 / 右「切换」
            if px < W / 2 then M.page = 4 else pkg_switch_ask() end
        elseif M.page == 4 then
            if px < W / 2 then M.page = 2 else M.page = 3 end     -- 行程 / GPS
        elseif M.page == 2 then
            if px < W / 2 then M.page = 4 else M.page = 3 end     -- 设置 / GPS
        elseif M.page == 3 then
            if px < W / 2 then M.page = 4                          -- 设置
            else M.page = 1; ask_download_if_no_map() end          -- 地图 (没数据就提醒下载)
        else
            if px < W / 2 then M.page = 4 else M.page = 3 end     -- 设置 / GPS
        end
        app.redraw()
        return
    end
    if M.page == 5 then                       -- ★ B10 地图包管理: 只认行, 没有其它控件
        pkg_press(py, held)
        return
    end
    if M.page == 4 then                       -- 设置页: 二级菜单
        local rows = set_cur_rows()
        if not rows then
            -- 一级: 点分类 → 进二级
            for i = 1, #SET_GROUPS do
                local y = set_row_y(i)
                if py >= y and py < y + SET_H then
                    M.set_cat = i
                    app.redraw()
                    return
                end
            end
            return
        end
        -- 二级: 点具体项执行; 点最后一行"返回"回一级
        for k = 1, #rows do
            local y = set_row_y(k)
            if py >= y and py < y + SET_H then
                SET_ROWS[rows[k]][2]()
                app.redraw()
                return
            end
        end
        local yb = set_row_y(#rows + 1)
        if py >= yb and py < yb + SET_H then
            M.set_cat = 0
            app.redraw()
        end
        return
    end
    if M.page == 3 then return end             -- 卫星页其余区域无控件 (底栏已在上面处理)
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

-- ============================================================================
-- B10 地图包管理 (page 5)
-- ============================================================================
-- 三个操作: 点按行=选中 / 底栏「切换」=换包 / 长按行=删除。
-- ⛔ 删除只用**精确路径**(清单里的 jsonpath/binpath, 或老包的三个固定文件名),
--   绝不按文件名前缀匹配 —— 那会误删别的包或用户的 gpx。
-- ⚠ 全局函数: 顶层 local 已顶到 200 上限。
function pkg_size_text(p)
    local b = 0
    if p.tiled then
        b = p.bytes or 0
        if b <= 0 and p.binpath then b = app.fs.size(p.binpath) or 0 end
    elseif p.dir then
        b = (app.fs.size(p.dir .. "/" .. LEGACY_ROAD) or 0)
          + (app.fs.size(p.dir .. "/" .. LEGACY_CONTOUR) or 0)
          + (app.fs.size(p.dir .. "/" .. LEGACY_POI) or 0)
    end
    if b >= 1048576 then return string.format("%.1fMB", b / 1048576) end
    return string.format("%dK", math.floor(b / 1024 + 0.5))
end

-- 完整性: 切换前先验, 不合格就不动当前地图
function pkg_ok(p)
    if p.tiled then
        if not (p.jsonpath and p.binpath) then return false end
        if not app.fs.exists(p.jsonpath) then return false end
        local sz = app.fs.size(p.binpath)
        if not sz or sz <= 0 then return false end
        if (p.bytes or 0) > 0 and sz ~= p.bytes then return false end
        return true
    end
    if not p.dir then return false end
    return app.fs.exists(p.dir .. "/" .. LEGACY_ROAD) and true or false
end

-- 该包**确切**关联的文件 (不扫目录、不拼通配)
function pkg_files(p)
    local out = {}
    if p.tiled then
        if p.jsonpath then out[#out + 1] = p.jsonpath end
        if p.binpath then out[#out + 1] = p.binpath end
    elseif p.dir then
        out[#out + 1] = p.dir .. "/" .. LEGACY_ROAD
        out[#out + 1] = p.dir .. "/" .. LEGACY_CONTOUR
        out[#out + 1] = p.dir .. "/" .. LEGACY_POI
    end
    return out
end

function page_pkg()
    local ps = M.pkgs or {}
    local n = #ps
    app.mix(PX, MAP_T + 6, "地图包", false)
    local cnt = tostring(n) .. " 个"
    app.mix(W - 14 - app.mixw(cnt), MAP_T + 6, cnt, false)
    app.hline(4, W - 5, MAP_T + 26, BK)
    if n == 0 then
        app.mixc(W / 2, MAP_T + 70, "还没有地图包 (从商店或 TF 卡装)", false)
        return
    end
    local sel = M.pm_sel or 1
    if sel > n then sel = n; M.pm_sel = n end
    if sel < 1 then sel = 1; M.pm_sel = 1 end
    local top = M.pm_top or 1
    if sel < top then top = sel; M.pm_top = top end
    if sel > top + 3 then top = sel - 3; M.pm_top = top end
    if top < 1 then top = 1; M.pm_top = 1 end
    for k = 1, 4 do
        local i = top + k - 1
        if i > n then break end
        local p = ps[i]
        local y = MAP_T + 34 + (k - 1) * SET_H
        local on = (i == sel)
        if on then app.fill(4, y - 2, W - 5, y + SET_H - 5, BK) end
        local nm = p.name or p.id or "?"
        local sz = pkg_size_text(p)
        if app.mixw(nm) + app.mixw(sz) + 20 <= 320 then
            app.mix(W - 14 - app.mixw(sz), y + 3, sz, on)   -- 名字太长就只留名字, 不截断(截断会切断汉字)
        end
        local used = (M.pkg and M.pkg.id == p.id) and "* " or "  "
        app.mix(PX, y + 3, used .. nm, on)
    end
    if n > 4 then
        app.textsb(W - 40, MAP_T + 8, tostring(top) .. "-" .. tostring(math.min(n, top + 3)), false)
    end
    -- 选中包详情
    local p = ps[sel]
    if not p then return end
    app.hline(PX, W - 5, 176, BK)
    local l1, l2
    if p.tiled then
        -- ⚠ json 的 num 回来的是 float (610.0) ⇒ 先 floor 再 tostring, 否则显示 "610.0 块"
        l1 = "分块 " .. tostring(math.floor((p.count or 0) + 0.5)) .. " 块  " .. pkg_size_text(p)
        -- ⚠ 分块包的图层按编译默认产出写 (map_prov 默认含地标); 用 --no-poi 编的包会显示不准
        l2 = "图层 道路 地标"
    else
        l1 = "单文件  " .. pkg_size_text(p)
        l2 = "图层 道路"
        if p.dir then
            if app.fs.exists(p.dir .. "/" .. LEGACY_CONTOUR) then l2 = l2 .. " 等高线" end
            if app.fs.exists(p.dir .. "/" .. LEGACY_POI) then l2 = l2 .. " 地标" end
        end
    end
    app.mix(PX, 182, l1, false)
    app.mix(PX, 200, l2, false)
    if M.pkg and M.pkg.id == p.id then
        app.mix(PX, 218, "正在使用", false)
    else
        app.mix(PX, 218, "长按行 = 删除", false)
    end
end

-- 点按行 = 选中; 长按行 = 删除 (与收藏抽屉同一手感)
function pkg_press(py, held)
    local ps = M.pkgs or {}
    local n = #ps
    if n == 0 then return end
    local top = M.pm_top or 1
    for k = 1, 4 do
        local i = top + k - 1
        if i > n then break end
        local y = MAP_T + 34 + (k - 1) * SET_H
        if py >= y - 2 and py < y + SET_H - 2 then
            M.pm_sel = i
            if held >= 700 then
                local p = ps[i]
                if M.pkg and M.pkg.id == p.id then
                    app.dlg.toast("正在使用的包不能删除", 1500)
                else
                    M.pm_want = p.id
                    M.asking = "pkgdel"
                    app.dlg.confirm("删除 " .. (p.name or p.id) .. " ?")
                end
            end
            app.redraw()
            return
        end
    end
end

function pkg_switch_ask()
    local ps = M.pkgs or {}
    local p = ps[M.pm_sel or 1]
    if not p then return end
    if M.pkg and M.pkg.id == p.id then
        app.dlg.toast("已经是当前包", 1000)
        return
    end
    if not pkg_ok(p) then
        app.dlg.toast("包不完整, 未切换", 1500)
        return
    end
    M.pm_want = p.id
    M.asking = "pkgsw"
    app.dlg.confirm("切换到 " .. (p.name or p.id) .. " ?")
end

function pkg_delete(id)
    local ps = M.pkgs or {}
    local p
    for i = 1, #ps do
        if ps[i].id == id then p = ps[i]; break end
    end
    if not p then return end
    if M.pkg and M.pkg.id == p.id then
        app.dlg.toast("正在使用的包不能删除", 1500)
        return
    end
    local fl = pkg_files(p)
    local ok, bad = 0, 0
    for i = 1, #fl do
        if app.fs.exists(fl[i]) then
            if app.fs.remove(fl[i]) then ok = ok + 1 else bad = bad + 1 end
        end
    end
    M.pm_sel, M.pm_top = 1, 1
    map_reload(true)                      -- 重扫 (keepview: 别把视口挪走)
    if bad > 0 then
        app.dlg.toast(string.format("有 %d 个文件删不掉", bad), 1600)
    else
        app.dlg.toast(string.format("已删除 %d 个文件", ok), 1600)
    end
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
            elseif what == "dlmap" and r == 0 then
                dl_start()                        -- 二级弹窗点"确定" → 起联网下载
            elseif what == "track" and r and r >= 1 then
                local fn = D.tracks and D.tracks[r]
                if fn then track_open(fn) end     -- ★ B2: 列表下标 → 载入并进查看态
            elseif what == "pkgsw" and r == 0 and M.pm_want then
                M.pkg_switch = M.pm_want          -- ★ B10: 走既有的换包通道 (下一个 poll 执行)
                M.pm_want = nil
            elseif what == "pkgdel" and r == 0 and M.pm_want then
                local id = M.pm_want              -- ★ B10: 确认后才删, 只删该包的精确文件
                M.pm_want = nil
                pkg_delete(id)
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
        -- ★ 2026-10-27: 卫星数据**不再只在卫星页取** —— 否则刚切进卫星页第一帧是空的,
        --   再叠上"空就演示"的旧逻辑, 就表现为"卫星状态乱跳"。这里全页面通用, 500ms 节流
        --   (卫星方位本来就是秒级变化, 每帧深拷 40 项纯浪费)。
        if app.gps.sats then
            local tfetch = M.sat_fetch_ms or 0
            if now - tfetch >= 500 then
                M.sat_fetch_ms = now
                M.sats = app.gps.sats()
            end
        end
    end
    trip_step(M.fix, dt)

    -- 卫星页: 地球自转 + 轮播条都要连续重绘。70ms 一帧 ⇒ ~14fps (面板 16Hz 封顶)
    if M.page == 3 then
        local last = M.sat_anim or 0
        -- 间隔 = 实测渲染耗时 + 6ms (下限 62ms: 面板本身 ~16Hz)。这样上一帧的
        -- 异步 DMA 一定已经读完, 不会再出现"只画了半个地球"的花屏
        local gap = (M.sat_cost or 20) + 6
        if gap < 62 then gap = 62 end
        if now - last >= gap then M.sat_anim = now; app.redraw() end
    end

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
        -- ★ B10: NVS 只在**换成功之后**才记 —— 以前是先写后换, 换失败会把下次开机的默认包带偏
        if map_reload(false) then
            if app.nvs and app.nvs.set_str then app.nvs.set_str("mappkg", want) end
            for i = 1, #(M.pkgs or {}) do
                if M.pkgs[i] == M.pkg then M.pkgi = i end
            end
            app.dlg.toast("地图包: " .. (M.pkg and M.pkg.name or want), 1200)
            app.redraw()
        else
            app.dlg.toast("这个包装不起来, 已保留原图", 1800)
            app.redraw()
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
        -- ★ B2: 历史轨迹查看态优先退出 (回到实时地图, 视口不动)
        if M.hist then
            M.hist = nil
            app.dlg.toast("回到实时地图", 900)
            app.redraw()
            return true
        end
        if M.dr_on then open_drawer(false); return true end
        if M.page == 5 then M.page = 4; app.redraw(); return true end   -- ★ B10: 包管理 → 设置
        if M.page == 4 then
            -- ★ 2026-10-27: 设置页分两级 ⇒ 返回键先回上一级, 一级再按才回地图
            if (M.set_cat or 0) > 0 then M.set_cat = 0; app.redraw(); return true end
            M.page = 1; app.redraw(); return true            -- 设置页先回地图
        end
        return false                                    -- 交内核弹「退出程序?」
    elseif a == A_LEFT or a == A_RIGHT then
        -- 页数 = 左列按键数 (LB_LABEL)。⛔ 原来这里写 `#PAGES`, 而 PAGES 在删掉顶部
        -- 页签时就没了 —— 悬空引用, 一按左右键就崩 "attempt to get length of a nil value"。
        local np = #LB_LABEL
        -- ★ B10: page 5 不在 LB_LABEL 里, 先归位到设置页, 否则 5%4+1=2 会跳到行程页
        if M.page == 5 then M.page = 4 end
        M.page = a == A_RIGHT and (M.page % np) + 1 or ((M.page + np - 2) % np) + 1
        app.redraw()
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
    -- ★ 2026-10-24: 没有卡/没有地图包 **不再直接退出应用** —— 卫星信息页不依赖地图数据,
    --   进不去就等于"GPS 用不了"。这里改成: 记下错误、仍起 GPS, 并直接落到卫星页。
    local map_ok = true
    if not app.fs.exists(TIER0) then
        M.err_kind = "notf"
        M.err = "请插入 TF 卡后重新打开"
        map_ok = false
    elseif not map_reload(false) then
        M.err_kind = "nofile"
        M.err = app.fs.exists(MAP_DIRS[1]) and "Map 文件夹里没有地图包"
                                            or "TF 卡上没有 Map 文件夹"
        map_ok = false
    end
    -- ★ B9: 图层开关必须在 map_reload **之后**恢复 —— load_map() 结尾会把
    --   M.cont_on 复位成 false (默认看普通图), 放在前面会被它覆盖。
    --   掩码与 lay_save() 严格对应; 没有这个键(首次运行)= 保持源码默认值。
    if map_ok and app.nvs and app.nvs.get_int then
        local v = app.nvs.get_int("lay")
        if v and v >= 0 and v <= 15 then
            M.road_on = (v % 2) == 1
            M.cont_on = (v // 2) % 2 == 1
            M.poi_on  = (v // 4) % 2 == 1
            M.labels  = (v // 8) % 2 == 1
        end
    end
    if app.gps and app.gps.start then M.gps_ok = app.gps.start() end
    M.map_ok = map_ok
    -- ★ 2026-10-27 linit: 有地图包 ⇒ **默认落地图页**(并定位到当前位置);
    --   没有地图包 ⇒ 先落地球/卫星页(它不依赖地图数据, 至少 GPS 状态能看)。
    --   第 1 键随后变成"GPS", 点一下就回到地球/卫星数据界面。
    M.page = map_ok and 1 or 3
    if map_ok and M.fix and M.fix.valid then recenter() end
end

function on_exit()
    if M.gps_ok and app.gps and app.gps.stop then app.gps.stop(); M.gps_ok = false end
    M.road, M.contour, M.ov, M.poi = nil, nil, nil, nil
    M.tiles, M.torder = nil, nil
    M.tbytes = 0
    if D.st ~= "idle" and app.net and app.net.fetch_poll then app.net.fetch_poll() end
    D.st = "idle"
end
