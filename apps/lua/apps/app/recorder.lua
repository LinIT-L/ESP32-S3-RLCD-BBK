--@lapp v1
--@app name=录音机
--@app cat=软件
--@app desc=麦克风录音存 TF 卡 (WAV), 可回放、改名、删除
--@app version=1
--@app build=202610300638
--@app icon=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAfgAAAAAAAAH/gAAAAAAAB//gAAAAAAAP//AAAAAAAB//+AAAAAAAH//4AAAAAAA///wAAAAAAD///AAAAAAAf//+AAAAAAB///4AAAAAAH///gAAAAAAf//+AAAAAAB///4AAAAAAH///gAAAAAAf//+AAAAAAB///4AAAAAAH///gAAAAAAf//+AAAAAAB///4AAAAAAH///gAAAAAAf//+AAAAAAB///4AAAAAAH///gAAAAAAf//+AAAAAAB///4AAAAAAD///AAAAAAAP//8AAAAAAAf//gAAAAAAB//+AAAAAAAD//wAAAAAAAH/+AAAAAAAAH/gAAAAAAAAH4AAAAAAAAAAAAAAAAAAAAAAAAAAAD8AAAAPwAAAPwAAAA/AAAA/AAAAD8AAAD+AAAAfwAAAH8AAAD+AAAAf4AAAf4AAAA/wAAD/AAAAB/wAA/4AAAAD/4Af/AAAAAH////4AAAAAP////AAAAAAf///4AAAAAAf//+AAAAAAAf//gAAAAAAAH/gAAAAAAAAH4AAAAAAAAAfgAAAAAAA////8AAAAAD////wAAAAAP////AAAAAA////8AAAAAD////wAAAAAAAAAAAAAAAAAAAAAAAA=
-- 图标自绘: blit_app_icon(x, y[, inv=true])
-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAfgAAAAAAAAH/gAAAAAAAB//gAAAAAAAP//AAAAAAAB//+AAAAAAAH//4AAAAAAA///wAAAAAAD///AAAAAAAf//+AAAAAAB///4AAAAAAH///gAAAAAAf//+AAAAAAB///4AAAAAAH///gAAAAAAf//+AAAAAAB///4AAAAAAH///gAAAAAAf//+AAAAAAB///4AAAAAAH///gAAAAAAf//+AAAAAAB///4AAAAAAH///gAAAAAAf//+AAAAAAB///4AAAAAAD///AAAAAAAP//8AAAAAAAf//gAAAAAAB//+AAAAAAAD//wAAAAAAAH/+AAAAAAAAH/gAAAAAAAAH4AAAAAAAAAAAAAAAAAAAAAAAAAAAD8AAAAPwAAAPwAAAA/AAAA/AAAAD8AAAD+AAAAfwAAAH8AAAD+AAAAf4AAAf4AAAA/wAAD/AAAAB/wAA/4AAAAD/4Af/AAAAAH////4AAAAAP////AAAAAAf///4AAAAAAf//+AAAAAAAf//gAAAAAAAH/gAAAAAAAAH4AAAAAAAAAfgAAAAAAA////8AAAAAD////wAAAAAP////AAAAAA////8AAAAAD////wAAAAAAAAAAAAAAAAAAAAAAAA="
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
-- 录音机 · v2.1 (2026-10-30)
--
-- 底层事实（读 audio_player.c / luat_lib_rec.c 确认，不是推测）：
--   · audio_player_rec_stop() 只置 s_rec_stop 就返回 —— WAV 头回填、fclose、
--     关麦全在录音任务里做（最多一轮 100ms 超时后收尾）。
--   · rec.ms() 是"墙上时间"（xTaskGetTickCount - t0），麦克风没数据它照样涨。
--   · rec.bytes() 是真实已写入的 PCM 字节 → 换算才是真时长。
--   · rec.stop() 后 s_rec_ms 不清零，返回值是残留值，不能当"刚录了多久"。
--   · WAV 播放没有 pause / seek / 变速接口，只有 play / stop。
--   · fs.write 是 fopen("wb") 会截断 —— 不能用它回填/修复 WAV 头。
--
-- v2.1 修正（每条都对应上面的事实）：
--   ① 停止后进入 saving 态，等 active()==false 再读回 44 字节头校验
--      data_len；头没写全绝不提示"已保存"（旧版 stop 后立刻报成功是假报）。
--   ② 录音时长用 bytes 换算；列表时长读 WAV 头实算，不硬编码 64000 B/s。
--   ③ 录音任务自己退出（写卡失败 / 麦掉线）→ 立即校验并明确报错。
--   ④ 头未收尾的文件标 bad，播放前明确提示，不假装能播。
--   ⑤ 删除加二次确认（系统弹窗），改名过滤非法字符。
--   ⑥ 顶栏不再刷掉状态栏 (y<24)。
--   ⑦ 录音中只重画计时/频谱两块（秒级 + 120ms），不再每帧整屏 fill。
--   ⑧ 低电量自动停止并保存（阈值 3420mV = 本项目 3300/4100 标定的 15% 线，连续 2 次确认）。
--   ⑨ 切换播放先等上一条任务真正退出，避免 play() 返回 -1 被当成"打不开"。
-- ============================================================================

local W, H, SB_H = 400, 300, 24
local DIR      = "/sdcard/Rec/"
-- 临时功能(2026-10-30 linit 要求): 列表里同时列出 mp3，方便在同一台机器、同一音量档下
-- 直接对比"音乐"和"录音"的响度差。mp3 只在本地解码播放，不属于录音库。
local MP3_DIRS = { "/sdcard/mp3", "/sdcard/media/mp3" }
local RATE     = 16000
local CH       = 2
local BYTE_RATE = RATE * CH * 2      -- 64000 B/s（16k / 2ch / 16bit）
-- 低电量停录阈值：按本项目自己的电池标定反推，不是拍脑袋的通用值。
--   board_battery.c:111-112  FULL_MV=4100  EMPTY_MV=3300
--   os_mgr.c:197             系统低电告警线 percent < 15
--   → 3300 + (4100-3300)*15% = 3420 mV
-- ⚠ 这是按百分比线性映射反推的，没有实测放电曲线；录音时 I2S+TF 写入的
--   瞬态压降可能让读数短暂跌破该线，所以要求**连续 2 次**采样都低才停
--   （间隔 5s → 至少 10s），阈值本身仍需真机确认。
local BATT_LOW   = 3420
local BATT_NEED  = 2                 -- 连续多少次采样低于阈值才判定
local STALL_MS   = 3000              -- 连续这么久没有新字节 → 停录（原因见 tick_rec）

