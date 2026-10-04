--@lapp v1
--@app name=航班雷达
--@app cat=运维
--@app desc=自动定位当前位置, 扫描附近空域航班: 距离/方位/高度/地速/呼号
--@app need=wifi
--@app version=1.0
--@app icon=AAAAAAAAAAAAAAAB4AAAAAAAAAPwAAAAAAAAA/AAAAAAAAAD8AAAAAAAAAPwAAAAB4AAAeAAAeAPwAAAwAAD8A/AAAf4AAPwD8AAD/4AA/APwAAX/gAD8AeAABf+gAHgAwAA1/5gAMAf4A8X/hgH+D/48Af8Bg/+X/sAAEABl/5f+AAAQABX/l/4AABADxf+X/geAIAfl/4f8D8AgB+H/AAAPwCAH4AgAQA/AIAfgEABAD8AgA8AQACAHgCABgCAAIAMAQA/wIAAQH+BAH/xAABA/+EAv/EAACF/4QC/8gAAIX/hAL/0AAAhf+EAv/QAABF/4gA/6AAAEH/CAAIIAAAIBAIADBAAAAgEAgAwEAAAAAQAAEAgAAAf//////gAACqqqqqqrAAAVVVVVVVWAACqqqqqqqsAAVVVVVVVVYACqqqqqqqqwAVVVVVVVVVgD/////////Af////////+DgAAAAAAAAccAAAAAAAAA5hmZmZmZmZhmGZmZmZmZmGYAAAAAAAAAZiCQSCQSCQRmAAAAAAAAAGYfD4fD4fD4ZjGYzGYzGYxmIJBIJBIJBGYgkEgkEgkEZiqVSqVSqVRmP5/P5/P5/GMAAAAAAAAAwf////////+A/////////wB+AAAAAAB8AH4AAAAAAHwAAAAAAAAAAAAAAAAAAAAAA=
-- 图标自绘: blit_app_icon(x, y[, inv=true])
-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "AAAAAAAAAAAAAAAB4AAAAAAAAAPwAAAAAAAAA/AAAAAAAAAD8AAAAAAAAAPwAAAAB4AAAeAAAeAPwAAAwAAD8A/AAAf4AAPwD8AAD/4AA/APwAAX/gAD8AeAABf+gAHgAwAA1/5gAMAf4A8X/hgH+D/48Af8Bg/+X/sAAEABl/5f+AAAQABX/l/4AABADxf+X/geAIAfl/4f8D8AgB+H/AAAPwCAH4AgAQA/AIAfgEABAD8AgA8AQACAHgCABgCAAIAMAQA/wIAAQH+BAH/xAABA/+EAv/EAACF/4QC/8gAAIX/hAL/0AAAhf+EAv/QAABF/4gA/6AAAEH/CAAIIAAAIBAIADBAAAAgEAgAwEAAAAAQAAEAgAAAf//////gAACqqqqqqrAAAVVVVVVVWAACqqqqqqqsAAVVVVVVVVYACqqqqqqqqwAVVVVVVVVVgD/////////Af////////+DgAAAAAAAAccAAAAAAAAA5hmZmZmZmZhmGZmZmZmZmGYAAAAAAAAAZiCQSCQSCQRmAAAAAAAAAGYfD4fD4fD4ZjGYzGYzGYxmIJBIJBIJBGYgkEgkEgkEZiqVSqVSqVRmP5/P5/P5/GMAAAAAAAAAwf////////+A/////////wB+AAAAAAB8AH4AAAAAAHwAAAAAAAAAAAAAAAAAAAAAA="
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
-- planeradar.lua — 附近航班雷达 (Lua 单文件应用)
--
-- 数据: adsb.fi 开放数据 API (免 key, 公开接口限流 1 次/秒)
--       一次返回以站点为中心 N 海里内的全部 ADS-B 目标, 且**已带 dst(距离 NM)
--       / dir(方位角)**, 不需要自己算球面距离。
--
-- 位置来源 (三层, 2026-10-02 linit 要求"根据 Wi-Fi 获取当前位置"):
--   ① 自动定位  —— 联网后用 IP 定位接口拿经纬度 (连的就是 Wi-Fi, 所以这就是"从网络定位")
--   ② 预设城市  —— IP 定位只能到城市级, 且常偏到运营商出口城市 → 可就近手选修正
--   ③ 经纬度微调 —— 完全手动: 上下调纬度 / 左右调经度 / 确认切步长
--
-- 为什么不用自己收: ESP32-S3 的 2.4GHz 射频收不到 1090MHz ADS-B 原始广播
-- (要外接专用接收机), 所以取"社区接收网络已汇总的数据"。
--
-- 契约: on_enter / render / poll / action / on_exit
-- 按键: 上/下=选航班  左/右=距离档  确认=详情(长按=位置设置)  返回=关详情/退出
-- ============================================================================

