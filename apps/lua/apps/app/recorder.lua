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
  mode = "list",              -- list / rec / play / rename
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

local function next_name()
  local mx = 0
  for i = 1, G.n do
    local k = G.files[i].name:match("rec_(%d+)")
    if k then
      k = tonumber(k)
      if k and k > mx then mx = k end
    end
  end
  return string.format("rec_%03d.wav", mx + 1)
end

local function cur() return G.files[G.sel] end

-- ================= 动作 =================

local function start_rec()
  if not app.rec then msg("固件不支持录音") return end
  local name = next_name()
  local rc = app.rec.start(DIR .. name, 16000)
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
  local ms = app.rec.stop()
  G.mode = "list"
  G.rec_ms = 0
  scan()
  msg("已保存 " .. fmt_ms(ms))
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
    app.mixc(W // 2, LIST_Y + 70, "还没有录音", false)
    app.mixc(W // 2, LIST_Y + 96, "点「录音」开始", false)
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

  app.hline(0, W - 1, LIST_B + 2, BK)
  button(4,   94, (G.mode == "rec") and "停止" or "录音", G.mode == "rec")
  button(104, 94, app.rec and app.rec.playing() and "停止" or "播放", false)
  button(204, 94, "改名", false)
  button(304, 92, G.confirm_del and "确认删" or "删除", G.confirm_del)
end

local function draw_rec()
  header("录音中", app.rec and "16kHz WAV" or "")
  local ms = app.rec and app.rec.ms() or G.rec_ms
  app.bigc(W // 2, 74, fmt_ms(ms), 5, false)

  -- 音量示意: 用已写字节的增长速率画一根跳动条
  local bytes = app.rec and app.rec.bytes and app.rec.bytes() or 0
  local lvl = (bytes // 4096) % 20
  if lvl > 16 then lvl = 32 - lvl end
  local bw = 300
  local x0 = (W - bw) // 2
  app.rect(x0, 168, x0 + bw, 186, BK)
  local fw = (bw - 4) * lvl // 16
  if fw > 0 then app.fill(x0 + 2, 170, x0 + 1 + fw, 184, BK) end

  app.mixc(W // 2, 200, "再按「停止」保存", false)
  button(120, 160, "停止录音", true)
end

local function draw_play()
  header("播放中", G.play_name)
  local ms = app.rec and app.rec.play_ms() or 0
  local tot = app.rec and app.rec.play_total_ms() or 0
  app.bigc(W // 2, 70, fmt_ms(ms), 5, false)
  app.mixc(W // 2, 150, "/ " .. fmt_ms(tot), false)

  local bw = 320
  local x0 = (W - bw) // 2
  app.rect(x0, 178, x0 + bw, 194, BK)
  if tot > 0 then
    local fw = (bw - 4) * math.min(ms, tot) // tot
    if fw > 0 then app.fill(x0 + 2, 180, x0 + 1 + fw, 192, BK) end
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
  scan()
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
  if x >= 4   and x <= 98  then return 1 end
  if x >= 104 and x <= 198 then return 2 end
  if x >= 204 and x <= 298 then return 3 end
  if x >= 304 and x <= 396 then return 4 end
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
