--@lapp v1
--@app name=录音机
--@app cat=程序
--@app desc=麦克风录音存 TF 卡 (WAV), 可回放、改名、删除
--@app version=1.0
--@app icon=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB/4AAAAAAAAH/gAAAAAAAAf+AAAAAAAAB/4AAAAAAAA/D8AAAAAAAD8PwAAAAAAAPw/AAAAAAAA/D8AAAAAAAD8PwAAAAAAAP8/AAAAAAAA/D8AAAAAAAD8PwAAAAAAAP//AAAAAAAA//8AAAAAAAD//wAAAAAAAP//AAAAAAAA//8AAAAAAAD//wAAAAAAAP//AAAAAAAA//8AAAAAACD//wYAAAAAIP//BgAAAAAg//8EAAAAACD//wQAAAAAIB/4BAAAAAAwH/gMAAAAABAf+AgAAAAAGB/4GAAAAAAMAAAwAAAAAAwAADAAAAAABgAAYAAAAAADgAHAAAAAAAHgB4AAAAAAAH//AAAAAAAAH/gAAAAAAAABgAAAAAAAAAGAAAAAAAAAAYAAAAAAAAABgAAAAAAAAAGAAAAAAAAAAYAAAAAAAA///wAAAAAAD///AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
--
-- 录音机 · LinTOS Lua 单文件应用
--
-- 数据通路: app.rec.*  (components/services/audio_player 的录音任务 + WAV 播放)
--           app.fs.*   (TF 卡文件: 列表 / 大小 / 删除 / 改名)
--
-- 为什么要 C 侧支持: 录音必须**连续**读 I2S, 在 Lua 的 poll 里读会丢样本
-- (poll 一帧几十毫秒, 中间没人取 DMA 就溢出)。所以录音跑在独立任务里,
-- Lua 只发"开始/停止"两个命令。
--
-- 文件格式: WAV (PCM 无损), 16 kHz / 16 bit / 双声道 = 64 KB/s
--   ⇒ 1 分钟约 3.8 MB。TF 卡完全够, 而且不需要任何编码器。
--
-- 交互: 触摸点按钮; 无触摸的板子用按键 ——
--   上下 选文件 · OK 播放/停止 · 长按 OK 录音/停止 · 左 改名 · 右 删除 · BACK 退出

local W, H, SB_H = 400, 300, 24
local DIR     = "/sdcard/Rec/"
local ROW_H   = 30
local LIST_Y  = SB_H + 26
local LIST_B  = 238
local BTN_Y   = 246
local BTN_H   = 40
local BYTE_RATE = 64000                 -- 16k * 2ch * 2byte

local A_UP    = (type(app.UP)      == "number") and app.UP      or 1
local A_DOWN  = (type(app.DOWN)    == "number") and app.DOWN    or 2
local A_LEFT  = (type(app.LEFT)    == "number") and app.LEFT    or 3
local A_RIGHT = (type(app.RIGHT)   == "number") and app.RIGHT   or 4
local A_OK    = (type(app.CONFIRM) == "number") and app.CONFIRM or 5
local A_BACK  = (type(app.BACK)    == "number") and app.BACK    or 6
local BK, WH  = 0, 1

local G = {
  mode = "list",
  rate = 16000,               -- 采样率（设置页可改）
  show_spec = true,           -- 录音时是否画频谱
  saving = false,
  save_t = 0,
  set_sel = 1,
  spec = {},                  -- 频谱缓存（16 段）
  no_mic = false,             -- 本机没有麦克风输入（标准扩展板）              -- list / rec / play / rename
  files = {}, n = 0, sel = 1, top = 1,
  msg = "", msg_t = 0,
  key = "",
  rec_ms = 0,
  play_name = "",
  rn_from = "", rn_buf = "",
  kb_x = 6, kb_y = 0, kb_idx = -1,
  hold_ms = 0, holding = false,
  confirm_del = false,
  t_down = 0,
}

local function now_ms() return app.millis() or 0 end

