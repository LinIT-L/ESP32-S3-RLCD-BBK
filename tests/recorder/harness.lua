-- tests/recorder/harness.lua —— 录音机脚本的主机侧桩
--
-- 为什么这样做: 录音机是纯 Lua 应用 (store/pkgs/录音机.lua), 逻辑 bug 都出在
-- 布局/命中/确认流程上。真机验证成本高且不可回归, 所以在电脑上用 Lua 运行时
-- **原样加载生产脚本**, 把 app.* 换成桩: 绘制调用全部记录坐标, 触摸/弹窗结果可控。
-- 测的是生产代码本身, 不是复制一份"看起来一样"的实现。
--
-- 约定:
--   · 所有绘制记录进 REC.fills (统一 {x0,y0,x1,y1}), 用来断言"没有画到状态栏 (y<24)"。
--   · 文字记录进 REC.texts, 用来断言文案/页面切换。
--   · app.millis 返回虚拟时钟 CLOCK, 由测试驱动推进 (不自己走, 保证可重复)。

REC = {
  fills = {}, texts = {}, dlg = {}, removed = {}, played = {},
  rec_started = 0, stopped = 0, stopped_play = 0, redraws = 0,
  kbd_draw = 0, statusbar = nil, exit = false, toast = "",
}
FS_LIST, FS_SIZE, FS_DATA = {}, {}, {}
TOUCH_Q, DLG_Q = {}, {}
REC_STATE = { active = false, playing = false }
CLOCK = 0
-- 按住时长 (ms): 由测试推进, 供"长按"路径用。
-- 生产脚本写的是 `(app.touch_hold and app.touch_hold()) or (now - P.t0)`;
-- Lua 里 0 是真值 ⇒ 桩恒返回 0 就等于"永远不说按了多久", 长按路径永远进不去。
HOLD_MS = 0

local function u16(v) return string.char(v % 256, math.floor(v / 256) % 256) end
local function u32(v) return u16(v % 65536) .. u16(math.floor(v / 65536) % 65536) end

-- 造一条"头已收尾"的 WAV: dl = PCM 数据字节数
function H_add_wav(name, dl)
  local p = "/sdcard/Rec/" .. name
  local hdr = "RIFF" .. u32(36 + dl) .. "WAVE" .. "fmt " .. u32(16)
           .. u16(1) .. u16(2) .. u32(16000) .. u32(64000) .. u16(4) .. u16(16)
           .. "data" .. u32(dl)
  FS_LIST[#FS_LIST + 1] = name
  FS_SIZE[p] = 44 + dl
  FS_DATA[p] = hdr
end

local function rec_fill(x0, y0, x1, y1)
  REC.fills[#REC.fills + 1] = { x0 = x0, y0 = y0, x1 = x1, y1 = y1 }
end

app = {}
app.millis = function() return CLOCK end
app.redraw = function() REC.redraws = REC.redraws + 1 end
app.fill   = function(x0, y0, x1, y1) rec_fill(x0, y0, x1, y1) end
app.rect   = function(x0, y0, x1, y1) rec_fill(x0, y0, x1, y1) end
app.hline  = function(x0, x1, y) rec_fill(x0, y, x1, y) end
app.px     = function() end
app.mix    = function(x, y, s) REC.texts[#REC.texts + 1] = { x = x, y = y, s = s } end
app.mixc   = function(x, y, s) REC.texts[#REC.texts + 1] = { x = x, y = y, s = s } end
app.bigc   = function(x, y, s) REC.texts[#REC.texts + 1] = { x = x, y = y, s = s } end
app.textsb = function(x, y, s) REC.texts[#REC.texts + 1] = { x = x, y = y, s = s } end
app.mixw   = function(s) return #s * 8 end
app.statusbar   = function(on) REC.statusbar = (on == nil) and true or (on and true or false) end
app.statusbar_h = function() return 24 end
app.exit = function() REC.exit = true end
app.volume = function() return 50 end

app.rec = {
  active  = function() return REC_STATE.active end,
  start   = function(p) REC_STATE.active = true; REC.rec_started = REC.rec_started + 1; return 0 end,
  stop    = function() REC_STATE.active = false; REC.stopped = REC.stopped + 1 end,
  playing = function() return REC_STATE.playing end,
  play    = function(p) REC_STATE.playing = true; REC.played[#REC.played + 1] = p; return 0 end,
  stop_play = function() REC_STATE.playing = false; REC.stopped_play = REC.stopped_play + 1 end,
  play_ms = function() return 0 end,
  play_total_ms = function() return 1000 end,
  ms = function() return 0 end,
  bytes = function() return 0 end,
  mic_available = function() return true end,
  spec_bins = function() return 16 end,
  spec = function() return 0 end,
  mplay = function() return 0 end,
  mstop = function() end,
  mplaying = function() return false end,
}

app.fs = {
  listdir = function(d) return FS_LIST end,
  size    = function(p) return FS_SIZE[p] or 0 end,
  read_at = function(p, off, n) return FS_DATA[p] end,
  remove  = function(p)
    REC.removed[#REC.removed + 1] = p
    FS_SIZE[p] = nil
    FS_DATA[p] = nil
    local nm = p:match("([^/]+)$")
    for i = #FS_LIST, 1, -1 do
      if FS_LIST[i] == nm then table.remove(FS_LIST, i) end
    end
    return true
  end,
  mkdir   = function() return true end,
  exists  = function(p) return FS_SIZE[p] ~= nil end,
  rename  = function(a, b) REC.renamed = b; return true end,
}

app.dlg = {
  confirm = function(m)
    REC.dlg[#REC.dlg + 1] = m
    REC.dlgkind = "confirm"
  end,
  -- app.dlg.list(title, items[, sel]) —— 系统统一列表选择弹窗 (长按菜单走这条)
  list = function(t, items)
    REC.dlg[#REC.dlg + 1] = t
    REC.dlgkind = "list"
    REC.items = {}
    for i = 1, #items do REC.items[i] = items[i] end
  end,
  toast   = function(t) REC.toast = t end,
  result  = function()
    local v = DLG_Q[1]
    if v == nil then return nil end
    table.remove(DLG_Q, 1)
    return v
  end,
}

app.touch = function()
  local v = TOUCH_Q[1]
  if v == nil then return nil end
  table.remove(TOUCH_Q, 1)
  return v[1], v[2]
end
app.touch_hold = function() return HOLD_MS end

app.kbd = {
  use   = function() end,
  draw  = function() REC.kbd_draw = REC.kbd_draw + 1 end,
  hit   = function() return -1 end,
  press = function() return 0 end,
  char  = function() return "" end,
}

app.UP, app.DOWN, app.LEFT, app.RIGHT, app.CONFIRM, app.BACK = 1, 2, 3, 4, 5, 6