local A_UP    = (type(app.UP)      == "number") and app.UP      or 1
local A_DOWN  = (type(app.DOWN)    == "number") and app.DOWN    or 2
local A_LEFT  = (type(app.LEFT)    == "number") and app.LEFT    or 3
local A_RIGHT = (type(app.RIGHT)   == "number") and app.RIGHT   or 4
local A_OK    = (type(app.CONFIRM) == "number") and app.CONFIRM or 5
local A_BACK  = (type(app.BACK)    == "number") and app.BACK    or 6
local BK, WH  = 0, 1

-- 布局
local HDR_Y   = SB_H + 6          -- 标题基线
local HDR_H   = SB_H + 46         -- 顶栏底边
local ROW_H   = 40                 -- 行高(只压缩不放大)
local ROWS    = 5                  -- 5 行: 26 + 40*5 = 226, 填到按钮上方
local LIST_Y  = 26                 -- 列表顶: 去掉"N 条 xxK"汇总行后直接顶到状态栏下
local LIST_B  = LIST_Y + ROW_H * ROWS
local DRAG_STEP = 30               -- 拖动多少像素翻一行(手指跟随)
local BTN_Y0  = 238                -- ★ 第5项: 开始录音按钮整体下移 10px (228→238)
local BTN_Y1  = 286
local BTN_X0  = 40
local BTN_X1  = 360
-- 录音页分区（计时/频谱各自局部重绘，互不整屏刷）
-- ★ 第7项: 计时上移 50px (72→22); 频谱从 40px 高扩到 86px, 吃掉空出来的区域
--   T_Y0 取 22 而不是 24: 上方 0~23 是系统状态栏, 清屏从 SB_H 起, 所以只清 24 以下,
--   大号数字本身从 T_Y0+10=32 开始, 不会压到状态栏。
local T_Y0, T_Y1   = 22, 114      -- 大号计时 (5x 字模 32..112)
local S_Y0, S_Y1   = 118, 202     -- 16 段实时频谱 (84px 高)
local SHINT_Y      = 204          -- 频谱下方（提示/消息）
local RBTN_CY      = 246          -- 圆形停止键中心
local RBTN_R       = 26

local G = {
  mode = "list",                 -- list / rec / saving / play / sheet / rename
  files = {}, n = 0, top = 1, sel = 0,
  rec_path = "",
  rec_bytes = 0, rec_bytes_t = 0,
  save_path = "", save_t0 = 0, exit_after_save = false,
  play_name = "", play_total = 0,
  pending_play = 0,
  msg = "", msg_t = 0,
  no_mic = false,
  sheet_for = 0,
  rn_from = "", rn_buf = "",
  await = nil,                   -- "del"：等系统确认框结果
  batt_t = 0, batt_n = 0,        -- 低电量采样节拍 / 连续低值计数
  tick_ui = 0, last_sec = -1,
  part = nil,                    -- "time" / "spec"：录音页局部重绘请求
}

-- 按下状态（手势机）
local P = { on = false, x = 0, y = 0, lx = 0, ly = 0, t0 = 0,
            moved = false, long = false, id = nil }

-- ---------------------------------------------------------------- 基础工具
local function now_ms()
  return app.millis and app.millis() or 0
end

local function fmt_ms(ms)
  if not ms or ms < 0 then ms = 0 end
  local s = math.floor(ms / 1000)
  return string.format("%d:%02d", math.floor(s / 60), s % 60)
end

local function fmt_size(b)
  if not b then return "0K" end
  if b >= 1048576 then return string.format("%.1fM", b / 1048576) end
  return string.format("%dK", math.floor(b / 1024))
end

-- ---- UTF-8 安全截断 ----
-- ⛔ 绝不能对可能含中文的字符串用 sub(1, N)：切成半个字符后送进 ui_text16()，
--    那边 `if ((*p & 0xe0) == 0xe0) { ... p += 3; }` 会跳过结尾的 \0 继续读 → 越界。
local function cut_b(s, maxb)         -- 头部截断：不切断多字节字符
  if #s <= maxb then return s end
  local i = maxb
  while i > 0 do
    local nb = s:byte(i + 1)
    if not nb or nb < 0x80 or nb >= 0xC0 then break end
    i = i - 1
  end
  if i == 0 then return "" end
  return s:sub(1, i)
end

local function tail_b(s, maxb)        -- 尾部截断（改名输入框要显示末尾）
  if #s <= maxb then return s end
  local st = #s - maxb + 1
  while st <= #s do
    local b = s:byte(st)
    if not b or b < 0x80 or b >= 0xC0 then break end
    st = st + 1
  end
  if st > #s then return "" end
  return s:sub(st)
end

-- 列表行左侧标签：只用 ASCII。
-- ⚠ 名字不是标准时间戳时（用户改名 / 从电脑拷进来的中文名）一律显示序号 ——
--   这一列走 app.textsb()（8x16 ASCII 字体），塞中文既画不出来，截断还会越界。
-- 行标签: 日期-时间-编号（20261030_115651_3f2a.wav → "10-30 11:56 #3f2a"）
local function row_label(idx, name)
  -- 新命名: 20261030_115651_0001.wav → "10-30 11:56 #0001"
  -- 老文件尾部是十六进制(3f2a)，%x 一起收，照样能显示
  local y, mo, d, hh, mi, tail = name:match("^(%d%d%d%d)(%d%d)(%d%d)_(%d%d)(%d%d)%d%d_?(%x*)")
  if y and y ~= "1970" then
    if tail and tail ~= "" then
      return string.format("%s-%s %s:%s #%s", mo, d, hh, mi, tail)
    end
    return string.format("%s-%s %s:%s", mo, d, hh, mi)
  end
  return string.format("NO.%02d", idx)
end

-- 播放页标题：时间是标准命名就显示 "10-27 10:30"，否则显示名字本身（走 app.mix，可含中文）
local function pretty_name(name)
  local nm = name:gsub("%.wav$", "")
  local y, mo, d, hh, mi = nm:match("^(%d%d%d%d)(%d%d)(%d%d)_(%d%d)(%d%d)%d%d")
  if y then
    if y == "1970" then return hh .. ":" .. mi end
    return mo .. "-" .. d .. " " .. hh .. ":" .. mi
  end
  if #nm > 18 then nm = cut_b(nm, 17) .. "…" end
  return nm
end

local function msg(t)
  G.msg = t
  G.msg_t = now_ms()
end

-- 非录音状态用系统 toast（规范：弹窗一律走 app.dlg）；
-- 录音中用自绘，因为 toast 是独立弹窗页，弹出期间 Lua 的 poll 不跑。
local function toast(t)
  if app.dlg and app.dlg.toast then
    app.dlg.toast(t, 1200)
  else
    msg(t)
  end