local function fmt_ms(ms)
  local s = (ms or 0) // 1000
  return string.format("%02d:%02d", s // 60, s % 60)
end

local function fmt_size(b)
  if b >= 1048576 then return string.format("%.1fM", b / 1048576) end
  if b >= 1024 then return string.format("%dK", b // 1024) end
  return tostring(b) .. "B"
end

local function msg(s)
  G.msg = s or ""
  G.msg_t = now_ms()
  app.redraw()
end

-- ================= 文件 =================

local function scan()
  if app.fs.mkdir then pcall(app.fs.mkdir, DIR) end
  local names = app.fs.listdir(DIR) or {}
  local out = {}
  for i = 1, #names do
    local nm = names[i]
    if nm:lower():match("%.wav$") then
      local path = DIR .. nm
      local sz = app.fs.size(path) or 0
      out[#out + 1] = {
        name = nm,
        path = path,
        size = sz,
        dur  = (sz > 44) and ((sz - 44) * 1000 // BYTE_RATE) or 0,
      }
    end
  end
  table.sort(out, function(a, b) return a.name < b.name end)
  G.files, G.n = out, #out
  if G.sel > G.n then G.sel = G.n end
  if G.sel < 1 then G.sel = 1 end
  if G.top > G.n then G.top = 1 end
end

-- epoch → 本地日期时间（UTC+8）。Lua 沙箱里没有 os.date，只能手算。
local function datetime_from_epoch(ep)
  local t = ep + 8 * 3600
  local days = t // 86400
  local sod  = t % 86400
  local hh, mi, ss = sod // 3600, (sod % 3600) // 60, sod % 60
  local y = 1970
  while true do
    local leap = ((y % 4 == 0 and y % 100 ~= 0) or (y % 400 == 0)) and 366 or 365
    if days < leap then break end
    days = days - leap
    y = y + 1
  end
  local md = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
  if (y % 4 == 0 and y % 100 ~= 0) or (y % 400 == 0) then md[2] = 29 end
  local m = 1
  while m <= 12 and days >= md[m] do
    days = days - md[m]
    m = m + 1
  end
  return y, m, days + 1, hh, mi, ss
end

-- 命名规则: YYYYMMDD_HHMMSS_xxxx.wav
--   前段是录音开始时间（看着就有序）, 后段 4 位十六进制随机（同一秒内也不会撞名）
local function next_name()
  local ep = 0
  if app.util and app.util.epoch then
    ep = tonumber(app.util.epoch()) or 0
  end
  local y, mo, d, hh, mi, ss
  if ep > 86400 then
    y, mo, d, hh, mi, ss = datetime_from_epoch(ep)
  else
    -- 系统时间没同步（刚开机没联网）：用运行时长凑一个不重名的名字
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

local function cur() return G.files[G.sel] end

-- ================= 动作 =================

local function start_rec()
  if not app.rec then msg("固件不支持录音") return end
  if G.no_mic then msg("未检测到麦克风") return end
  local name = next_name()
  local rc = app.rec.start(DIR .. name, G.rate)
  if rc == 0 then
    G.mode = "rec"
    G.rec_ms = 0
    msg("录音中 " .. name)
  else
    msg("录音失败 (" .. tostring(rc) .. ")")
  end
end

local function stop_rec()
  if not app.rec then return end
  -- ★ 只发命令就返回：C 侧 stop 已改成非阻塞（以前在这里阻塞 1 秒 → 触发看门狗重启）
  app.rec.stop()
  G.mode = "saving"
  G.save_t = now_ms()
  G.saving = true
  app.redraw()
end

local function toggle_play()
  local f = cur()
  if not f then return end
  if not app.rec then msg("固件不支持播放") return end
  if app.rec.playing() then
    app.rec.stop_play()
    G.mode = "list"
    G.play_name = ""
  else
    if app.rec.play(f.path) == 0 then
      G.mode = "play"
      G.play_name = f.name
    else
      msg("播放失败")
    end
  end
end

local function do_delete()
  local f = cur()
  if not f then return end
  if not G.confirm_del then
    G.confirm_del = true
    msg("再按一次删除 " .. f.name)
    return
  end
  G.confirm_del = false
  if app.fs.remove(f.path) then
    scan()
    msg("已删除")
  else
    msg("删除失败")
  end
end

-- ================= 改名键盘 =================

local KB_ROWS = 4

local function kb_setup()
  app.kbd.use("rows", KB_ROWS)
  G.kb_y = H - KB_ROWS * app.kbd.ROW
  G.kb_x = 6
  G.kb_idx = app.kbd.hit(G.kb_x, G.kb_y + math.floor(app.kbd.ROW / 2))
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
  if nm == "" then msg("名字不能为空") return end
  if nm == G.rn_from then G.mode = "list" return end
  local to = DIR .. nm .. ".wav"
  if app.fs.exists(to) then msg("已存在同名文件") return end
  if app.fs.rename(G.rn_from, to) then
    G.mode = "list"
    scan()
    msg("已改名")
  else
    msg("改名失败")
  end
end

local function kb_move(dx, dy)
  local nx = G.kb_x + dx * 48
  local ny = G.kb_y + dy * app.kbd.ROW
  local row_h = app.kbd.ROW
  if app.kbd.hit(nx, ny) < 0 then
    if dy == 0 then
      nx = (dx > 0) and 6 or (W - 10)
    else
      ny = (dy > 0) and (G.kb_y + row_h // 2) or (H - row_h // 2)
    end
    if app.kbd.hit(nx, ny) < 0 then return end
  end
  G.kb_x, G.kb_y = nx, ny
  G.kb_idx = app.kbd.hit(nx, ny)
  app.redraw()
end

local function kb_tap()
  local i = G.kb_idx
  if i < 0 then return end
  local act = app.kbd.press(i)
  local ch = app.kbd.char(i)
  if act == app.kbd.CHAR and ch then
    if #G.rn_buf < 24 then G.rn_buf = G.rn_buf .. ch end
  elseif act == app.kbd.BKSP then
    G.rn_buf = G.rn_buf:sub(1, -2)
  elseif act == app.kbd.ENTER then
    commit_rename()
  elseif act == app.kbd.ESC then
    G.mode = "list"
  end
  app.redraw()
end

-- ================= 绘制 =================

local function header(title, right)
  app.fill(0, 0, W - 1, SB_H + 22, WH)
  app.mix(6, SB_H + 4, title, false)
  if right and right ~= "" then
    app.mix(W - 6 - app.mixw(right), SB_H + 4, right, false)
  end
  app.hline(0, W - 1, SB_H + 22, BK)
end

local function button(x0, w, label, active)
  app.rect(x0, BTN_Y, x0 + w, BTN_Y + BTN_H - 1, BK)
  if active then
    app.fill(x0 + 2, BTN_Y + 2, x0 + w - 2, BTN_Y + BTN_H - 3, BK)
  end
  app.mixc(x0 + w // 2, BTN_Y + 12, label, active)
end

local function draw_list()
  local total = 0
  for i = 1, G.n do total = total + G.files[i].size end
  header("录音机", G.n .. " 条  " .. fmt_size(total))

  app.fill(0, LIST_Y, W - 1, LIST_B, WH)
  local rows = (LIST_B - LIST_Y) // ROW_H
  if G.sel < G.top then G.top = G.sel end
  if G.sel > G.top + rows - 1 then G.top = G.sel - rows + 1 end

  if G.n == 0 then
    if G.no_mic then
      app.mixc(W // 2, LIST_Y + 62, "未检测到麦克风", false)
      app.mixc(W // 2, LIST_Y + 88, "本机没有音频输入", false)
      app.mixc(W // 2, LIST_Y + 114, "录音功能不可用", false)
    else
      app.mixc(W // 2, LIST_Y + 70, "还没有录音", false)
      app.mixc(W // 2, LIST_Y + 96, "点「录音」开始", false)
    end
  else
    for r = 0, rows - 1 do
      local i = G.top + r
      if i > G.n then break end
      local f = G.files[i]
      local y = LIST_Y + r * ROW_H
      local sel = (i == G.sel)
      if sel then app.fill(0, y, W - 1, y + ROW_H - 2, BK) end
      local nm = f.name:gsub("%.wav$", "")
      app.mix(8, y + 7, nm, sel)
      local meta = fmt_ms(f.dur) .. "  " .. fmt_size(f.size)
      app.mix(W - 8 - app.mixw(meta), y + 7, meta, sel)
      if G.mode == "play" and f.name == G.play_name then
        app.textsb(2, y + 10, ">", sel)
      end
    end
  end

  if G.no_mic then
    app.mixc(W // 2, LIST_B - 18, "未检测到麦克风", false)
  end
  app.hline(0, W - 1, LIST_B + 2, BK)
  button(4,   76, (G.mode == "rec") and "停止" or "录音", G.mode == "rec")
  button(84,  76, (app.rec and app.rec.playing()) and "停止" or "播放", false)
  button(164, 76, "改名", false)
  button(244, 76, G.confirm_del and "确认删" or "删除", G.confirm_del)
  button(324, 76, "设置", false)
end

local function draw_rec()
  header("录音中", (G.rate // 1000) .. "kHz WAV")
  local ms = app.rec and app.rec.ms() or G.rec_ms
  app.bigc(W // 2, 56, fmt_ms(ms), 5, false)

  local x0, y0, x1, y1 = 16, 132, 384, 212
  if G.show_spec and app.rec and app.rec.spec then
    local nb = app.rec.spec_bins and app.rec.spec_bins() or 16
    if nb < 1 then nb = 16 end
    local bw = (x1 - x0 + 1) // nb
    for i = 1, nb do
      local v = app.rec.spec(i) or 0
      if v > 100 then v = 100 end
      local h = (y1 - y0) * v // 100
      if h < 1 then h = 1 end
      local bx = x0 + (i - 1) * bw
      app.fill(bx, y1 - h, bx + bw - 5, y1, BK)
    end
    app.hline(x0, x1, y1, BK)
    app.mix(x0, y0 - 14, "0Hz", false)
    app.mix(x1 - 34, y0 - 14, "8kHz", false)
  else
    app.rect(x0, y0, x1, y1, BK)
    app.mixc(W // 2, (y0 + y1) // 2 - 8, "频谱已关闭", false)
  end

  -- 输入电平自诊断: 录了 1.5 秒还是 0 = 麦克风没进数据, 直接说出来
  local pk = app.rec and app.rec.peak and app.rec.peak() or 0
  if pk > 0 then
    app.mix(x0, y1 + 8, "电平 " .. tostring(pk), false)
    app.mixc(W // 2, y1 + 8, "再按「停止」保存", false)
  elseif ms > 1500 then
    app.mixc(W // 2, y1 + 8, "! 无输入信号", true)
  else
    app.mixc(W // 2, y1 + 8, "正在拾音…", false)
  end
  button(120, 160, "停止录音", true)
end

-- 保存中（C 侧任务在回填 WAV 头，通常几十毫秒）
local function draw_saving()
  header("保存中", "")
  app.mixc(W // 2, 120, "正在写入文件…", false)
  app.bigc(W // 2, 150, fmt_ms(G.rec_ms), 4, false)
end

-- 设置页：采样率 / 频谱开关
local function draw_settings()
  header("设置", "")
  local items = {
    { "采样率", (G.rate == 8000) and "8 kHz" or "16 kHz" },
    { "录音频谱", G.show_spec and "开" or "关" },
    { "文件数", tostring(G.n) .. " 条" },
    { "存储位置", "/sdcard/Rec/" },
  }
  local y = SB_H + 46
  for i = 1, #items do
    local it = items[i]
    local sel = (i == G.set_sel)
    if sel then app.fill(6, y - 4, W - 7, y + 20, BK) end
    app.mix(16, y, it[1], sel)
    app.mix(W - 16 - app.mixw(it[2]), y, it[2], sel)
    y = y + 34
  end
  app.hline(6, W - 7, H - 44, BK)
  app.mix(12, H - 38, "左右改值  BACK 返回", false)
end

local function draw_play()
  header("播放", G.play_name)
  local ms = app.rec and app.rec.play_ms() or 0
  local tot = app.rec and app.rec.play_total_ms() or 0
  app.bigc(W // 2, 52, fmt_ms(ms), 5, false)
  app.mixc(W // 2, 132, "/ " .. fmt_ms(tot), false)

  -- 播放时也画频谱（同一个 C 侧频谱，播放任务里算）
  local x0, y0, x1, y1 = 16, 166, 384, 226
  if G.show_spec and app.rec and app.rec.spec then
    local nb = app.rec.spec_bins and app.rec.spec_bins() or 16
    if nb < 1 then nb = 16 end
    local bw = (x1 - x0 + 1) // nb
    for i = 1, nb do
      local v = app.rec.spec(i) or 0
      if v > 100 then v = 100 end
      local h = (y1 - y0) * v // 100
      if h < 1 then h = 1 end
      local bx = x0 + (i - 1) * bw
      app.fill(bx, y1 - h, bx + bw - 3, y1, BK)
    end
    app.hline(x0, x1, y1, BK)
  end

  -- 进度条压在底部
  local py = 234
  app.rect(x0, py, x1, py + 8, BK)
  if tot > 0 then
    local fw = (x1 - x0 - 4) * math.min(ms, tot) // tot
    if fw > 0 then app.fill(x0 + 2, py + 2, x0 + 1 + fw, py + 6, BK) end
  end

  button(120, 160, "停止", true)
end

local function draw_rename()
  app.fill(0, 0, W - 1, G.kb_y - 4, WH)
  app.mix(8, 6, "改名为", false)
  app.rect(8, 28, W - 9, 60, BK)
  app.mix(14, 36, G.rn_buf .. "_", false)
  app.mix(8, 66, "确定=回车  取消=ESC", false)
  if G.rn_from ~= "" then
    local base = G.rn_from:match("([^/]+)$") or G.rn_from
    app.mix(8, 84, "原名 " .. base, false)
  end
  app.kbd.draw(G.kb_idx)
end

-- ================= 契约函数 =================

function render()
  app.clear()
  if G.mode == "rename" then
    draw_rename()
  elseif G.mode == "rec" then
    draw_rec()
  elseif G.mode == "play" then
    draw_play()
  elseif G.mode == "saving" then
    draw_saving()
  elseif G.mode == "settings" then
    draw_settings()
  else
    draw_list()
  end
  -- 提示条（改名页不显示，键盘已经占满下半屏）
  if G.mode ~= "rename" and G.msg ~= "" and (now_ms() - (G.msg_t or 0)) < 2200 then
    local wpx = app.mixw(G.msg) + 12
    local x0 = (W - wpx) // 2
    local y0 = G.kb_y - 26
    if G.mode ~= "rename" then y0 = LIST_B - 26 end
    app.fill(x0, y0, x0 + wpx, y0 + 20, WH)
    app.rect(x0, y0, x0 + wpx, y0 + 20, BK)
    app.mix(x0 + 6, y0 + 2, G.msg, false)
  end
end

function on_enter()
  G.mode = "list"
  G.confirm_del = false
  G.msg = ""
  -- 打开就检测麦克风：标准扩展板 i2s_din = -1，物理上没有输入脚
  G.no_mic = false
  if app.rec and app.rec.mic_available then
    G.no_mic = not app.rec.mic_available()
  end
  scan()
  if G.no_mic then msg("未检测到麦克风") end
  app.redraw()
end

function on_exit()
  if app.rec then
    if app.rec.playing() then app.rec.stop_play() end
    if app.rec.active() then app.rec.stop() end
  end
  G.files = {}
end

-- 只在"显示值真的变了"时重画, 避免 poll 每帧都刷屏
local function ui_key()
  local m = app.rec and app.rec.ms() or 0
  local pm = app.rec and app.rec.play_ms() or 0
  local pl = (app.rec and app.rec.playing()) and 1 or 0
  return table.concat({
    G.mode, G.sel, G.top, G.n, G.confirm_del and 1 or 0,
    m // 500, pm // 500, pl,
    G.play_name, G.msg, G.rn_buf, G.kb_idx,
  }, "|")
end

-- 触摸：按钮栏四个按钮 + 列表点名。用"按下沿"判定，按住不会连发。
local function hit_button(x, y)
  if y < BTN_Y or y > BTN_Y + BTN_H then return nil end
  if x >= 4   and x <= 80  then return 1 end
  if x >= 84  and x <= 160 then return 2 end
  if x >= 164 and x <= 240 then return 3 end
  if x >= 244 and x <= 320 then return 4 end
  if x >= 324 and x <= 398 then return 5 end
  return nil
end

local function on_tap(x, y)
  if G.mode == "rec" then
    if hit_button(x, y) or y > BTN_Y then stop_rec() end
    return
  end
  if G.mode == "play" then
    if hit_button(x, y) or y > BTN_Y then
      if app.rec then app.rec.stop_play() end
      G.mode = "list"; G.play_name = ""; app.redraw()
    end
    return
  end
  if G.mode ~= "list" then return end

  local b = hit_button(x, y)
  if b == 1 then
    if app.rec and app.rec.active() then stop_rec() else start_rec() end
    return
  elseif b == 2 then
    toggle_play(); return
  elseif b == 3 then
    enter_rename(); return
  elseif b == 4 then
    do_delete(); return
  elseif b == 5 then
    G.mode = "settings"; G.set_sel = 1; app.redraw(); return
  end

  -- 点列表行
  if y >= LIST_Y and y < LIST_B then
    local r = (y - LIST_Y) // ROW_H
    local i = G.top + r
    if i >= 1 and i <= G.n then
      G.sel = i
      G.confirm_del = false
      app.redraw()
    end
  end
end

function poll()
  if app.touch then
    local tx, ty = app.touch()
    if tx then
      if not G.tdown then
        G.tdown = true
        on_tap(tx, ty)
      end
    else
      G.tdown = false
    end
  end

  if G.mode == "saving" then
    if not app.rec or not app.rec.active() then
      G.mode = "list"
      G.saving = false
      local ms = G.rec_ms
      G.rec_ms = 0
      scan()
      msg("已保存 " .. fmt_ms(ms))
    elseif now_ms() - (G.save_t or 0) > 4000 then
      G.mode = "list"; G.saving = false
      scan(); msg("停止超时")
    end
  end

  if G.mode == "rec" and app.rec then
    G.rec_ms = app.rec.ms()
    if not app.rec.active() then
      -- 任务自己结束了(写卡失败等) → 回到列表并刷新
      G.mode = "list"
      scan()
      msg("录音已结束")
    end
  elseif G.mode == "play" and app.rec then
    if not app.rec.playing() then
      G.mode = "list"
      G.play_name = ""
      app.redraw()
    end
  end

  local k = ui_key()
  if k ~= G.key then
    G.key = k
    app.redraw()
  end
end

function action(a)
  if type(a) ~= "number" then return true end

  if G.mode == "rename" then
    if a == A_UP then kb_move(0, -1)
    elseif a == A_DOWN then kb_move(0, 1)
    elseif a == A_LEFT then kb_move(-1, 0)
    elseif a == A_RIGHT then kb_move(1, 0)
    elseif a == A_OK then kb_tap()
    elseif a == A_BACK then G.mode = "list"; app.redraw() end
    return true
  end

  if G.mode == "settings" then
    if a == A_UP then
      if G.set_sel > 1 then G.set_sel = G.set_sel - 1 end
    elseif a == A_DOWN then
      if G.set_sel < 4 then G.set_sel = G.set_sel + 1 end
    elseif a == A_LEFT or a == A_RIGHT then
      if G.set_sel == 1 then
        G.rate = (G.rate == 8000) and 16000 or 8000
      elseif G.set_sel == 2 then
        G.show_spec = not G.show_spec
      end
    elseif a == A_BACK then
      G.mode = "list"
    end
    app.redraw()
    return true
  end

  if G.mode == "saving" then return true end

  if G.mode == "rec" then
    if a == A_OK or a == A_BACK then stop_rec() end
    return true
  end

  if G.mode == "play" then
    if a == A_OK or a == A_BACK then
      if app.rec then app.rec.stop_play() end
      G.mode = "list"; G.play_name = ""
      app.redraw()
    end
    return true
  end

  -- 列表
  G.confirm_del = false
  if a == A_UP then
    if G.sel > 1 then G.sel = G.sel - 1 end
  elseif a == A_DOWN then
    if G.sel < G.n then G.sel = G.sel + 1 end
  elseif a == A_OK then
    toggle_play()
  elseif a == A_LEFT then
    enter_rename()
  elseif a == A_RIGHT then
    do_delete()
  elseif a == A_BACK then
    app.exit()
  end
  app.redraw()
  return true
end
