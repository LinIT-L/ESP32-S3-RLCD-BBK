--@lapp v1
--@app name=录音机
--@app cat=程序
--@app desc=麦克风录音存 TF 卡 (WAV), 可回放、改名、删除
--@app version=2.0
-- ============================================================================
-- 录音机 · 界面重做（2026-10-27 v2）
--
-- 设计原则（对应"点击不灵敏/按钮太多"的反馈）：
--   ① 一屏只有一个主操作：底部大胶囊按钮（48px 高，整块可点）
--   ② 列表行 44px 高（触控友好），整行都是热区，不用瞄准文字
--   ③ 按下立即反白（视觉反馈），**松手才触发** —— 按下就执行会误触
--   ④ 长按 480ms 出操作单（改名/删除），把常驻按钮从 5 个减到 1 个
--   ⑤ 拖动超过 12px 视为滑动（列表翻页），不触发点击
--   ⑥「完成」按钮固定在右上角，BACK 键与它同效：逐层返回，主页再按退出
-- ============================================================================

local W, H, SB_H = 400, 300, 24
local DIR  = "/sdcard/Rec/"
local BYTE_RATE = 64000            -- 16k * 2ch * 2B

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
local ROW_H   = 44
local ROWS    = 3
local LIST_Y  = HDR_H + 22
local LIST_B  = LIST_Y + ROW_H * ROWS
local BTN_Y0  = 228
local BTN_Y1  = 276
local BTN_X0  = 40
local BTN_X1  = 360