end

local function fill_rect(x0, y0, x1, y1, c)
  if x1 >= x0 and y1 >= y0 then app.fill(x0, y0, x1, y1, c) end
end

-- 顶部状态栏。Lua 页面是 fullscreen，系统**不会**画状态栏 ⇒
-- 0..SB_H-1 这一条会一直留着上一个页面的残影（弹窗、桌面图标、上一次的提示语）。
-- 每次整屏重绘时补一遍，既保住状态栏，也把残影擦掉。


-- 胶囊（iOS 按钮形状）：填充 + 两端半圆
local function pill(x0, y0, x1, y1, on)
  local c = on and BK or WH
  local r = (y1 - y0) // 2
  local cy = (y0 + y1) // 2
  fill_rect(x0 + r, y0, x1 - r, y1, c)
  for dy = -r, r do
    local dx = math.floor(math.sqrt(math.max(0, r * r - dy * dy)))
    fill_rect(x0 + r - dx, cy + dy, x0 + r + dx, cy + dy, c)
    fill_rect(x1 - r - dx, cy + dy, x1 - r + dx, cy + dy, c)
  end
  if not on then
    for x = x0 + r, x1 - r do
      app.px(x, y0, BK); app.px(x, y1, BK)
    end
    for dy = -r, r do
      local dx = math.floor(math.sqrt(math.max(0, r * r - dy * dy)))
      app.px(x0 + r - dx, cy + dy, BK)
      app.px(x1 - r + dx, cy + dy, BK)
    end
  end
end