local W = app.W or 400
local H = app.H or 300

-- 动作常量带数值兜底 (桥常量万一取不到, 应用照常能用)
local A_UP    = (type(app.UP)      == "number") and app.UP      or 1
local A_DOWN  = (type(app.DOWN)    == "number") and app.DOWN    or 2
local A_LEFT  = (type(app.LEFT)    == "number") and app.LEFT    or 3
local A_RIGHT = (type(app.RIGHT)   == "number") and app.RIGHT   or 4
local A_OK    = (type(app.CONFIRM) == "number") and app.CONFIRM or 5
local A_BACK  = (type(app.BACK)    == "number") and app.BACK    or 6
local BK, WH  = 0, 1

-- ---- 预设城市 (IP 定位偏了就用它就近修正) ----
local PRESETS = {
    { "莆田", 25.4310, 119.0078 },
    { "福州", 26.0745, 119.2965 },
    { "厦门", 24.4798, 118.0894 },
    { "泉州", 24.8741, 118.6757 },
    { "广州", 23.3924, 113.2988 },
    { "上海", 31.1443, 121.8083 },
    { "北京", 40.0799, 116.6031 },
}
-- 位置来源编号: 1 = 自动定位(IP); 2..(#PRESETS+1) = 预设城市; #PRESETS+2 = 手动微调
local SRC_AUTO   = 1
local SRC_MANUAL = #PRESETS + 2
local SRC_COUNT  = #PRESETS + 2

-- 兜底位置 (定位全失败时用): 福建莆田湄洲岛
local FALLBACK = { name = "湄洲岛", lat = 25.0436, lon = 119.1186 }

local RANGES = { 25, 50, 100, 200 }        -- 海里
local REFRESH_MS = 8000                    -- 航班自动刷新间隔 (接口限流 1/s, 8s 很宽裕)
local MAX_AC     = 160
local FIELDS = "hex,flight,r,t,lat,lon,alt_baro,alt_geom,gs,track,baro_rate,"
            .. "squawk,dst,dir,rssi,ownOp,emergency"

-- IP 定位接口 (实测国内均可达)。**首选 ip-api**:
--   ① 带 lang=zh-CN → 直接返回中文城市名 (ip.sb 只给英文 "Putian")
--   ② 走 **HTTP** → 不需要 TLS 握手, 既不占那 16KB 内部连续块, 也避开证书/时间问题
--   (只查"我在哪个城市", 不含任何隐私数据)
-- 兜底 ip.sb (HTTPS), 给 latitude/longitude 字段。
local GEO_URLS = {
    "http://ip-api.com/json/?lang=zh-CN",
    "https://api.ip.sb/geoip",
}

-- ---- 版面 ----
local BAR_H     = 16
local CX, CY    = 125, 130
local CR        = 100
local LIST_X    = 252
local INFO_Y    = 240
local LIST_ROWS = 10
local ROW_H     = 22

-- ---- 状态 ----
local G = {
    st     = "idle",     -- idle | loading | ok | err
    ac     = {},
    sel    = 1,
    src    = SRC_AUTO,   -- 位置来源
    lat    = FALLBACK.lat,
    lon    = FALLBACK.lon,
    place  = FALLBACK.name,
    rng    = 3,
    t0     = 0,
    err    = "",
    detail = false,
    menu   = false,      -- 位置设置菜单
    tune   = false,      -- 经纬度微调
    msel   = 1,
    step   = 2,          -- 微调步长索引: 1=0.01 2=0.1 3=1.0
    geo    = "idle",     -- idle | run | done | fail
    geoi   = 1,          -- 正在试的定位接口下标
    geoerr = "",
    ok_t   = 0,          -- 确认键按下时刻 (判长按)
    ok_pend= false,
    ok_long= false,
    dirty  = true,       -- 需要重绘 (没变化就整帧跳过绘制)
}
local STEPS = { 0.01, 0.1, 1.0 }

-- ============================ 小工具 ============================
-- ⚠ Lua 5.3+ 的 string.format("%d", x) 遇到**非整数浮点**会直接抛错 → 一律先取整
local function i0(v)
    v = tonumber(v)
    if not v then return 0 end
    return math.floor(v + 0.5)
end
local function trim(s)
    s = tostring(s or "")
    s = s:gsub("^[%s]+", "")
    s = s:gsub("[%s]+$", "")
    return s
end
local function pad_l(s, n)
    s = tostring(s)
    if #s >= n then return s:sub(1, n) end
    return string.rep(" ", n - #s) .. s
end
local function pad_r(s, n)
    s = tostring(s)
    if #s >= n then return s:sub(1, n) end
    return s .. string.rep(" ", n - #s)
end
local function alt_txt(a)
    local v = a.alt_baro
    if v == "ground" then return "GND" end
    if type(v) ~= "number" then v = a.alt_geom end
    if type(v) ~= "number" then return "--" end
    return string.format("%d", i0(v))
end
local function alt_short(a)
    local v = a.alt_baro
    if v == "ground" then return "GND" end
    if type(v) ~= "number" then v = a.alt_geom end
    if type(v) ~= "number" then return "  -" end
    local f = i0(v) / 100
    if f < 0 then f = 0 end
    if f > 999 then f = 999 end
    return string.format("%3d", f)
end
local function fnum(v, fmt, suf)
    if type(v) ~= "number" then return "--" end
    return string.format(fmt, v) .. (suf or "")
end

-- ============================ 雷达基准图 ============================
local RINGPTS = {}
local function build_rings()
    if RINGPTS[1] then return end
    local radii = { CR * 0.25, CR * 0.5, CR * 0.75, CR }
    for ri = 1, #radii do
        local r = radii[ri]
        local n = math.max(32, math.floor(r * 4))
        local t = {}
        for i = 0, n - 1 do
            local a = i * 2 * math.pi / n
            t[#t + 1] = math.floor(CX + r * math.sin(a) + 0.5)
            t[#t + 1] = math.floor(CY - r * math.cos(a) + 0.5)
        end
        RINGPTS[ri] = t
    end
end
local function blip_xy(a, range)
    local d = a.dst
    if type(d) ~= "number" or type(a.dir) ~= "number" then return nil end
    local r = d / range * CR
    if r > CR then r = CR end
    local th = a.dir * math.pi / 180
    return math.floor(CX + r * math.sin(th) + 0.5), math.floor(CY - r * math.cos(th) + 0.5)
end

-- ============================ 位置 (NVS + IP 定位) ============================
local function place_of_src()
    if G.src == SRC_AUTO then
        return (G.place ~= "") and G.place or "自动"
    elseif G.src == SRC_MANUAL then
        return "自定义"
    end
    return PRESETS[G.src - 1][1]
end
local function pos_save()
    app.nvs.set_int("pr_src", G.src)
    app.nvs.set_int("pr_lat", i0(G.lat * 10000))     -- 32 位整数存 ×10000, 精度 0.0001 度
    app.nvs.set_int("pr_lon", i0(G.lon * 10000))
    app.nvs.set_int("pr_rng", G.rng)
    app.nvs.set_str("pr_place", G.place)
end
local function pos_load()
    local s  = app.nvs.get_int("pr_src")
    local la = app.nvs.get_int("pr_lat")
    local lo = app.nvs.get_int("pr_lon")
    local nm = app.nvs.get_str("pr_place")
    local r  = app.nvs.get_int("pr_rng")
    if type(s) == "number" and s >= 1 and s <= SRC_COUNT then G.src = s end
    if type(la) == "number" and la ~= 0 then G.lat = la / 10000 end
    if type(lo) == "number" and lo ~= 0 then G.lon = lo / 10000 end
    if type(nm) == "string" and nm ~= "" then G.place = trim(nm) end
    if type(r) == "number" and r >= 1 and r <= #RANGES then G.rng = r end
end
-- 预设/微调: 立刻把经纬度落到实处并落盘
local function pos_apply_src()
    if G.src >= 2 and G.src <= #PRESETS + 1 then
        local p = PRESETS[G.src - 1]
        G.lat, G.lon, G.place = p[2], p[3], p[1]
    end
    pos_save()
end

-- ============================ 取数 ============================
local function flights_url()
    return string.format("https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d",
                         G.lat, G.lon, RANGES[G.rng])
end
local function flights_start()
    if G.st == "loading" then return end
    if not app.net.is_connected() then
        G.st = "err"
        G.err = "未联网"
        return
    end
    G.st = "loading"
    G.t0 = app.millis()
    if not app.net.fetch_start(flights_url(), 9000) then
        G.st = "err"
        G.err = "请求忙"
    end
end
-- 依次试 GEO_URLS (app.net 同时只允许一个请求, 所以串行)
local function geo_start()
    if G.geo == "run" then return end
    if not app.net.is_connected() then
        G.geo = "fail"
        G.geoerr = "未联网"
        return
    end
    G.geo = "run"
    -- 自愈: 上次哪个接口成功过就从它开始 (ip-api 偶发失败, 不要每次都白等一轮)
    local gi = app.nvs.get_int("pr_geoi")
    if type(gi) ~= "number" or gi < 1 or gi > #GEO_URLS then gi = 1 end
    G.geoi = gi
    if not app.net.fetch_start(GEO_URLS[G.geoi], 8000) then
        G.geo = "fail"
        G.geoerr = "请求忙"
    end
end
local function geo_parse(body)
    -- 首选接口: latitude/longitude; 兜底接口: lat/lon。用 -999 区分"缺失"与"真的 0"
    local la = app.json.num(body, "latitude", -999)
    local lo = app.json.num(body, "longitude", -999)
    if la == -999 then la = app.json.num(body, "lat", -999) end
    if lo == -999 then lo = app.json.num(body, "lon", -999) end
    if la == -999 or lo == -999 or la == 0 or lo == 0 then return nil end
    local nm = app.json.str(body, "city")
    if not nm or nm == "" then nm = app.json.str(body, "regionName") end
    return la, lo, trim(nm)
end

local function apply_rows(rows)
    local list = {}
    for i = 1, #rows do
        local a = rows[i]
        if type(a.dst) == "number" then
            a.flight = trim(a.flight)
            a.ownOp  = trim(a.ownOp)
            a.r      = trim(a.r)
            a.t      = trim(a.t)
            a.hex    = trim(a.hex)
            a.squawk = trim(a.squawk)
            if a.flight == "" then
                a.flight = (a.r ~= "") and a.r or ((a.hex ~= "") and a.hex or "?")
            end
            list[#list + 1] = a
        end
    end
    table.sort(list, function(x, y) return x.dst < y.dst end)
    G.ac = list
    if G.sel > #list then G.sel = #list end
    if G.sel < 1 then G.sel = 1 end
end

-- ============================ 绘制 ============================
local function draw_bar()
    app.fill(0, 0, W - 1, BAR_H - 1, BK)
    local left = "航班 " .. place_of_src()
    app.mix(4, 0, left, true)
    local right
    if G.geo == "run" then
        right = "定位中"
    elseif G.st == "loading" then
        right = "刷新中"
    elseif G.st == "err" then
        right = "离线"
    else
        right = string.format("%d架 %dNM", #G.ac, RANGES[G.rng])
    end
    app.mix(W - 6 - app.mixw(right), 0, right, true)
    app.hline(0, W - 1, BAR_H, BK)
end

local function draw_radar(range)
    for ri = 1, 4 do
        local t = RINGPTS[ri]
        for i = 1, #t, 2 do app.px(t[i], t[i + 1], BK) end
    end
    for k = 0, 7 do
        local a = k * math.pi / 4
        local s, c = math.sin(a), math.cos(a)
        local f = 0.82
        while f <= 1.001 do
            app.px(math.floor(CX + CR * f * s + 0.5), math.floor(CY - CR * f * c + 0.5), BK)
            f = f + 0.045
        end
    end
    app.texts(CX - 4, CY - CR + 1, "N")
    app.texts(CX + CR - 11, CY - 4, "E")
    app.texts(CX - 4, CY + CR - 9, "S")
    app.texts(CX - CR + 3, CY - 4, "W")
    app.fill(CX - 2, CY - 2, CX + 2, CY + 2, BK)

    for i = 1, #G.ac do
        local a = G.ac[i]
        local x, y = blip_xy(a, range)
        if x then
            if i == G.sel then
                app.fill(x - 2, y - 2, x + 2, y + 2, BK)
                local tr = (type(a.track) == "number") and a.track or a.dir
                local t = tr * math.pi / 180
                local f = 4
                while f <= 9 do
                    app.px(math.floor(x + f * math.sin(t) + 0.5),
                           math.floor(y - f * math.cos(t) + 0.5), BK)
                    f = f + 1
                end
                local lx = x + 6
                if lx + #a.flight * 8 > LIST_X - 8 then lx = x - 6 - #a.flight * 8 end
                if lx < 2 then lx = 2 end
                local ly = (y - 12 >= BAR_H + 2) and (y - 12) or (y + 4)
                app.texts(lx, ly, a.flight)
            elseif a.alt_baro == "ground" then
                app.rect(x - 2, y - 2, x + 2, y + 2, BK)
            else
                app.fill(x - 1, y - 1, x + 1, y + 1, BK)
            end
        end
    end
end

local function draw_list()
    app.vline(LIST_X - 4, BAR_H + 1, INFO_Y - 2, BK)
    local n = #G.ac
    if n > LIST_ROWS then n = LIST_ROWS end
    local y = BAR_H + 3
    for i = 1, n do
        local a = G.ac[i]
        local sel = (i == G.sel)
        local line = pad_r(a.flight:sub(1, 7), 7)
                     .. pad_l(string.format("%.1f", a.dst), 5)
                     .. " " .. alt_short(a)
        if sel then app.fill(LIST_X - 2, y - 2, W - 2, y + 11, BK) end
        app.texts(LIST_X, y, line, sel)
        y = y + ROW_H
    end
end

local function draw_info()
    app.hline(0, W - 1, INFO_Y - 1, BK)
    local a = G.ac[G.sel]
    if a then
        app.mix(4, INFO_Y + 2,
                string.format("%s  %s  %s", a.flight, (a.t ~= "" and a.t or "--"),
                              (a.r ~= "" and a.r or "")))
        app.mix(4, INFO_Y + 21,
                string.format("距 %.1fNM  方位 %d  高 %sft", a.dst, i0(a.dir), alt_txt(a)))
        app.mix(4, INFO_Y + 40,
                string.format("地速 %s  升降 %s  信号 %s", fnum(a.gs, "%.0f", "kt"),
                              fnum(a.baro_rate, "%.0f", "fpm"), fnum(a.rssi, "%.0f", "")))
    else
        local msg
        if G.geo == "run" then
            msg = "正在根据网络定位所在位置..."
        elseif G.geo == "fail" then
            msg = "定位失败 (" .. G.geoerr .. ")  长按确认键改位置"
        elseif G.err == "未联网" then
            msg = "未联网: 返回后重进本应用会弹出连接 WiFi"
        elseif G.st == "err" then
            msg = "取数失败 (" .. G.err .. ")  按确认重试"
        elseif G.st == "loading" then
            msg = "正在获取航班数据..."
        else
            msg = "附近空域暂无目标  按确认立即刷新"
        end
        app.mix(4, INFO_Y + 20, msg)
    end
end

local function draw_detail()
    local a = G.ac[G.sel]
    if not a then return end
    local x0, y0, x1, y1 = 36, 38, W - 36, H - 38
    app.fill(x0, y0, x1, y1, WH)
    app.rect(x0, y0, x1, y1, BK)
    app.rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, BK)
    app.fill(x0 + 3, y0 + 3, x1 - 3, y0 + 21, BK)
    app.mix(x0 + 10, y0 + 4, "航班详情", true)

    local op = a.ownOp
    if op == "" then op = "--" end
    if #op > 18 then op = op:sub(1, 18) end
    local lines = {
        string.format("呼号 %s   机型 %s", a.flight, (a.t ~= "" and a.t or "--")),
        string.format("注册号 %s   ICAO %s", (a.r ~= "" and a.r or "--"),
                      (a.hex ~= "" and a.hex or "--")),
        string.format("高度 %s ft   升降 %s fpm", alt_txt(a), fnum(a.baro_rate, "%.0f", "")),
        string.format("地速 %s kt   航向 %d", fnum(a.gs, "%.0f", ""), i0(a.track)),
        string.format("应答机 %s   紧急 %s", (a.squawk ~= "" and a.squawk or "--"),
                      (a.emergency and a.emergency ~= "none") and a.emergency or "无"),
        string.format("距本站 %.1f NM   方位 %d", a.dst, i0(a.dir)),
        string.format("本站 %s %.4f/%.4f", place_of_src(), G.lat, G.lon),
        "运营人 " .. op,
    }
    local y = y0 + 28
    for i = 1, #lines do
        app.mix(x0 + 10, y, lines[i])
        y = y + 20
    end
    app.hline(x0 + 6, x1 - 6, y1 - 22, BK)
    app.mix(x0 + 10, y1 - 19, "上下切换航班   返回关闭")
end

-- ---- 位置设置菜单 ----
local function menu_label(i)
    if i == SRC_AUTO then
        return (G.place ~= "") and ("自动定位 " .. G.place) or "自动定位"
    elseif i == SRC_MANUAL then
        return string.format("手动微调 %.4f/%.4f", G.lat, G.lon)
    end
    return PRESETS[i - 1][1]
end
local function draw_menu()
    local x0, y0, x1, y1 = 46, 30, W - 46, H - 30
    app.fill(x0, y0, x1, y1, WH)
    app.rect(x0, y0, x1, y1, BK)
    app.rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, BK)
    app.fill(x0 + 3, y0 + 3, x1 - 3, y0 + 21, BK)
    app.mix(x0 + 10, y0 + 4, "位置来源", true)
    local y = y0 + 28
    for i = 1, SRC_COUNT do
        local sel = (i == G.msel)
        -- ⚠ 选中行必须"整行填黑 + 反白字"。若先铺白底再 inv=true,
        --   反白字是白的、底也是白的 → 文字整行消失 (踩过)。
        if sel then app.fill(x0 + 5, y - 2, x1 - 5, y + 14, BK) end
        app.mix(x0 + 10, y, menu_label(i), sel)
        y = y + 20
    end
    app.hline(x0 + 6, x1 - 6, y1 - 22, BK)
    app.mix(x0 + 10, y1 - 19, "上下选择  确认应用  返回关闭")
end

-- ---- 经纬度微调 ----
local function draw_tune()
    local x0, y0, x1, y1 = 40, 60, W - 40, H - 60
    app.fill(x0, y0, x1, y1, WH)
    app.rect(x0, y0, x1, y1, BK)
    app.rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, BK)
    app.fill(x0 + 3, y0 + 3, x1 - 3, y0 + 21, BK)
    app.mix(x0 + 10, y0 + 4, "经纬度微调", true)
    app.mix(x0 + 12, y0 + 34, string.format("纬度 %.4f", G.lat))
    app.mix(x0 + 12, y0 + 58, string.format("经度 %.4f", G.lon))
    app.mix(x0 + 12, y0 + 82, string.format("步长 %.2f", STEPS[G.step]))
    app.hline(x0 + 6, x1 - 6, y1 - 22, BK)
    app.mix(x0 + 10, y1 - 19, "上下调纬度 左右调经度")
end

function render()
    -- ★ 只在"有变化"时重绘: 框架在 RUN 态强制每帧 needs_redraw, render() 每帧都被调用;
    -- 但本应用是"数据 8s 一次 + 按键驱动", 没必要逐帧重画 ~3000 个点。
    -- 更关键: mbedtls 工作内存改到 PSRAM 后, TLS 握手期间 PSRAM 带宽被抢,
    -- 逐帧重绘会慢两个数量级(实测 836ms/帧) → 画面像卡死。跳过绘制后,
    -- 帧缓冲保留上一帧, flush 照旧(≈0.8ms), CPU 与带宽都省下来。
    if not G.dirty then return end
    G.dirty = false
    build_rings()
    app.clear(WH)
    draw_bar()
    draw_radar(RANGES[G.rng])
    draw_list()
    draw_info()
    if G.detail then draw_detail() end
    if G.menu then draw_menu() end
    if G.tune then draw_tune() end
end

-- ============================ 交互 ============================
local function move_sel(d)
    local n = #G.ac
    if n == 0 then return end
    G.sel = G.sel + d
    if G.sel < 1 then G.sel = n elseif G.sel > n then G.sel = 1 end
end

local function set_rng(d)
    local r = G.rng + d
    if r < 1 then r = #RANGES elseif r > #RANGES then r = 1 end
    if r ~= G.rng then
        G.rng = r
        pos_save()
        G.ac = {}
        G.sel = 1
        flights_start()
    end
end

-- 切来源: 自动 → 重新定位; 预设/自定义 → 直接用坐标
local function src_apply()
    if G.src == SRC_AUTO then
        G.geo = "idle"
        geo_start()
    else
        pos_apply_src()
        G.ac = {}
        G.sel = 1
        flights_start()
    end
end

function action(a)
    G.dirty = true                     -- 任何按键都可能改画面
    if type(a) ~= "number" or a < 1 or a > 7 then return true end

    if G.tune then                       -- 经纬度微调
        local s = STEPS[G.step]
        if a == A_UP then          G.lat = G.lat + s
        elseif a == A_DOWN then    G.lat = G.lat - s
        elseif a == A_LEFT then    G.lon = G.lon - s
        elseif a == A_RIGHT then   G.lon = G.lon + s
        elseif a == A_OK then
            G.step = G.step + 1
            if G.step > #STEPS then G.step = 1 end
        elseif a == A_BACK then
            if G.lat > 90 then G.lat = 90 elseif G.lat < -90 then G.lat = -90 end
            if G.lon > 180 then G.lon = 180 elseif G.lon < -180 then G.lon = -180 end
            G.src = SRC_MANUAL
            G.tune = false
            G.menu = true
            pos_save()
            G.ac = {}
            G.sel = 1
            flights_start()
        end
        return true
    end

    if G.menu then                       -- 位置设置菜单
        if a == A_UP then
            G.msel = G.msel - 1
            if G.msel < 1 then G.msel = SRC_COUNT end
        elseif a == A_DOWN then
            G.msel = G.msel + 1
            if G.msel > SRC_COUNT then G.msel = 1 end
        elseif a == A_OK then
            G.src = G.msel
            if G.src == SRC_MANUAL then
                G.menu = false
                G.tune = true               -- 进微调界面
            else
                G.menu = false
                src_apply()
            end
        elseif a == A_BACK then
            G.menu = false
        end
        return true
    end

    if G.detail then
        if a == A_UP then move_sel(-1)
        elseif a == A_DOWN then move_sel(1)
        elseif a == A_OK or a == A_BACK then G.detail = false end
        return true
    end

    if a == A_UP then        move_sel(-1)
    elseif a == A_DOWN then  move_sel(1)
    elseif a == A_LEFT then  set_rng(-1)
    elseif a == A_RIGHT then set_rng(1)
    elseif a == A_OK then
        -- 只记时刻: 短按=航班详情, 长按=位置设置 (在 poll 里判, 因为 action 只给按下沿)
        G.ok_t = app.millis()
        G.ok_pend = true
    elseif a == A_BACK then  app.exit()
    end
    return true
end

-- ---- 触摸 (当前板子没有触摸芯片; 逻辑保留, 换触屏板即可用) ----
local t_was = false
local function poll_touch()
    local x, y = app.touch()
    if not x then t_was = false; return end
    if t_was then return end
    t_was = true
    G.dirty = true
    if G.tune then return end
    if G.menu then
        local y0 = 30 + 28
        local i = math.floor((y - y0 + 3) / 20) + 1
        if i >= 1 and i <= SRC_COUNT then G.msel = i end
        return
    end
    if G.detail then
        if y > INFO_Y then G.detail = false end
        return
    end
    if y < BAR_H then                       -- 点顶栏 = 位置设置
        G.menu = true
        G.msel = G.src
        return
    end
    if (x - CX) * (x - CX) + (y - CY) * (y - CY) <= (CR + 8) * (CR + 8) then
        local range = RANGES[G.rng]
        local best, bd = nil, 16 * 16
        for i = 1, #G.ac do
            local bx, by = blip_xy(G.ac[i], range)
            if bx then
                local dd = (bx - x) * (bx - x) + (by - y) * (by - y)
                if dd < bd then best, bd = i, dd end
            end
        end
        if best then G.sel = best
        elseif #G.ac > 0 then G.detail = true end
    else
        local i = math.floor((y - BAR_H - 3) / ROW_H) + 1
        if x >= LIST_X - 4 and i >= 1 and i <= #G.ac and i <= LIST_ROWS then
            G.sel = i
            G.detail = true
        elseif y >= INFO_Y and G.st ~= "loading" then
            flights_start()
        end
    end
end

function poll()
    build_rings()
    local now = app.millis()

    -- 确认键: 短按=详情, 长按 0.6s=位置设置
    if G.ok_pend then
        if app.key_down(A_OK) then
            if now - G.ok_t >= 600 then
                G.ok_pend = false
                G.ok_long = true
                G.menu = true
                G.msel = G.src
            end
        else
            G.ok_pend = false
            -- 短按: 有数据→详情; 出错/没数据→**重新取数** (原来这一支漏了, 长得像"按确认没反应")
            if G.st == "err" or #G.ac == 0 then flights_start()
            else G.detail = true end
        end
    elseif G.ok_long and not app.key_down(A_OK) then
        G.ok_long = false
    end

    if G.geo == "run" then
        G.dirty = true
        local st, body, code, nm = app.net.fetch_poll()
        if st == "ok" then
            local la, lo, nm2 = geo_parse(body)
            if la then
                G.lat, G.lon = la, lo
                if nm2 and nm2 ~= "" then G.place = nm2 end
                G.geo = "done"
                G.geoerr = ""
                G.dirty = true
                app.nvs.set_int("pr_geoi", G.geoi)   -- 记住这个接口好用
                pos_save()
                G.ac = {}
                G.sel = 1
                flights_start()
            elseif G.geoi < #GEO_URLS then    -- 换下一个定位接口
                G.geoi = G.geoi + 1
                if not app.net.fetch_start(GEO_URLS[G.geoi], 8000) then
                    G.geo = "fail"; G.geoerr = "请求忙"
                end
            else
                G.geo = "fail"; G.geoerr = "无坐标"
            end
        elseif st == "err" then
            if G.geoi < #GEO_URLS then
                G.geoi = G.geoi + 1
                if not app.net.fetch_start(GEO_URLS[G.geoi], 8000) then
                    G.geo = "fail"; G.geoerr = tostring(code)
                end
            else
                G.geo = "fail"
                G.geoerr = nm or tostring(code)
            end
        end
        poll_touch()
        return                               -- 定位期间独占网络, 不与航班请求并发
    end

    if G.st == "loading" then
        G.dirty = true
        local st, body, code, nm = app.net.fetch_poll()
        G.dirty = true
        if st == "ok" then
            local rows = app.json.pick(body, "aircraft", FIELDS, MAX_AC)
            if type(rows) == "table" and #rows > 0 then apply_rows(rows) end
            G.st = "ok"
            G.err = ""
        elseif st == "err" then
            G.st = "err"
            G.err = nm or tostring(code)
        end
    elseif G.geo == "idle" and G.src == SRC_AUTO then
        geo_start()                          -- 首次进来先定位, 定完再取航班
    elseif now - G.t0 >= REFRESH_MS then
        flights_start()
    end
    poll_touch()
end

function on_enter()
    pos_load()
    build_rings()
    G.st = "idle"
    G.geo = "idle"
    G.geoi = 1
    G.geoerr = ""
    G.ac = {}
    G.sel = 1
    G.detail = false
    G.menu = false
    G.tune = false
    G.msel = G.src
    G.ok_pend = false
    G.ok_long = false
    G.t0 = 0
    G.dirty = true
    if G.src == SRC_AUTO then
        geo_start()                          -- 自动定位: 定完位再取航班
    else
        pos_apply_src()
        flights_start()
    end
end

function on_exit()
    G.detail = false
    G.menu = false
    G.tune = false
    G.ac = {}
end