local G = {
  mode = "list",                 -- list / rec / play / sheet / rename
  files = {}, n = 0, top = 1, sel = 0,
  rec_ms = 0, rec_start = 0,
  play_name = "", play_total = 0,
  msg = "", msg_t = 0,
  no_mic = false,
  sheet_for = 0,
  rn_from = "", rn_buf = "", kb_ok = false,
  dirty_t = 0,
  last_scroll = 0,
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
  local s = ms // 1000
  return string.format("%d:%02d", s // 60, s % 60)
end

local function fmt_size(b)
  if not b then return "0K" end
  if b >= 1048576 then return string.format("%.1fM", b / 1048576) end
  return string.format("%dK", b // 1024)
end

-- 从文件名 20261027_103012_a1b2.wav 里取 "10:30"
local function clock_of(name)
  local hh, mi = name:match("_(%d%d)(%d%d)%d%d_")
  if hh then return hh .. ":" .. mi end
  return name:sub(1, 5)
end

-- 友好名称（时间没同步时不显示 1970 开头的长文件名）
local function label_of(f)
  if not f then return "" end
  if f.name:match("^1970") then return "录音" end
  return clock_of(f.name)
end

local function msg(t)
  G.msg = t
  G.msg_t = now_ms()
end

local function fill_rect(x0, y0, x1, y1, c)
  if x1 >= x0 and y1 >= y0 then app.fill(x0, y0, x1, y1, c) end
end

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
  -- 未填充时描边
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
  local inside = on
  if inside then
    app.fill(x0 + 1, y0 + 1, x1 - 1, y1 - 1, BK)
    app.mix((x0 + x1) // 2 - w // 2, (y0 + y1) // 2 - 8, text, true)
  else
    pill(x0, y0, x1, y1, false)
    app.mix((x0 + x1) // 2 - w // 2, (y0 + y1) // 2 - 8, text, false)
  end
end

-- 圆形（实心圆 + 内嵌白方块，iOS 停止键）
local function stop_button(cx, cy, r, pressed)
  for dy = -r, r do
    local dx = math.floor(math.sqrt(math.max(0, r * r - dy * dy)))
    local c = pressed and WH or BK
    app.hline(cx - dx, cx + dx, cy + dy, c)
  end
  -- 中间方块（白色/黑色反过来）
  local s = r // 2
  local c2 = pressed and BK or WH
  fill_rect(cx - s, cy - s, cx + s, cy + s, c2)
end

-- 播放三角
local function play_glyph(x, y, on)
  local c = on and WH or BK
  for r = 0, 8 do
    local w = 8 - math.abs(4 - r)
    app.hline(x, x + w, y + r, c)
  end
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
      local p = DIR .. nm
      local sz = (app.fs.size and app.fs.size(p)) or 0
      G.n = G.n + 1
      local secs = math.max(0, (sz - 44)) // BYTE_RATE
      G.files[G.n] = { name = nm, path = p, size = sz, dur = secs }
    end
  end
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
  for _ = 1, 8 do
    local r = 0
    if app.util and app.util.rand then r = app.util.rand(65535) end
    local nm = string.format("%04d%02d%02d_%02d%02d%02d_%04x.wav", y, mo, d, hh, mi, ss, r)
    if not (app.fs.exists and app.fs.exists(DIR .. nm)) then return nm end
  end
  return string.format("%04d%02d%02d_%02d%02d%02d.wav", y, mo, d, hh, mi, ss)
end

-- ---------------------------------------------------------------- 动作
local function start_rec()
  if not app.rec then msg("固件不支持录音"); return end
  if G.no_mic then msg("未检测到麦克风"); return end
  if app.rec.active() then return end
  local nm = next_name()
  local rc = app.rec.start(DIR .. nm, 16000)
  if rc ~= 0 then msg("录音启动失败(" .. tostring(rc) .. ")"); return end
  G.rec_start = now_ms()
  G.mode = "rec"
  app.redraw()
end

local function stop_rec()
  if not app.rec or not app.rec.active() then
    G.mode = "list"; return
  end
  app.rec.stop()
  G.mode = "list"
  G.rec_ms = 0
  scan()
  msg("已保存")
  app.redraw()
end

local function play(sel)
  local f = G.files[sel]
  if not f then return end
  if not app.rec then msg("固件不支持播放"); return end
  if app.rec.playing() then app.rec.stop_play() end
  local rc = app.rec.play(f.path)
  if rc ~= 0 then msg("打不开这条录音"); return end
  G.sel = sel
  G.play_name = f.name
  G.mode = "play"
  app.redraw()
end

local function stop_play()
  if app.rec then app.rec.stop_play() end
  G.mode = "list"
  app.redraw()
end

local function do_delete()
  local f = cur()
  if not f then return end
  if app.fs.remove(f.path) then scan(); msg("已删除") else msg("删除失败") end
  G.sel = math.min(G.sel, math.max(1, G.n))
  G.mode = "list"
  app.redraw()
end

local KB_ROWS = 4
local function kb_setup()
  app.kbd.use("rows", KB_ROWS)
  G.kb_ok = true
end

local function enter_rename()
  local f = cur()
  if not f then return end
  G.mode = "rename"
  G.rn_from = f.path
  G.rn_buf = f.name:gsub("%.wav$", "")
  kb_setup()
  app.redraw()
end

local function commit_rename()
  local nm = G.rn_buf:gsub("^%s+", ""):gsub("%s+$", "")
  if nm == "" then msg("名字不能为空"); return end
  local to = DIR .. nm .. ".wav"
  if to ~= G.rn_from and app.fs.exists(to) then msg("已有同名文件"); return end
  if app.fs.rename(G.rn_from, to) then scan(); msg("已改名") else msg("改名失败") end
  G.mode = "list"
  app.redraw()
end

-- ---------------------------------------------------------------- 命中区
-- 返回区域 id：done / primary / row1..3 / stop / back / sheet_rename / sheet_del / sheet_cancel
local function hit(x, y)
  if G.mode == "sheet" then
    local card_y = 300 - 3 * 46
    if y < card_y then return "sheet_cancel" end
    local idx = (y - card_y) // 46
    if idx == 0 then return "sheet_rename" end
    if idx == 1 then return "sheet_del" end
    return "sheet_cancel"
  end

  if G.mode == "list" then
    if y >= 26 and y <= 58 and x >= 306 then return "done" end
    if y >= BTN_Y0 and y <= BTN_Y1 and x >= BTN_X0 and x <= BTN_X1 then return "primary" end
    if y >= LIST_Y and y < LIST_B then
      local r = (y - LIST_Y) // ROW_H
      local i = G.top + r
      if i >= 1 and i <= G.n then return "row" .. (r + 1) end
    end
    return "none"
  end

  if G.mode == "rec" then
    local d = math.sqrt((x - 200) ^ 2 + (y - 246) ^ 2)
    if d <= 44 then return "stop" end
    return "none"
  end

  if G.mode == "play" then
    local d = math.sqrt((x - 200) ^ 2 + (y - 246) ^ 2)
    if d <= 44 then return "stop" end
    if y >= 26 and y <= 58 and x <= 100 then return "back" end
    return "none"
  end

  if G.mode == "rename" then
    if y >= 26 and y <= 58 and x <= 100 then return "back" end
    if y >= 26 and y <= 58 and x >= 300 then return "sheet_rename" end
    return "kbd"
  end
  return "none"
end

-- ---------------------------------------------------------------- 绘制
local function draw_header(title, left_label, right_label, right_on)
  app.fill(0, 0, W - 1, HDR_H, WH)
  if left_label then
    pill_label(12, 26, 96, 58, left_label, false)
  else
    app.mix(14, HDR_Y + 6, title, false)
  end
  if right_label then
    pill_label(W - 12 - 78, 26, W - 12, 58, right_label, right_on or false)
  end
  app.hline(0, W - 1, HDR_H, BK)
end

local function draw_row(i, y, pressed)
  local f = G.files[i]
  if pressed then fill_rect(10, y, W - 11, y + ROW_H - 6, BK) end
  local c = pressed and WH or BK
  local label
  if f.name:match("^1970") then
    label = string.format("NO.%02d", G.n - i + 1)   -- 时间没同步时按序号显示
  else
    label = clock_of(f.name)
  end
  app.textsb(22, y + 14, label, c)
  local right = fmt_ms(f.dur * 1000)
  app.textsb(W - 52 - #right * 8, y + 14, right, c)
  play_glyph(W - 30, y + 13, pressed)
end

local function draw_list()
  local total = 0
  for i = 1, G.n do total = total + G.files[i].size end
  draw_header("录音", nil, "完成")

  if G.n == 0 then
    if G.no_mic then
      app.mixc(W // 2, 120, "未检测到麦克风", false)
      app.mixc(W // 2, 146, "本机没有音频输入", false)
    else
      app.mixc(W // 2, 118, "还没有录音", false)
      app.mixc(W // 2, 146, "点下面按钮开始", false)
    end
  else
    -- 顶部小计
    app.mix(16, HDR_H + 4, string.format("%d 条  %s", G.n, fmt_size(total)), false)
    for r = 0, ROWS - 1 do
      local i = G.top + r
      if i > G.n then break end
      local pressed = (not P.moved and P.id == "row" .. (r + 1))
      draw_row(i, LIST_Y + r * ROW_H, pressed)
      app.hline(14, W - 15, LIST_Y + (r + 1) * ROW_H - 2, BK)
    end
  end

  -- 主按钮
  local pressed = (P.id == "primary")
  if pressed then
    fill_rect(BTN_X0 + 24, BTN_Y0, BTN_X1 - 24, BTN_Y1, BK)
    local w = app.mixw("停止录音")
    app.mix((BTN_X0 + BTN_X1) // 2 - w // 2, (BTN_Y0 + BTN_Y1) // 2 - 8, "停止录音", true)
  else
    pill(BTN_X0, BTN_Y0, BTN_X1, BTN_Y1, true)
    local txt = (app.rec and app.rec.active()) and "停止录音" or "开始录音"
    local w = app.mixw(txt)
    app.mix((BTN_X0 + BTN_X1) // 2 - w // 2, (BTN_Y0 + BTN_Y1) // 2 - 8, txt, true)
  end

  if G.msg ~= "" and now_ms() - G.msg_t < 1800 then
    app.mixc(W // 2, 208, G.msg, false)
  end
end

local function draw_rec()
  local ms = app.rec and app.rec.ms() or 0
  draw_header("录音中", nil, "完成")
  app.bigc(W // 2, 84, fmt_ms(ms), 5, false)

  -- 频谱
  local x0, y0, x1, y1 = 24, 158, W - 24, 202
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

  stop_button(200, 246, 26, P.id == "stop")
  app.mixc(W // 2, 276, "轻点方块结束并保存", false)

  local pk = (app.rec and app.rec.peak and app.rec.peak()) or 0
  if pk == 0 and ms > 2000 then
    app.mixc(W // 2, 212, "无输入信号", false)
  end
end

local function draw_play()
  draw_header("", "返回", nil)
  local ms  = app.rec and app.rec.play_ms() or 0
  local tot = app.rec and app.rec.play_total_ms() or 0
  if tot > 0 then G.play_total = tot end

  app.mixc(W // 2, 72, label_of({ name = G.play_name }), false)

  app.bigc(W // 2, 106, fmt_ms(ms), 5, false)
  app.mixc(W // 2, 168, fmt_ms(tot), false)

  local x0, x1 = 32, W - 32
  app.rect(x0, 190, x1, 200, BK)
  if tot > 0 then
    local fw = (x1 - x0 - 4) * math.min(ms, tot) // tot
    if fw > 0 then fill_rect(x0 + 2, 192, x0 + 1 + fw, 198, BK) end
  end

  stop_button(200, 246, 26, P.id == "stop")
  app.mixc(W // 2, 276, "轻点方块停止", false)
end

local function draw_sheet()
  local card_y = 300 - 3 * 46
  fill_rect(0, card_y - 8, W - 1, 299, WH)
  app.hline(0, W - 1, card_y - 8, BK)
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
  app.mix(22, 106, G.rn_buf .. "_", false)
  app.mixc(W // 2, 146, "用键盘编辑，回车保存", false)
  app.kbd.draw()
end

function render()
  app.fill(0, SB_H, W - 1, H - 1, WH)
  if G.mode == "list" then draw_list()
  elseif G.mode == "rec" then draw_rec()
  elseif G.mode == "play" then draw_play()
  elseif G.mode == "sheet" then draw_list(); draw_sheet()
  elseif G.mode == "rename" then draw_rename()
  end
end

-- ---------------------------------------------------------------- 手势
local function tap(id)
  if id == nil or id == "none" then return end

  if id == "done" then
    if app.rec and app.rec.active() then app.rec.stop() end
    app.exit(); return
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
  if id == "sheet_del" then do_delete(); return end
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
    G.play_name = G.files[i].name
    G.mode = "sheet"
    app.redraw()
  end
end

function poll()
  local tx, ty = nil, nil
  if app.touch then
    local x, y = app.touch()          -- 返回 (x,y)；“无手势”返回 nil
    if type(x) == "number" then tx, ty = x, y end
  end
  local now = now_ms()

  if tx then
    if not P.on then
      P.on, P.x, P.y, P.lx, P.ly = true, tx, ty, tx, ty
      P.t0, P.moved, P.long = now, false, false
      P.id = hit(tx, ty)
      app.redraw()
    else
      P.lx, P.ly = tx, ty
      if math.abs(tx - P.x) > 12 or math.abs(ty - P.y) > 12 then P.moved = true end
      if not P.long and not P.moved and (now - P.t0) >= 480 then
        P.long = true
        long_press(P.id)
      end
    end
  elseif P.on then
    P.on = false
    local quick = (now - P.t0) < 900
    if not P.long then
      if P.moved then
        -- 竖向拖动 = 列表翻页
        local dy = P.ly - P.y
        if G.mode == "list" and math.abs(dy) > 40 then
          local page = (dy > 0) and ROWS or -ROWS
          local max_top = math.max(1, G.n - ROWS + 1)
          G.top = math.max(1, math.min(max_top, G.top + page))
          app.redraw()
        end
      elseif quick then
        tap(P.id)
      end
    end
    P.id = nil
    app.redraw()
  end

  -- 录音/播放时逐帧刷新
  if G.mode == "rec" or G.mode == "play" then
    if now - G.dirty_t > 80 then G.dirty_t = now; app.redraw() end
  end
  -- 录音结束（任务自己收尾）→ 回列表并刷新
  if G.mode == "rec" and app.rec and not app.rec.active() then
    G.mode = "list"; scan(); app.redraw()
  end
  if G.mode == "play" and app.rec and not app.rec.playing() then
    local ms, tot = app.rec.play_ms(), app.rec.play_total_ms()
    if tot > 0 and ms >= tot then G.mode = "list"; app.redraw() end
  end
  if G.mode == "list" and now - G.last_scroll > 1000 then
    G.last_scroll = now
    if app.rec and not app.rec.active() then app.redraw() end
  end
end

function action(a)
  if G.mode == "rename" and a ~= A_BACK then
    if a == A_OK then commit_rename()
    elseif a == A_LEFT then -- 键盘左移交给 app.kbd
      return true
    end
    return true
  end

  if a == A_BACK then
    if G.mode == "list" then
      if app.rec and app.rec.active() then app.rec.stop() end
      app.exit()
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
      do_delete(); return true
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
    if a == A_OK then stop_play() end
    return true
  end
  return false
end

function on_enter()
  G.mode = "list"
  G.msg = ""
  G.no_mic = false
  if app.rec and app.rec.mic_available then
    G.no_mic = not app.rec.mic_available()
  end
  if app.fs and app.fs.mkdir then app.fs.mkdir(DIR) end
  scan()
  if G.no_mic then msg("未检测到麦克风") end
  app.redraw()
end

function on_exit()
  if app.rec then
    if app.rec.active() then app.rec.stop() end
    if app.rec.playing() then app.rec.stop_play() end
  end
end