local function pill_label(x0, y0, x1, y1, text, on)
  local w = app.mixw(text)
  if on then
    pill(x0, y0, x1, y1, true)        -- ★ 原来这里是 app.fill ⇒ 选中态变成长方形
    app.mix((x0 + x1) // 2 - w // 2, (y0 + y1) // 2 - 8, text, true)
  else
    pill(x0, y0, x1, y1, false)
    app.mix((x0 + x1) // 2 - w // 2, (y0 + y1) // 2 - 8, text, false)
  end
end

local function circle(cx, cy, r, c)
  for dy = -r, r do
    local dx = math.floor(math.sqrt(math.max(0, r * r - dy * dy)))
    app.hline(cx - dx, cx + dx, cy + dy, c)
  end
end

-- 圆形停止键（实心圆 + 内嵌方块）
local function stop_button(cx, cy, r, pressed)
  circle(cx, cy, r, pressed and WH or BK)
  circle(cx, cy, r - 1, pressed and WH or BK)
  local s = r // 2
  fill_rect(cx - s, cy - s, cx + s, cy + s, pressed and BK or WH)
end

-- 播放三角
local function play_glyph(x, y, on)
  local c = on and WH or BK
  for r = 0, 8 do
    local w = 8 - math.abs(4 - r)
    app.hline(x, x + w, y + r, c)
  end
end

-- ---------------------------------------------------------------- WAV 头解析
-- 全部用 byte() 手算小端：LUA_32BITS 下数字是 float，不依赖 string.unpack
local function le16(h, i)
  return h:byte(i) + h:byte(i + 1) * 256
end

local function le32(h, i)
  return h:byte(i) + h:byte(i + 1) * 256
       + h:byte(i + 2) * 65536 + h:byte(i + 3) * 16777216
end

-- 读 44 字节头 → {rate, ch, bits, dl}；读不到/不是 RIFF 返回 nil
local function wav_info(path)
  if not (app.fs and app.fs.read_at) then return nil end
  local h = app.fs.read_at(path, 0, 44)
  if type(h) ~= "string" or #h < 44 then return nil end
  if h:sub(1, 4) ~= "RIFF" then return nil end
  local ch   = le16(h, 23)          -- offset 22
  local rate = le32(h, 25)          -- offset 24
  local bits = le16(h, 35)          -- offset 34
  local dl   = le32(h, 41)          -- offset 40
  if ch < 1 or ch > 2 then ch = CH end
  if bits ~= 16 and bits ~= 8 then bits = 16 end
  if rate < 8000 or rate > 48000 then rate = RATE end
  return { rate = rate, ch = ch, bits = bits, dl = dl }
end

local function bps_of(info)
  local n = info.rate * info.ch * (info.bits / 8)
  if n < 1 then n = BYTE_RATE end
  return n
end

-- ---------------------------------------------------------------- 文件
local function scan()
  G.files, G.n = {}, 0
  if not (app.fs and app.fs.listdir) then return end
  local t = app.fs.listdir(DIR)
  if type(t) ~= "table" then return end
  for i = 1, #t do
    local nm = t[i]
    if type(nm) == "string" and nm:lower():match("%.wav$") then
      local p  = DIR .. nm
      local sz = (app.fs.size and app.fs.size(p)) or 0
      local info = wav_info(p)
      local dur, bad = 0, false
      if info and info.dl > 0 and info.dl <= sz - 44 then
        dur = math.floor(info.dl * 1000 / bps_of(info))
      elseif sz > 44 then
        -- 头没写成（掉电/未收尾）：时长按固定格式估，但底层播放读不到数据
        dur = math.floor((sz - 44) * 1000 / BYTE_RATE)
        bad = true
      else
        bad = true
      end
      G.n = G.n + 1
      G.files[G.n] = { name = nm, path = p, size = sz, dur = dur, bad = bad }
    end
  end
  -- ★ 2026-10-30 linit: 录音机列表**只放录音**，不再混入 MP3 试听条目。
  --   MP3 归音乐播放器，这里列出来既乱又让人以为录音里有它。
  table.sort(G.files, function(a, b) return a.name > b.name end)  -- 新的在上
  if G.sel > G.n then G.sel = G.n end
  if G.sel < 1 and G.n > 0 then G.sel = 1 end
  if G.top > G.n - ROWS + 1 then G.top = math.max(1, G.n - ROWS + 1) end
  if G.top < 1 then G.top = 1 end
end

local function cur()
  if G.sel >= 1 and G.sel <= G.n then return G.files[G.sel] end
  return nil
end

-- 命名：YYYYMMDD_HHMMSS_xxxx.wav（时间 + 4 位随机，同秒也不会重名）
local function datetime_from_epoch(ep)
  local t = ep + 8 * 3600
  local days, sod = t // 86400, t % 86400
  local hh, mi, ss = sod // 3600, (sod % 3600) // 60, sod % 60
  local y = 1970
  while true do
    local leap = ((y % 4 == 0 and y % 100 ~= 0) or (y % 400 == 0)) and 366 or 365
    if days < leap then break end
    days = days - leap; y = y + 1
  end
  local md = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
  if (y % 4 == 0 and y % 100 ~= 0) or (y % 400 == 0) then md[2] = 29 end
  local m = 1
  while m <= 12 and days >= md[m] do days = days - md[m]; m = m + 1 end
  return y, m, days + 1, hh, mi, ss
end

local function next_name()
  local ep = 0
  if app.util and app.util.epoch then ep = tonumber(app.util.epoch()) or 0 end
  local y, mo, d, hh, mi, ss
  if ep > 86400 then
    y, mo, d, hh, mi, ss = datetime_from_epoch(ep)
  else
    local ms = now_ms()
    y, mo, d = 1970, 1, 1
    hh, mi, ss = (ms // 3600000) % 24, (ms // 60000) % 60, (ms // 1000) % 60
  end
  -- 编号：目录里已有的最大 4 位序号 +1（从 0001 起），不是随机十六进制
  local seq = 0
  if app.fs and app.fs.listdir then
    local ok, t = pcall(app.fs.listdir, DIR)
    if ok and type(t) == "table" then
      for _, nm in ipairs(t) do
        local k = tostring(nm):match("_(%d%d%d%d)%.wav$")
        if k then k = tonumber(k); if k > seq then seq = k end end
      end
    end
  end
  for _ = 1, 9999 do
    seq = seq + 1
    if seq > 9999 then seq = 1 end
    local nm = string.format("%04d%02d%02d_%02d%02d%02d_%04d.wav", y, mo, d, hh, mi, ss, seq)
    if not (app.fs.exists and app.fs.exists(DIR .. nm)) then return nm end
  end
  return string.format("%04d%02d%02d_%02d%02d%02d_%04d.wav", y, mo, d, hh, mi, ss, seq)
end

-- ---------------------------------------------------------------- 动作
local function rec_real_ms()
  if not (app.rec and app.rec.bytes) then return 0 end
  local b = app.rec.bytes() or 0
  return math.floor(b * 1000 / BYTE_RATE)
end

local function rec_err(rc)
  if rc == -1 then return "录音已在进行"
  elseif rc == -2 then return "麦克风启动失败"
  elseif rc == -3 then return "无法创建文件"
  elseif rc == -4 then return "录音任务创建失败"
  end
  return "录音启动失败(" .. tostring(rc) .. ")"
end

local function start_rec()
  if not app.rec then msg("固件不支持录音"); return end
  if G.no_mic then msg("未检测到麦克风"); return end
  -- 底层 s_rec_task 还没清空（上一条正在收尾）→ 明确告知，别让按钮点了没反应
  if app.rec.active() then msg("上一条还在收尾"); app.redraw(); return end
  if not (app.fs.exists and app.fs.exists(DIR)) then
    if not (app.fs.mkdir and app.fs.mkdir(DIR)) then
      msg("无法创建录音目录"); app.redraw(); return
    end
  end
  local nm = next_name()
  local full = DIR .. nm
  -- 双保险：底层是 fopen("wb")，同名会直接覆盖
  if app.fs.exists and app.fs.exists(full) then msg("文件名冲突"); app.redraw(); return end
  local rc = app.rec.start(full, RATE)
  if rc ~= 0 then msg(rec_err(rc)); app.redraw(); return end
  G.rec_path    = full
  G.rec_bytes   = 0
  G.rec_bytes_t = now_ms()
  G.batt_n      = 0
  G.last_sec    = -1
  G.tick_ui     = 0
  G.mode        = "rec"
  G.part        = nil
  app.redraw()
end

-- 停止录音 → 进 saving；真正的"已保存"要等收尾校验
local function stop_rec(why)
  if not (app.rec and app.rec.active()) then
    G.mode = "list"; app.redraw(); return
  end
  app.rec.stop()
  G.mode      = "saving"
  G.save_path = G.rec_path
  G.save_t0   = now_ms()
  if why then msg(why) end
  G.part = nil
  app.redraw()
end

-- 收尾校验：文件存在、RIFF 头、data_len 已回填
local function verify_saved(path)
  local sz = (app.fs.size and app.fs.size(path)) or 0
  if sz <= 44 then return false, "没有录到声音" end
  local info = wav_info(path)
  if not info then return false, "文件头异常" end
  if info.dl == 0 then return false, "文件未收尾" end
  if info.dl > sz - 44 + 4 then return false, "数据长度异常" end
  return true, math.floor(info.dl * 1000 / bps_of(info))
end

local function do_delete()
  local f = cur()
  if not f then return end
  if app.fs.remove(f.path) then
    scan(); toast("已删除")
  else
    toast("删除失败")
  end
  G.sel = math.min(G.sel, math.max(1, G.n))
  G.mode = "list"
  app.redraw()
end

-- 删除前二次确认（系统弹窗，结果在 poll 里取）
local function ask_delete()
  local f = cur()
  if not f then return end
  G.sheet_for = G.sel
  if app.dlg and app.dlg.confirm then
    G.await = "del"
    app.dlg.confirm("删除这条录音?")
  else
    do_delete()               -- 无弹窗能力时退回直接删（旧行为）
  end
end

local KB_ROWS = 4
local function enter_rename()
  local f = cur()
  if not f then return end
  G.mode = "rename"
  G.rn_from = f.path
  G.rn_buf = f.name:gsub("%.wav$", "")
  if app.kbd and app.kbd.use then app.kbd.use("rows", KB_ROWS) end
  app.redraw()
end

local function valid_name(nm)
  if nm == "" or nm:match("^%s*$") then return false, "名字不能为空" end
  if #nm > 40 then return false, "名字太长" end
  if nm:find('[/\\:*?"<>|]') then return false, "含非法字符" end
  if nm:match("%.wav$") then return false, "不要带 .wav" end
  return true
end

local function commit_rename()
  local nm = G.rn_buf:gsub("^%s+", ""):gsub("%s+$", "")
  local ok, why = valid_name(nm)
  if not ok then msg(why); app.redraw(); return end
  local to = DIR .. nm .. ".wav"
  if to ~= G.rn_from and app.fs.exists(to) then
    msg("已有同名文件"); app.redraw(); return
  end
  if app.fs.rename(G.rn_from, to) then scan(); toast("已改名")
  else msg("改名失败") end
  G.mode = "list"
  app.redraw()
end

local function stop_play()
  if G.mp3 and app.rec and app.rec.mstop then app.rec.mstop() end
  G.mp3 = false
  if app.rec then app.rec.stop_play() end
  G.pending_play = 0
  G.mode = "list"
  app.redraw()
end

local function play(i)
  local f = G.files[i]
  if not f then return end
  if not app.rec then msg("固件不支持播放"); return end
  if f.mp3 then
    if not app.rec.mplay then msg("固件不支持MP3试听"); app.redraw(); return end
    if app.rec.mplaying and app.rec.mplaying() then app.rec.mstop() end
    local rc = app.rec.mplay(f.path)
    if rc ~= 0 then
      if rc == -4 then msg("已静音，先调音量") else msg("MP3 打不开") end
      app.redraw(); return
    end
    G.sel = i
    G.mp3 = true
    G.mp3_tick = 0
    G.play_name = f.name
    G.play_total = 0
    G.mode = "play"
    app.redraw()
    return
  end
  if f.bad then
    msg("文件未收尾，无法播放")
    app.redraw()
    return
  end
  -- 上一条还在播：wav_play_task 退出前 play() 返回 -1（不是文件坏了）
  if app.rec.playing() then
    app.rec.stop_play()
    G.pending_play = i
    G.mode = "list"
    app.redraw()
    return
  end
  -- ★ 2026-10-30: 音量为 0 时底层照样返回 0（它只判 muted，不判 volume=0）
  --   ⇒ 界面进入播放页、进度条在走，但一点声音都没有，看着像"播放坏了"。
  --   这里提前拦一次并给出明确提示，别让人以为文件有问题。
  if app.volume and (app.volume() or 0) <= 0 then
    msg("音量为 0，按音量键调大")
    app.redraw()
    return
  end
  local rc = app.rec.play(f.path)
  if rc ~= 0 then
    if rc == -4 then msg("已静音，先调音量")
    elseif rc == -2 then msg("播放任务创建失败")
    elseif rc == -1 then msg("上一段还没停")
    else msg("打不开这条录音") end
    app.redraw()
    return
  end
  G.sel = i
  G.play_name = f.name
  G.mode = "play"
  app.redraw()
end

-- ---------------------------------------------------------------- 命中区
local function hit(x, y)
  if G.mode == "saving" then return "none" end

  if G.mode == "sheet" then
    local card_y = 300 - 3 * 46
    if y < card_y then return "sheet_cancel" end
    local idx = (y - card_y) // 46
    if idx == 0 then return "sheet_rename" end
    if idx == 1 then return "sheet_del" end
    return "sheet_cancel"
  end

  if G.mode == "list" then
    if y >= BTN_Y0 and y <= BTN_Y1 and x >= BTN_X0 and x <= BTN_X1 then return "primary" end
    if y >= LIST_Y and y < LIST_B then
      local r = (y - LIST_Y) // ROW_H
      local i = G.top + r
      if i >= 1 and i <= G.n then return "row" .. (r + 1) end
    end
    return "none"
  end

  if G.mode == "rec" then
    local d = math.sqrt((x - 200) ^ 2 + (y - RBTN_CY) ^ 2)
    if d <= RBTN_R + 8 then return "stop" end
    return "none"
  end

  if G.mode == "play" then
    local d = math.sqrt((x - 200) ^ 2 + (y - RBTN_CY) ^ 2)
    if d <= RBTN_R + 8 then return "stop" end
    return "none"                      -- ★ 第6项: 左上角不再有隐藏的返回热区
  end

  if G.mode == "rename" then
    if y >= 26 and y <= 58 and x <= 100 then return "back" end
    if y >= 26 and y <= 58 and x >= 300 then return "sheet_rename" end
    return "kbd"
  end
  return "none"
end

-- ---------------------------------------------------------------- 绘制
-- ⚠ 从 SB_H 开始，绝不动 y<24 的状态栏
local function draw_header(title, left_label, right_label, right_on)
  app.fill(0, SB_H, W - 1, HDR_H, WH)
  if left_label then
    pill_label(12, 26, 96, 58, left_label, false)
  elseif title and title ~= "" then
    app.mix(14, HDR_Y + 6, title, false)
  end
  if right_label then
    pill_label(W - 12 - 78, 26, W - 12, 58, right_label, right_on or false)
  end
  -- ★ 2026-10-30: 顶栏下方那条分割线去掉（用户要求，各页统一）
end

local function draw_row(i, y, pressed)
  local f = G.files[i]
  if pressed then fill_rect(10, y, W - 11, y + ROW_H - 6, BK) end
  -- ⚠ textsb 第4参是 **Lua 布尔**（固件里 lua_toboolean），不是颜色常量。
  --   以前传 `pressed and WH or BK` ⇒ 恒为数字 0，而 Lua 里 0 是真值 ⇒ inv=true
  --   ⇒ 每一行都是白字黑底。必须显式传布尔。
  local inv = (pressed == true)
  local label = row_label(G.n - i + 1, f.name)
  app.textsb(22, y + 14, label, inv)
  local right = f.mp3 and "MP3" or (f.bad and "--:--" or fmt_ms(f.dur))
  app.textsb(W - 56 - #right * 8, y + 14, right, inv)
  if f.bad then
    app.textsb(W - 26, y + 14, "!", inv)            -- 未收尾标记
  else
    play_glyph(W - 30, y + 13, pressed)
  end
end

local function draw_list()
  -- ★ 第3项: 顶栏已无标题/按钮, 不再用 draw_header(它按 HDR_H=70 清屏, 白留 46px)
  app.fill(0, SB_H, W - 1, LIST_Y - 1, WH)

  if G.n == 0 then
    if G.no_mic then
      app.mixc(W // 2, 120, "未检测到麦克风", false)
      app.mixc(W // 2, 146, "本机没有音频输入", false)
    else
      app.mixc(W // 2, 118, "还没有录音", false)
      app.mixc(W // 2, 146, "点下面按钮开始", false)
    end
  else
    app.fill(0, LIST_Y, W - 1, LIST_B, WH)
    for r = 0, ROWS - 1 do
      local i = G.top + r
      if i > G.n then break end
      local pressed = (not P.moved and P.id == "row" .. (r + 1))
      draw_row(i, LIST_Y + r * ROW_H, pressed)
      app.hline(14, W - 15, LIST_Y + (r + 1) * ROW_H - 2, BK)
    end
  end

  -- 主按钮。★ 按下态必须**保持胶囊形状**：
  --   旧版按下时画的是 fill_rect(BTN_X0+24 … BTN_X1-24) ⇒ 只剩中间那段直角矩形。
  --   现在外层还是胶囊，按下时内缩 5px 画一个白色胶囊（黑圈 + 白底黑字），形状不变。
  local txt = (app.rec and app.rec.active()) and "停止录音" or "开始录音"
  local w = app.mixw(txt)
  local tx = (BTN_X0 + BTN_X1) // 2 - w // 2
  local ty = (BTN_Y0 + BTN_Y1) // 2 - 8
  pill(BTN_X0, BTN_Y0, BTN_X1, BTN_Y1, true)
  if P.id == "primary" then
    pill(BTN_X0 + 5, BTN_Y0 + 5, BTN_X1 - 5, BTN_Y1 - 5, false)
    app.mix(tx, ty, txt, false)
  else
    app.mix(tx, ty, txt, true)
  end

  if G.msg ~= "" and now_ms() - G.msg_t < 1800 then
    app.mixc(W // 2, 208, G.msg, false)
  end
end

local function draw_rec_time()
  -- ⚠ 清屏不能越过状态栏: T_Y0=22 时上边 2 行属于系统状态栏, 从 SB_H 起清
  local fy = (T_Y0 > SB_H) and T_Y0 or SB_H
  app.fill(0, fy, W - 1, T_Y1, WH)
  app.bigc(W // 2, T_Y0 + 10, fmt_ms(rec_real_ms()), 5, false)
end

-- 16 段实时频谱（底层 audio_player 录音任务里算好的 Goertzel 能量，0~100）
local function draw_rec_spec()
  app.fill(22, S_Y0 - 4, W - 22, S_Y1 + 2, WH)
  local x0, y0, x1, y1 = 24, S_Y0, W - 24, S_Y1
  local nb = (app.rec and app.rec.spec_bins and app.rec.spec_bins()) or 16
  if nb < 1 then nb = 16 end
  local bw = (x1 - x0 + 1) // nb
  for i = 1, nb do
    local v = 0
    if app.rec and app.rec.spec then v = app.rec.spec(i) or 0 end
    if v > 100 then v = 100 end
    local h = (y1 - y0) * v // 100
    if h < 2 then h = 2 end
    local bx = x0 + (i - 1) * bw
    fill_rect(bx, y1 - h, bx + bw - 4, y1, BK)
  end
  app.hline(x0, x1, y1, BK)
  -- 注意: 不在这里判"无输入信号"。底层 peak 只统计左声道，单声道麦接在右声道时
  -- 会恒 0 → 误报。真没数据由 tick_rec 的 STALL_MS 判定并直接停录。
end

local function draw_rec_full()
  draw_header(nil, nil, nil)
  draw_rec_time()
  draw_rec_spec()
  stop_button(200, RBTN_CY, RBTN_R, P.id == "stop")
  if G.msg ~= "" and now_ms() - G.msg_t < 1800 then
    app.mixc(W // 2, SHINT_Y, G.msg, false)
  end
end

local function draw_saving()
  draw_header("保存中", nil, nil)
  local spin = math.floor(now_ms() // 120) % 12
  local cx, cy, r = W // 2, 140, 18
  circle(cx, cy, r, BK)
  for k = 0, 2 do
    local a = (spin + k * 4) * 2 * math.pi / 12
    local px = math.floor(cx + (r + 8) * math.cos(a))
    local py = math.floor(cy + (r + 8) * math.sin(a))
    fill_rect(px - 2, py - 2, px + 2, py + 2, BK)
  end
  app.mixc(W // 2, 186, "正在收尾 WAV 文件", false)
  app.mixc(W // 2, 210, "请勿退出", false)
end

local function draw_play()
  -- ★ 第6项: 播放页不显示左上角"返回"（返回入口统一用中间圆形停止键）
  draw_header(nil, nil, nil)
  if G.mp3 then
    app.mixc(W // 2, 74, pretty_name(G.play_name), false)
    app.mixc(W // 2, 122, "MP3 试听中", false)
    app.mixc(W // 2, 150, "音量与音乐一致", false)
    stop_button(200, RBTN_CY, RBTN_R, P.id == "stop")
    return
  end
  local ms  = app.rec and app.rec.play_ms() or 0
  local tot = app.rec and app.rec.play_total_ms() or 0
  if tot > 0 then G.play_total = tot end
  local shown_total = (tot > 0) and tot or G.play_total

  local nm = pretty_name(G.play_name)
  app.mixc(W // 2, 74, nm, false)

  app.bigc(W // 2, 100, fmt_ms(ms), 5, false)
  app.mixc(W // 2, 162, fmt_ms(shown_total), false)

  local x0, x1 = 32, W - 32
  app.rect(x0, 186, x1, 198, BK)
  if shown_total > 0 then
    local fw = math.floor((x1 - x0 - 4) * math.min(ms, shown_total) / shown_total)
    if fw > 0 then fill_rect(x0 + 2, 188, x0 + 1 + fw, 196, BK) end
  end

  stop_button(200, RBTN_CY, RBTN_R, P.id == "stop")
end

local function draw_sheet()
  local card_y = 300 - 3 * 46
  fill_rect(10, card_y - 8, W - 11, 299, WH)
  app.hline(10, W - 11, card_y - 8, BK)
  local items = { { "改名", "sheet_rename" }, { "删除", "sheet_del" }, { "取消", "sheet_cancel" } }
  for i = 1, 3 do
    local y = card_y + (i - 1) * 46
    local pressed = (P.id == items[i][2])
    if pressed then fill_rect(12, y + 2, W - 13, y + 43, BK) end
    local w = app.mixw(items[i][1])
    app.mix((W - w) // 2, y + 13, items[i][1], pressed)
    if i < 3 then app.hline(16, W - 17, y + 45, BK) end
  end
end

local function draw_rename()
  draw_header("", "取消", "保存")
  app.mix(16, 78, "文件名", false)
  app.rect(16, 100, W - 17, 134, BK)
  local shown = tail_b(G.rn_buf, 26)     -- 按字符边界取末尾，不切断中文
  app.mix(22, 106, shown .. "_", false)
  app.mixc(W // 2, 146, "回车保存, 不要带 .wav", false)
  app.kbd.draw()
end

function render()
  if G.mode == "rec" then
    -- 局部重绘：计时秒变一次、频谱 120ms，只刷各自那一块，不整屏 fill
    if G.part == "spec" then
      draw_rec_spec(); G.part = nil; return
    elseif G.part == "time" then
      draw_rec_time(); draw_rec_spec(); G.part = nil; return
    end
    app.fill(0, SB_H, W - 1, H - 1, WH)
    draw_rec_full()
    return
  end

  app.fill(0, SB_H, W - 1, H - 1, WH)
  if G.mode == "list" then draw_list()
  elseif G.mode == "saving" then draw_saving()
  elseif G.mode == "play" then draw_play()
  elseif G.mode == "sheet" then draw_list(); draw_sheet()
  elseif G.mode == "rename" then draw_rename()
  end
end

-- ---------------------------------------------------------------- 手势
-- 改名页的软键盘：app.kbd 只给布局/命中/取字符，文本编辑要应用自己做
-- （参考 wuxia.lua 的登录框：hit → press 取动作码 → char 取字符）
local function kbd_tap(x, y)
  if not (app.kbd and app.kbd.hit) then return end
  local i = app.kbd.hit(x, y)
  if type(i) ~= "number" or i < 0 then return end
  local act = app.kbd.press(i)
  local ch  = app.kbd.char(i)
  if act == app.kbd.CHAR and ch then
    if #G.rn_buf < 40 then G.rn_buf = G.rn_buf .. ch end
  elseif act == app.kbd.BKSP then
    G.rn_buf = G.rn_buf:sub(1, -2)
  elseif act == app.kbd.ENTER then
    commit_rename(); return
  elseif act == app.kbd.ESC then
    G.mode = "list"
  end
end

local function tap(id, x, y)
  if id == nil or id == "none" then return end

  if id == "kbd" then
    kbd_tap(x, y)
    app.redraw()
    return
  end

  if id == "back" then
    if G.mode == "play" then stop_play() else G.mode = "list"; app.redraw() end
    return
  end
  if id == "primary" then
    if app.rec and app.rec.active() then stop_rec() else start_rec() end
    return
  end
  if id == "stop" then
    if G.mode == "rec" then stop_rec() else stop_play() end
    return
  end
  if id == "sheet_rename" then
    if G.mode == "rename" then commit_rename() else enter_rename() end
    return
  end
  if id == "sheet_del" then
    G.mode = "list"
    ask_delete()
    return
  end
  if id == "sheet_cancel" then G.mode = "list"; app.redraw(); return end

  local r = id:match("^row(%d)$")
  if r then
    local i = G.top + tonumber(r) - 1
    if i >= 1 and i <= G.n then play(i) end
    return
  end
end

local function long_press(id)
  local r = id and id:match("^row(%d)$")
  if not r then return end
  local i = G.top + tonumber(r) - 1
  if i >= 1 and i <= G.n then
    G.sel = i
    G.mode = "sheet"
    app.redraw()
  end
end

-- 录音 / 保存状态推进（每帧）
local function tick_rec(now)
  if G.mode == "saving" then
    local active = app.rec and app.rec.active()
    if not active then
      local ok, info = verify_saved(G.save_path)
      G.mode = "list"
      scan()
      if ok then msg("已保存 " .. fmt_ms(info)) else msg(info) end
      if G.exit_after_save then
        G.exit_after_save = false
        app.redraw()
        app.exit()
        return
      end
      app.redraw()
    elseif now - G.save_t0 > 5000 then
      G.mode = "list"
      msg("保存超时，请检查 TF 卡")
      app.redraw()
    elseif now - G.tick_ui > 200 then
      G.tick_ui = now
      app.redraw()                   -- 让"保存中"的转圈动起来，用户能看出在干活
    end
    return
  end

  if G.mode ~= "rec" then return end

  -- 任务自己退出：写卡失败 / 麦克风掉线（不是用户点的停止）
  if not (app.rec and app.rec.active()) then
    G.mode      = "saving"
    G.save_path = G.rec_path
    G.save_t0   = now - 6000        -- 直接走校验，不再等
    G.part      = nil
    app.redraw()
    return
  end

  -- 写入停滞：连续 STALL_MS 没有新字节。
  -- ⚠ bytes 不涨只能证明"没有数据落盘"，不能断定"麦克风没声音" ——
  --   麦没输入、I2S 卡住、TF 写阻塞/写失败、任务已退出都会这样。
  --   所以提示语只说事实，不替底层下结论。
  local b = app.rec.bytes() or 0
  if b ~= G.rec_bytes then
    G.rec_bytes, G.rec_bytes_t = b, now
  elseif now - G.rec_bytes_t > STALL_MS then
    stop_rec("没有数据写入，已停止")
    return
  end

  -- 低电量自动保存（连续 BATT_NEED 次采样都低于阈值才停，滤掉瞬时压降）
  if now - G.batt_t > 5000 then
    G.batt_t = now
    local mv = (app.sys and app.sys.battery) and app.sys.battery() or nil
    -- nil(接口无值) / 0(未检测到电池) / 明显异常高值 一律不参与判定
    if type(mv) == "number" and mv > 0 and mv < 4320 then
      if mv < BATT_LOW then
        G.batt_n = (G.batt_n or 0) + 1
        if G.batt_n >= BATT_NEED then
          G.batt_n = 0
          stop_rec("电量低，已自动保存")
          return
        end
      else
        G.batt_n = 0
      end
    else
      G.batt_n = 0
    end
  end

  -- 局部刷新节流：计时按秒边界；频谱 120ms（约 8fps，够跟手又不浪费）
  local sec = math.floor(rec_real_ms() / 1000)
  if sec ~= G.last_sec then
    G.last_sec = sec
    G.part = "time"
    app.redraw()
  elseif now - G.tick_ui > 120 then
    G.tick_ui = now
    G.part = "spec"
    app.redraw()
  end
end

function poll()
  local now = now_ms()

  -- ★ 2026-10-30: 提示语只显示 1.8s，到点必须自己触发一次重绘把字擦掉。
  --   原来只在 msg() 当时 redraw 一次，1.8s 后条件变假但**没人再画**
  --   ⇒ "已保存 / 上一条还在收尾 / 文件未收尾" 这些字会一直留在屏上，
  --   看起来就是"界面有残留"。这是整屏只按 dirty 重绘的直接后果。
  if G.msg ~= "" and now - G.msg_t >= 1800 then
    G.msg = ""
    app.redraw()
  end

  -- MP3 试听结束 → 自动回到列表（前 6 次轮询跳过，防"刚开始就判结束"）
  if G.mode == "play" and G.mp3 then
    G.mp3_tick = (G.mp3_tick or 0) + 1
    if G.mp3_tick > 6 and app.rec and app.rec.mplaying and not app.rec.mplaying() then
      G.mp3 = false
      G.mode = "list"
      app.redraw()
      return
    end
  end


  if G.await == "del" then
    local r = (app.dlg and app.dlg.result) and app.dlg.result() or nil
    if r ~= nil then
      G.await = nil
      if r == 0 then
        G.sel = G.sheet_for
        do_delete()
        return
      end
      app.redraw()
    end
  end

  -- 待切换的播放（等上一条 WAV 任务真正退出）
  if G.pending_play > 0 then
    if not (app.rec and app.rec.playing()) then
      local i = G.pending_play
      G.pending_play = 0
      if G.files[i] then play(i) end
      return
    end
  end

  tick_rec(now)
  if G.mode == "saving" then return end   -- 保存中不响应触摸

  local tx, ty = nil, nil
  if app.touch then
    local x, y = app.touch()
    if type(x) == "number" then tx, ty = x, y end
  end

  if tx then
    if not P.on then
      P.on, P.x, P.y, P.lx, P.ly = true, tx, ty, tx, ty
      P.pv = ty                        -- 上一帧的 y（拖动增量用，不能先覆盖再算）
      P.t0, P.moved, P.long = now, false, false
      P.id = hit(tx, ty)
      -- ★ 拖动只在"真的动了"之后才成立。
      --   之前把"按在列表里"直接当成拖动 ⇒ 手指没动也算拖过，松手走不到 tap()，
      --   表现就是"点条目播放没反应"。这里只记"候选"，位移超阈值才转正。
      P.drag_cand = (G.mode == "list" and ty >= LIST_Y and ty < LIST_B)
      P.drag = false
      P.acc  = 0
      app.redraw()
    else
      if math.abs(tx - P.x) > 12 or math.abs(ty - P.y) > 12 then P.moved = true end
      if P.drag_cand and not P.drag and math.abs(ty - P.y) > 8 then P.drag = true end
      if P.drag then
        -- 列表内容跟随手指：每累计 DRAG_STEP 像素滚一行，边界处夹住
        P.acc = P.acc + (ty - P.pv)
        if math.abs(P.acc) >= DRAG_STEP then
          local k   = math.floor(math.abs(P.acc) / DRAG_STEP)
          local dir = (P.acc > 0) and -1 or 1      -- 手指往下 = 看更早的条目
          local max_top = math.max(1, G.n - ROWS + 1)
          local nt = G.top + dir * k
          if nt < 1 then nt = 1 elseif nt > max_top then nt = max_top end
          P.acc = P.acc - dir * k * DRAG_STEP
          if nt ~= G.top then G.top = nt; app.redraw() end
        end
      end
      P.pv = ty
      P.lx, P.ly = tx, ty
      -- 长按用原生按住时长，不自己攒时间
      local hold = (app.touch_hold and app.touch_hold()) or (now - P.t0)
      if not P.long and not P.moved and not P.drag and hold >= 480 then
        P.long = true
        long_press(P.id)
      end
    end
  elseif P.on then
    P.on = false
    -- ★ 第4项: 拖过就不算点击（原来这里按"翻页"处理，现在滚动在按住时已完成）
    -- ⚠ 这里原来还有个 `and quick`，quick 在"去掉翻页"那次改动里被一起删了，
    --   值恒为 nil ⇒ 判据永远假 ⇒ 点条目/点按钮全都没反应。时长已由 P.long 覆盖。
    if not P.long and not P.drag and not P.moved then
      tap(P.id, P.x, P.y)
    end
    P.drag = false
    P.drag_cand = false
    P.id = nil
    app.redraw()
  end

  -- 播放推进
  if G.mode == "play" then
    local tot = 0
    if app.rec then tot = app.rec.play_total_ms() or 0 end
    if not (app.rec and app.rec.playing()) then
      -- 播放任务已经不在跑：不管是自然播完还是异常退出，一律回列表，
      -- 保证界面不会卡在"假播放"状态。
      if tot == 0 then msg("这条录音没有可播放的数据") end
      G.mode = "list"
      app.redraw()
    elseif now - G.tick_ui > 200 then
      G.tick_ui = now
      app.redraw()               -- 进度条按墙钟推进（非硬件播放位置）
    end
  end
end

function action(a)
  if G.mode == "rename" and a ~= A_BACK then
    if a == A_OK then commit_rename()
    elseif a == A_LEFT then
      return true
    end
    return true
  end

  if G.mode == "saving" then
    return true                        -- 收尾期间吞掉按键，防误操作
  end

  if a == A_BACK then
    if G.mode == "list" then
      if app.rec and app.rec.active() then app.rec.stop() end
      -- ★ 退出确认：**只有固件那一次**。
      --   app.exit() 内部走 lua_request_exit() → 固件弹"退出程序?"（page_lua.c:593）。
      --   这里再自己 dlg.confirm 就会变成两个弹窗（12:06 复现）；返回 false 又完全不弹
      --   （12:0x 复现）。所以：直接 app.exit()，并 return true 表示"本脚本已消费 BACK"。
      app.exit()
      return true
    elseif G.mode == "rec" then
      stop_rec()
    elseif G.mode == "play" then
      stop_play()
    else
      G.mode = "list"; app.redraw()
    end
    return true
  end

  if G.mode == "list" then
    if a == A_UP then
      if G.n > 0 then
        G.sel = math.max(1, (G.sel == 0) and 1 or G.sel - 1)
        if G.sel < G.top then G.top = G.sel end
        app.redraw()
      end
      return true
    elseif a == A_DOWN then
      if G.n > 0 then
        G.sel = math.min(G.n, (G.sel == 0) and 1 or G.sel + 1)
        if G.sel > G.top + ROWS - 1 then G.top = G.sel - ROWS + 1 end
        app.redraw()
      end
      return true
    elseif a == A_OK then
      if G.sel >= 1 and G.sel <= G.n then play(G.sel) else start_rec() end
      return true
    elseif a == A_RIGHT then
      enter_rename(); return true
    elseif a == A_LEFT then
      if G.sel >= 1 and G.sel <= G.n then ask_delete() end
      return true
    end
  elseif G.mode == "sheet" then
    if a == A_OK then enter_rename()
    elseif a == A_DOWN or a == A_UP then
      G.mode = "list"; app.redraw()
    end
    return true
  elseif G.mode == "rec" then
    if a == A_OK then stop_rec() end
    return true
  elseif G.mode == "play" then
    if a == A_OK then
      if app.rec and app.rec.playing() then stop_play() else G.mode = "list"; app.redraw() end
    end
    return true
  end
  return false
end

function on_enter()
  G.mode = "list"
  G.msg = ""
  G.no_mic = false
  G.await = nil
  G.pending_play = 0
  G.exit_after_save = false
  G.part = nil
  if app.rec and app.rec.mic_available then
    G.no_mic = not app.rec.mic_available()
  end
  if app.fs and app.fs.mkdir then app.fs.mkdir(DIR) end
  scan()
  if G.no_mic then msg("未检测到麦克风") end
  app.redraw()
end

function on_exit()
  -- 录音中退出：底层 stop 是异步的，录音任务独立于 Lua 状态，会自己把头写完
  if app.rec then
    if app.rec.active() then app.rec.stop() end
    if app.rec.playing() then app.rec.stop_play() end
  end
end
