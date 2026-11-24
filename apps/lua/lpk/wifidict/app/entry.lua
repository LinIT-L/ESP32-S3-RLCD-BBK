--[[ ============================================================================
 Wi-Fi 字典工具 — LinTOS 2.0 (ESP32-S3 / 400x300 1bpp) 单文件 Lua 应用

 定位: **离线**字典文件管理 + 随机测试字符串生成。
   · 不调用任何 app.wifi.* / app.net.* 接口, 不做 Wi-Fi 扫描, 不连网;
   · 不实现针对真实 Wi-Fi 网络的自动密码爆破或批量认证尝试;
   · 只管 TF 卡上的 .txt 字典文件: 选 / 导入 / 预览 / 改名 / 删除 / 生成。

 真相源 (本文件用到的全部接口, 均在 components/apps/lua_bridge/luat_lib_*.c 注册):
   app.clear/mix/mixc/mixw/texts/rect/fill/hline          luat_lib_gfx.c
   app.millis/touch/touch_hold/redraw                      luat_lib_input.c
   app.dlg.list/confirm/result                            luat_lib_sys.c
   app.fs.exists/read_at/write/append/remove/rename/size/
        mkdir/listdir                                      luat_lib_fs.c
   app.nvs.get_str/set_str                                luat_lib_store.c
   app.kbd.use/draw/hit/press/char/CHAR/BKSP/SPACE/
        ENTER/ESC                                         luat_lib_kbd.c
   app.util.rand                                          luat_lib_util.c (硬件 TRNG)
   app.exit                                               luat_lib_sys.c

 分批: 导入/统计/预览用 read_at 32KB/16KB 分块; 写入用 append 4KB~8KB 分批。
 每帧只推进一步, 不阻塞主循环 (poll 状态机), render 有 dirty + 时间闸门。
============================================================================ ]]

local W, H = app.W, app.H
local BK, WH = app.BLACK, app.WHITE
local A_UP    = (type(app.UP) == "number")    and app.UP    or 1
local A_DOWN  = (type(app.DOWN) == "number")  and app.DOWN  or 2
local A_LEFT  = (type(app.LEFT) == "number")  and app.LEFT  or 3
local A_RIGHT = (type(app.RIGHT) == "number") and app.RIGHT or 4
local A_OK    = (type(app.OK) == "number")    and app.OK    or 5
local A_BACK  = (type(app.BACK) == "number")  and app.BACK  or 6

-- 目录: 所有字典都放在这里, 一个 .txt = 一个字典, 文件名即字典名
local DIR = "/sdcard/wifidict"
local SRC_DIRS = { "/sdcard", "/sdcard/download", "/sdcard/txt" }
local CHUNK       = 32768     -- 读块 (read_at 上限 1MB, 这里取 32KB)
local PREV_CHUNK  = 16384     -- 预览读块
local WRITE_CHUNK = 4096      -- 导入写块
local GEN_BATCH   = 256       -- 每帧生成条数
local DEDUP_MAX   = 50000     -- 全局去重表条目上限 (PSRAM 保护)
local PAGE_ROWS   = 8
local QTY         = { 100, 500, 1000, 5000, 10000, 50000, 100000 }
local SPECIAL     = "!@#$%^&*()-_=+.,?"

-- 布局 (Lua 页是 400x300 全屏页, 顶部状态栏由系统画)
-- 骨架照 iOS: 状态栏 → Large Title / 导航栏 → inset-grouped 圆角卡片 → 底部 tab bar
-- ★ 2026-11-23: 高度向 OS 要 (app.statusbar_h), 原写死 20 比系统 24 少 4px,
--   顶栏第一行会被状态栏压掉; 并且把"要不要状态栏"改成声明式 (render 里 app.statusbar(true))。
local SB_H             = (app.statusbar_h and app.statusbar_h()) or 24
local TITLE_Y          = SB_H + 4  -- Large Title (zh24) 顶边
local CARD_X0, CARD_X1 = 14, 386
local CARD_T, CARD_B   = 52, 246
local ROW_H, VIS       = 37, 5
local NAV_T, NAV_B     = SB_H, 52 -- 次级页导航栏
local TAB_T            = 248    -- 底部 tab bar 顶线
local TAB_W            = 100
local PREV_H           = 20     -- 预览行高
local GEN_T, GEN_RH    = 76, 29 -- 生成页卡片顶 / 行高 (7 行)

local G = {
  mode = "list",            -- list | gen | preview | text | busy | result
  dicts = {}, sel = 1, top = 1, cur = "",
  dirty = true, last_draw = -1000,     -- 保证进页面第一帧必画
  msg = "", msg_until = 0,
  dlg = nil, next_dlg = nil,
  job = nil,
  result = nil,
  gen = { digit = true, lower = true, upper = false, special = false,
          custom = "", len = 8, qty = 3 },
  gen_sel = 1,
  text = nil,
  preview = nil,
  src_paths = nil, pending_src = nil, pending_stem = nil,
}
local P = { on = false, x = 0, y = 0, t0 = 0, moved = false }

-- ------------------------------------------------------------------ 小工具
local function dirty()
  G.dirty = true
  app.redraw()
end

-- 随机源: 优先固件硬件 TRNG (app.util.rand, luat_lib_util.c 已注册);
-- 老固件 / 主机模拟器没有该子表时退到 Lua 标准库, 结果页会如实标注。
-- ⚠ 用"存在性"判断而不是 type()=="function": 主机模拟器里 Python 函数在 Lua 侧是
--   userdata, type() 判断会误判成"没有 TRNG"。
local RAND_SRC = "app.util.rand (硬件 TRNG)"
local function rand_int(n)
  local u = app.util
  if u and u.rand then
    return u.rand(1, n)
  end
  RAND_SRC = "math.random (固件未提供 TRNG)"
  return math.random(1, n)
end

local function toast(s, ms)
  G.msg = s
  G.msg_until = app.millis() + (ms or 2200)
  dirty()
end

local function fmt_size(n)
  n = n or 0
  if n < 1024 then return tostring(n) .. " B" end
  if n < 1048576 then return string.format("%.1f KB", n / 1024) end
  return string.format("%.2f MB", n / 1048576)
end

-- 按 UTF-8 字符安全截断到 maxbytes 字节 (app.dlg.list 每项 <= 29 字节)
local function u8cut(s, n)
  if #s <= n then return s end
  local i = 1
  while i <= #s do
    local b = s:byte(i)
    local k = 1
    if b >= 0xF0 then k = 4 elseif b >= 0xE0 then k = 3 elseif b >= 0xC0 then k = 2 end
    if i + k - 1 > #s then k = 1 end
    if i + k - 1 > n then break end
    i = i + k
  end
  return s:sub(1, i - 1)
end

-- 按显示宽度截断 (app.mix: 汉字 16px / ASCII 8px), 不依赖 mixw 便于主机模拟
local function fit(s, maxw)
  s = s or ""
  local out, w, i = {}, 0, 1
  while i <= #s do
    local b = s:byte(i)
    local k = 1
    if b >= 0xF0 then k = 4 elseif b >= 0xE0 then k = 3 elseif b >= 0xC0 then k = 2 end
    if i + k - 1 > #s then k = 1 end
    local cw = (b < 128) and 8 or 16
    if w + cw > maxw then break end
    out[#out + 1] = s:sub(i, i + k - 1)
    w = w + cw
    i = i + k
  end
  return table.concat(out)
end

-- 只去 CR/LF 与首尾空白, 保留条目内部字符 (含 UTF-8 多字节)
local function trim_line(s)
  s = s:gsub("^[\r\n\t ]+", "")
  s = s:gsub("[\r\n\t ]+$", "")
  return s
end

-- 预览显示: 控制字符替换成 '?', 其余字节照原样交给混排字模
local function show_safe(s)
  return (s:gsub("[%c]", "?"))
end

-- 文件名安全化: 只留 ASCII 字母数字 - _ (避免 FAT LFN / 路径注入问题)
local function safe_stem(s)
  local t = {}
  for i = 1, #s do
    local c = s:sub(i, i)
    if c:match("[%w%-_]") then t[#t + 1] = c else t[#t + 1] = "_" end
  end
  local r = table.concat(t)
  if r == "" then r = "dict" end
  return r:sub(1, 24)
end

local function strip_txt(s)
  local low = s:lower()
  if low:sub(-4) == ".txt" then return s:sub(1, #s - 4) end
  return s
end

-- ------------------------------------------------------------------ 数据层
local function dir_ensure()
  if not app.fs.exists(DIR) then
    if not app.fs.mkdir(DIR) then return false end
  end
  return true
end

local function seed_builtin()
  -- 目录为空 (新卡 / 用户删空) 才写入内置示例字典; 之后它们是普通 .txt,
  -- 可以照常预览 / 改名 / 删除, 不会被反复重建。
  local n = app.fs.listdir(DIR) or {}
  if #n > 0 then return end
  local sets = {}
  local a = {}
  for i = 0, 199 do a[#a + 1] = string.format("%06d", i) end
  sets[#sets + 1] = { "示例-数字6位.txt", a }
  local b = {}
  for i = 1, 100 do b[#b + 1] = string.format("test%04d", i) end
  sets[#sets + 1] = { "示例-字母数字.txt", b }
  local c, pool = {}, "Aa0!"
  for i = 1, 60 do
    c[#c + 1] = string.format("%s%04d", pool, i)
  end
  sets[#sets + 1] = { "示例-混合字符.txt", c }
  for _, s in ipairs(sets) do
    app.fs.write(DIR .. "/" .. s[1], table.concat(s[2], "\n") .. "\n")
  end
end

local function scan_dicts()
  G.dicts = {}
  local names = app.fs.listdir(DIR) or {}
  table.sort(names)
  for _, nm in ipairs(names) do
    local low = nm:lower()
    if low:sub(-4) == ".txt" then
      local p = DIR .. "/" .. nm
      local sz = app.fs.size(p)
      if sz then
        G.dicts[#G.dicts + 1] = {
          name = strip_txt(nm), file = nm, path = p, size = sz, count = nil,
        }
      end
    end
  end
  if G.sel > #G.dicts then G.sel = #G.dicts end
  if G.sel < 1 then G.sel = 1 end
  if G.top > G.sel then G.top = G.sel end
  if #G.dicts > 0 and G.top < 1 then G.top = 1 end
  local found = false
  for _, d in ipairs(G.dicts) do
    if d.file == G.cur then found = true break end
  end
  if not found then G.cur = "" end
  dirty()
end

local function unique_stem(base)
  local stem = base
  local i = 2
  while app.fs.exists(DIR .. "/" .. stem .. ".txt") and i < 100 do
    stem = base .. "_" .. tostring(i)
    i = i + 1
  end
  return stem
end

local function set_current()
  local d = G.dicts[G.sel]
  if not d then toast("没有字典") return end
  G.cur = d.file
  app.nvs.set_str("wdcur", d.file)
  toast("当前字典: " .. d.name)
end

local function move_sel(delta)
  if #G.dicts == 0 then return end
  G.sel = math.max(1, math.min(#G.dicts, G.sel + delta))
  if G.sel < G.top then G.top = G.sel end
  if G.sel >= G.top + VIS then G.top = G.sel - VIS + 1 end
  dirty()
end

-- ------------------------------------------------------------------ 预览读取器
-- 逐块顺序读, 只保留当前缓冲 + 最近 12 页, 内存与文件大小无关
local function reader_reset(path)
  G.preview = {
    path = path, off = 0, data = "", eof = false,
    pages = {}, page = 1, seen = 0,
  }
end

local function reader_line()
  local R = G.preview
  if not R then return nil end
  while true do
    local p = R.data:find("\n", 1, true)
    if p then
      local ln = R.data:sub(1, p - 1)
      R.data = R.data:sub(p + 1)
      return trim_line(ln)
    end
    if R.eof then
      if R.data ~= "" then
        local ln = R.data
        R.data = ""
        return trim_line(ln)
      end
      return nil
    end
    local blk = app.fs.read_at(R.path, R.off, PREV_CHUNK)
    if not blk or #blk == 0 then
      R.eof = true
    else
      R.off = R.off + #blk
      R.data = R.data .. blk
    end
    -- 病态文件 (整文件无换行) 兜底: 不让缓冲无限增长
    if #R.data > 65536 then
      local ln = R.data:sub(1, 1024)
      R.data = ""
      return ln
    end
  end
end

local function preview_next()
  local R = G.preview
  if not R then return end
  if R.pages[R.page + 1] then
    R.page = R.page + 1
    dirty()
    return
  end
  local lines = {}
  for i = 1, PAGE_ROWS do
    local ln = reader_line()
    if not ln then break end
    if ln ~= "" then lines[#lines + 1] = ln end
  end
  if #lines == 0 then toast("已到文件末尾") return end
  R.pages[#R.pages + 1] = { lines = lines, first = R.seen + 1 }
  R.seen = R.seen + #lines
  R.page = #R.pages
  if #R.pages > 12 then
    table.remove(R.pages, 1)
    R.page = R.page - 1
  end
  dirty()
end

local function preview_prev()
  local R = G.preview
  if not R then return end
  if R.page > 1 then
    R.page = R.page - 1
    dirty()
  else
    toast("已到最早缓存页")
  end
end

local function open_preview()
  local d = G.dicts[G.sel]
  if not d then toast("没有字典") return end
  reader_reset(d.path)
  G.mode = "preview"
  preview_next()
  dirty()
end

-- ------------------------------------------------------------------ 任务状态机
local function job_fail(msg)
  local j = G.job
  if j and j.dst then app.fs.remove(j.dst) end
  G.job = nil
  G.result = { title = "操作失败", rows = { msg, "已清理半成品文件" } }
  G.mode = "result"
  dirty()
end

local function flush_job_buf()
  local j = G.job
  if not j or j.buf == "" then return true end
  if not app.fs.append(j.dst, j.buf) then return false end
  j.bytes = j.bytes + #j.buf
  j.buf = ""
  return true
end

local function start_count_job()
  -- 条数统计也分批: 每帧只读一个 32KB 块, 列表先显示 "..."
  if #G.dicts == 0 then return end
  G.job = { kind = "count", idx = 1, off = 0, tail = "", n = 0 }
end

local function step_count()
  local j = G.job
  local d = G.dicts[j.idx]
  if not d then
    G.job = nil
    dirty()
    return
  end
  local blk = app.fs.read_at(d.path, j.off, CHUNK)
  if not blk or #blk == 0 then
    if j.tail ~= "" then j.n = j.n + 1 end
    d.count = j.n
    j.idx = j.idx + 1
    j.off, j.tail, j.n = 0, "", 0
    if j.idx > #G.dicts then G.job = nil end
    dirty()
    return
  end
  j.off = j.off + #blk
  local data = j.tail .. blk
  local from = 1
  while true do
    local p = data:find("\n", from, true)
    if not p then break end
    if trim_line(data:sub(from, p - 1)) ~= "" then j.n = j.n + 1 end
    from = p + 1
  end
  j.tail = data:sub(from)
  if #j.tail > CHUNK then j.n = j.n + 1 j.tail = "" end
  dirty()
end

local function finish_import()
  local j = G.job
  if not j then return end
  if not flush_job_buf() then job_fail("写入 TF 卡失败") return end
  -- size() 失败或返回 0 时退回本次实际写入字节数 (有些文件系统刚 append 完取不到大小)
  local sz = app.fs.size(j.dst)
  if not sz or sz == 0 then sz = j.bytes end
  local rows = {
    "来源: " .. j.srcname,
    "字典: " .. j.stem .. ".txt",
    "导入条数: " .. tostring(j.n) .. " 条",
    "文件大小: " .. fmt_size(sz),
    "编码: UTF-8 / LF / CRLF 自动兼容",
  }
  if j.dedup then
    rows[#rows + 1] = "去重: 已开启 (去除空行与重复条目)"
    if j.dedup_limited then
      rows[#rows + 1] = "注意: 超过 " .. tostring(DEDUP_MAX) .. " 条后未再全局去重"
    end
  else
    rows[#rows + 1] = "去重: 未开启 (大文件模式)"
  end
  G.job = nil
  G.result = { title = "导入完成", rows = rows }
  G.mode = "result"
  scan_dicts()
  start_count_job()
  dirty()
end

local function finish_gen()
  local j = G.job
  if not j then return end
  local sz = app.fs.size(j.dst)
  if not sz or sz == 0 then sz = j.bytes end
  G.job = nil
  G.result = {
    title = "生成完成",
    rows = {
      "字典: " .. j.stem .. ".txt",
      "生成数量: " .. tostring(j.made) .. " 条",
      "字符串长度: " .. tostring(j.len),
      "字符集大小: " .. tostring(#j.cs) .. " 种字符",
      "文件大小: " .. fmt_size(sz),
      "随机源: " .. RAND_SRC,
    },
  }
  G.mode = "result"
  scan_dicts()
  start_count_job()
  dirty()
end

local function begin_import(src, stem, dedup)
  local dst = DIR .. "/" .. stem .. ".txt"
  if not app.fs.write(dst, "") then
    G.result = { title = "导入失败", rows = { "无法创建字典文件", "请检查 TF 卡是否可写" } }
    G.mode = "result"
    dirty()
    return
  end
  G.job = {
    kind = "import", src = src, dst = dst, stem = stem,
    srcname = (src:match("([^/]+)$") or src),
    off = 0, tail = "", n = 0, buf = "", bytes = 0,
    dedup = dedup, seen = {}, dedup_limited = false,
  }
  G.mode = "busy"
  G.busy_title = "正在导入"
  dirty()
end

local function begin_gen(cs, len, total, stem)
  local dst = DIR .. "/" .. stem .. ".txt"
  if not app.fs.write(dst, "") then
    G.result = { title = "生成失败", rows = { "无法创建字典文件", "请检查 TF 卡是否可写" } }
    G.mode = "result"
    dirty()
    return
  end
  G.job = {
    kind = "gen", dst = dst, stem = stem, cs = cs, len = len,
    total = total, made = 0, bytes = 0,
  }
  G.mode = "busy"
  G.busy_title = "正在生成"
  dirty()
end

local function step_import()
  local j = G.job
  local blk = app.fs.read_at(j.src, j.off, CHUNK)
  if not blk or #blk == 0 then
    if j.tail ~= "" then
      local ln = trim_line(j.tail)
      if ln ~= "" then
        local dup = false
        if j.dedup then
          if j.seen[ln] then dup = true
          elseif j.n < DEDUP_MAX then j.seen[ln] = true
          else j.dedup_limited = true end
        end
        if not dup then j.n = j.n + 1 j.buf = j.buf .. ln .. "\n" end
      end
      j.tail = ""
    end
    finish_import()
    return
  end
  j.off = j.off + #blk
  local data = j.tail .. blk
  if j.off == #blk then data = data:gsub("^\239\187\191", "", 1) end   -- UTF-8 BOM
  local from = 1
  while true do
    local p = data:find("\n", from, true)
    if not p then break end
    local ln = trim_line(data:sub(from, p - 1))
    if ln ~= "" then
      local dup = false
      if j.dedup then
        if j.seen[ln] then dup = true
        elseif j.n < DEDUP_MAX then j.seen[ln] = true
        else j.dedup_limited = true end
      end
      if not dup then
        j.n = j.n + 1
        j.buf = j.buf .. ln .. "\n"
      end
    end
    from = p + 1
  end
  j.tail = data:sub(from)
  if #j.tail > CHUNK then j.tail = "" end
  if #j.buf >= WRITE_CHUNK then
    if not flush_job_buf() then job_fail("写入 TF 卡失败") return end
  end
  dirty()
end

local function step_gen()
  local j = G.job
  local batch = math.min(GEN_BATCH, j.total - j.made)
  if batch <= 0 then finish_gen() return end
  local t = {}
  local ncs = #j.cs
  for i = 1, batch do
    local s = {}
    for k = 1, j.len do
      s[k] = j.cs[rand_int(ncs)]
    end
    t[#t + 1] = table.concat(s) .. "\n"
  end
  local chunk = table.concat(t)
  if not app.fs.append(j.dst, chunk) then job_fail("写入 TF 卡失败") return end
  j.made = j.made + batch
  j.bytes = j.bytes + #chunk
  if j.made >= j.total then finish_gen() return end
  dirty()
end

local function step_job()
  local j = G.job
  if not j then return end
  if j.kind == "count" then step_count()
  elseif j.kind == "import" then step_import()
  elseif j.kind == "gen" then step_gen()
  end
end

local function cancel_job()
  local j = G.job
  if j and j.dst then app.fs.remove(j.dst) end
  G.job = nil
  G.mode = "list"
  scan_dicts()
  start_count_job()
  toast("已取消, 半成品已删除")
end

-- ------------------------------------------------------------------ 动作层
local GEN_ROWS = {
  { k = "digit",   label = "数字 0-9",     kind = "bool" },
  { k = "lower",   label = "小写 a-z",     kind = "bool" },
  { k = "upper",   label = "大写 A-Z",     kind = "bool" },
  { k = "special", label = "特殊符号",     kind = "bool" },
  { k = "custom",  label = "自定义字符集", kind = "text" },
  { k = "len",     label = "字符串长度",   kind = "num" },
  { k = "qty",     label = "生成数量",     kind = "qty" },
}

local function gen_value(row)
  local g = G.gen
  if row.kind == "bool" then return g[row.k] and "开" or "关" end
  if row.kind == "text" then
    return (g.custom == "") and "(空, 按 OK 输入)" or fit(g.custom, 120)
  end
  if row.kind == "num" then return tostring(g.len) end
  return tostring(QTY[g.qty])
end

local function open_text(purpose)
  G.text = {
    purpose = purpose, buf = "",
    title = (purpose == "rename") and "重命名字典" or "自定义字符集",
    hint = (purpose == "rename") and "新名称 (字母/数字/-/_)" or "追加字符 (ASCII)",
  }
  app.kbd.use("rows", 4)
  G.mode = "text"
  dirty()
end

local function cancel_text()
  G.text = nil
  G.mode = (G.text_back or "list")
  dirty()
end

local function commit_text()
  local T = G.text
  if not T then return end
  if T.purpose == "charset" then
    G.gen.custom = T.buf
    G.text = nil
    G.mode = "gen"
    toast("自定义字符集已设置")
    return
  end
  local stem = safe_stem(T.buf)
  local d = G.dicts[G.sel]
  if not d then G.text = nil G.mode = "list" return end
  local newf = stem .. ".txt"
  if app.fs.exists(DIR .. "/" .. newf) then toast("该名称已存在") return end
  if not app.fs.rename(d.path, DIR .. "/" .. newf) then toast("重命名失败") return end
  if G.cur == d.file then
    G.cur = newf
    app.nvs.set_str("wdcur", newf)
  end
  G.text = nil
  G.mode = "list"
  scan_dicts()
  start_count_job()
  toast("已重命名为 " .. stem)
end

local function text_key(idx)
  local T = G.text
  if not T then return end
  local act = app.kbd.press(idx)
  local ch = app.kbd.char(idx)
  if act == app.kbd.CHAR and ch then
    if #T.buf < 24 then T.buf = T.buf .. ch end
  elseif act == app.kbd.SPACE then
    if #T.buf < 24 then T.buf = T.buf .. " " end
  elseif act == app.kbd.BKSP then
    T.buf = T.buf:sub(1, -2)
  elseif act == app.kbd.ENTER then
    commit_text()
    return
  elseif act == app.kbd.ESC then
    cancel_text()
    return
  end
  dirty()
end

local function open_menu()
  if #G.dicts == 0 then toast("没有字典") return end
  app.dlg.list("字典操作", { "预览", "设为当前", "重命名", "删除", "返回" }, 1)
  G.dlg = "menu"
  P.on = false
end

local function ask_delete()
  local d = G.dicts[G.sel]
  if not d then return end
  app.dlg.confirm("删除字典 " .. u8cut(d.name, 18) .. " ?")
  G.dlg = "del"
  P.on = false
end

local function ask_import()
  local paths, names = {}, {}
  for _, dir in ipairs(SRC_DIRS) do
    local ns = app.fs.listdir(dir) or {}
    for _, nm in ipairs(ns) do
      if nm:lower():sub(-4) == ".txt" then
        local p = dir .. "/" .. nm
        if app.fs.size(p) then
          -- 排除本应用目录自己的字典 (同目录不会出现在父目录列表, 这里防重)
          paths[#paths + 1] = p
          names[#names + 1] = u8cut(nm, 26)
          if #paths >= 60 then break end
        end
      end
    end
    if #paths >= 60 then break end
  end
  if #paths == 0 then
    toast("TF 卡根目录未找到 TXT 文件")
    return
  end
  G.src_paths = paths
  app.dlg.list("选择 TXT 源", names, 1)
  G.dlg = "src"
  P.on = false
end

local function handle_dlg(kind, r)
  if r == -1 then return end                    -- 取消
  if kind == "menu" then
    if r == 1 then open_preview()
    elseif r == 2 then set_current()
    elseif r == 3 then G.text_back = "list" open_text("rename")
    elseif r == 4 then ask_delete() end
  elseif kind == "del" then
    if r == 0 then
      local d = G.dicts[G.sel]
      if d and app.fs.remove(d.path) then
        if G.cur == d.file then G.cur = "" app.nvs.set_str("wdcur", "") end
        scan_dicts()
        start_count_job()
        toast("已删除")
      else
        toast("删除失败")
      end
    end
  elseif kind == "src" then
    if r and r >= 1 and G.src_paths[r] then
      local src = G.src_paths[r]
      G.pending_src = src
      G.pending_stem = unique_stem(safe_stem(strip_txt(src:match("([^/]+)$") or "dict")))
      -- ⚠ 弹窗回调里不能再开弹窗, 隔一帧由 poll 发起
      G.next_dlg = "mode"
    end
  elseif kind == "mode" then
    if r and r >= 1 and G.pending_src then
      begin_import(G.pending_src, G.pending_stem, (r == 1))
      G.pending_src = nil
      G.pending_stem = nil
    end
  end
end

local function build_cs()
  local s = ""
  if G.gen.digit then s = s .. "0123456789" end
  if G.gen.lower then s = s .. "abcdefghijklmnopqrstuvwxyz" end
  if G.gen.upper then s = s .. "ABCDEFGHIJKLMNOPQRSTUVWXYZ" end
  if G.gen.special then s = s .. SPECIAL end
  s = s .. G.gen.custom
  local seen, out = {}, {}
  for i = 1, #s do
    local c = s:sub(i, i)
    if c:byte(1) >= 32 and c:byte(1) < 127 and not seen[c] then
      seen[c] = true
      out[#out + 1] = c
    end
  end
  return out
end

local function begin_gen_from_ui()
  local cs = build_cs()
  if #cs == 0 then toast("请至少选择一组字符") return end
  local stem = unique_stem(string.format("gen%03d", 1))
  begin_gen(cs, G.gen.len, QTY[G.gen.qty], stem)
end

local function gen_adjust(delta)
  local row = GEN_ROWS[G.gen_sel]
  if not row then return end
  local g = G.gen
  if row.kind == "bool" then
    g[row.k] = not g[row.k]
  elseif row.kind == "num" then
    g.len = math.max(1, math.min(32, g.len + delta))
  elseif row.kind == "qty" then
    g.qty = math.max(1, math.min(#QTY, g.qty + delta))
  elseif row.kind == "text" then
    G.text_back = "gen"
    open_text("charset")
    return
  end
  dirty()
end

-- ------------------------------------------------------------------ 绘制层
-- ------------------------------------------------------------------ UI 工具箱
-- iOS 骨架。1bpp 没有灰阶也没有抗锯齿: 圆角 / 胶囊 / 开关全部用整数阶梯近似,
-- 只用 gfx 已有的 px/fill/rect/hline/vline/mix/mixc/texts/zh24/zhs,
-- 不新增任何固件接口, 也不改固件。

-- 16px 混排宽度 (汉字 16 / ASCII 8): 与 app.mix 同源, 但不依赖 app.mixw,
-- 这样主机模拟器不必实现 mixw 也能算出右对齐位置。
local function tw(s)
  s = s or ""
  local w, i = 0, 1
  while i <= #s do
    local b = s:byte(i)
    local k = 1
    if b >= 0xF0 then k = 4 elseif b >= 0xE0 then k = 3 elseif b >= 0xC0 then k = 2 end
    if i + k - 1 > #s then k = 1 end
    w = w + ((b < 128) and 8 or 16)
    i = i + k
  end
  return w
end

-- 右对齐到 xr (右边界不含)
local function draw_right(xr, y, s, inv)
  s = s or ""
  local x = xr - tw(s)
  if x < 0 then x = 0 end
  app.mix(x, y, s, inv)
end

-- 圆角矩形**描边**, 半径可调。卡片用 r=8 (iOS inset-grouped 观感);
-- 22px 高的小控件 (开关/步进器) 用 r=5, 否则圆角阶梯把内部空间吃光。
-- ⚠ 圆角行必须只落**两端各 1px**: 用 hline 画整行会让顶部变成一条实心粗边。
local function round_rect_r(x0, y0, x1, y1, r, inv)
  local c = inv and WH or BK
  app.hline(x0 + r, x1 - r, y0, c)
  app.hline(x0 + r, x1 - r, y1, c)
  app.vline(x0, y0 + r, y1 - r, c)
  app.vline(x1, y0 + r, y1 - r, c)
  for dy = 0, r - 1 do
    local t = r - dy
    local ins = r - math.floor(math.sqrt(r * r - t * t) + 0.5)
    app.px(x0 + ins, y0 + dy, c)
    app.px(x1 - ins, y0 + dy, c)
    app.px(x0 + ins, y1 - dy, c)
    app.px(x1 - ins, y1 - dy, c)
  end
end

local function round_rect(x0, y0, x1, y1, inv)
  round_rect_r(x0, y0, x1, y1, 8, inv)
end

-- 实心圆角块 (胶囊 / 主按钮 / 开关底)。半径默认封顶 6: 22~34px 高的控件已经够圆。
local function fill_round(x0, y0, x1, y1, inv, r)
  local c = inv and WH or BK
  local h = y1 - y0
  if not r then r = math.min(6, math.floor(h / 2)) end
  for dy = 0, r - 1 do
    local t = r - dy
    local ins = r - math.floor(math.sqrt(r * r - t * t) + 0.5)
    app.hline(x0 + ins, x1 - ins, y0 + dy, c)
    app.hline(x0 + ins, x1 - ins, y1 - dy, c)
  end
  app.fill(x0, y0 + r, x1, y1 - r, c)
end

-- iOS 对勾 (单选列表的选中标记), 7px 宽 6px 高
local function checkmark(x, y, inv)
  local c = inv and WH or BK
  app.px(x, y + 3, c) app.px(x + 1, y + 4, c) app.px(x + 2, y + 5, c)
  app.px(x + 3, y + 4, c) app.px(x + 4, y + 3, c) app.px(x + 5, y + 2, c)
  app.px(x + 6, y + 1, c)
end

-- iOS chevron ">" (行有下一级时用), 3px 宽 7px 高
local function chevron(x, cy, inv)
  local c = inv and WH or BK
  for d = 0, 3 do
    app.px(x + d, cy - 3 + d, c)
    app.px(x + d, cy + 3 - d, c)
  end
end

-- iOS 开关 40x22: 开 = 实心黑轨 + 白点靠右; 关 = 描边白轨 + 黑点靠左
-- 滑块 14x14 用三块矩形拼近似圆 (1bpp 上真正的圆只有 8px 以下才不像方块)
local function switch_knob(x, y, c)
  app.fill(x + 2, y, x + 11, y + 13, c)
  app.fill(x, y + 2, x + 13, y + 11, c)
  app.fill(x + 1, y + 1, x + 12, y + 12, c)
end

local function draw_switch(x, y, on)
  if on then
    fill_round(x, y, x + 39, y + 21, false, 5)
    switch_knob(x + 23, y + 4, WH)
  else
    round_rect_r(x, y, x + 39, y + 21, 5)
    switch_knob(x + 3, y + 4, BK)
  end
end

-- iOS 步进器 50x22: [ - ][ + ] 圆角双格
local function draw_stepper(x, y)
  round_rect_r(x, y, x + 49, y + 21, 5)
  app.vline(x + 24, y + 4, y + 17, BK)
  app.texts(x + 9, y + 5, "-")
  app.texts(x + 34, y + 5, "+")
end

-- 导航栏 (次级页): 左文字按钮 / 居中标题 / 右反色胶囊, 底边 1px
local function nav_bar(title, left, right)
  if left then app.mix(16, NAV_T + 8, left) end
  if title and title ~= "" then app.mixc(200, NAV_T + 8, title) end
  if right then
    local w = tw(right) + 22
    local x0 = W - CARD_X0 - w
    fill_round(x0, NAV_T + 4, W - CARD_X0, NAV_T + 28)
    app.mix(x0 + 11, NAV_T + 8, right, true)
  end
  app.hline(0, W - 1, NAV_B, BK)
end

-- iOS 分组小标题 (12px 小中文, 与卡片内文字左对齐)
local function section(y, label)
  app.zhs(CARD_X0 + 6, y, label)
end

-- 胶囊按钮: filled=true 为反色主按钮
local function pill(x0, y0, x1, y1, label)
  fill_round(x0, y0, x1, y1)
  local ty = y0 + math.floor((y1 - y0 - 15) / 2)
  app.mixc(math.floor((x0 + x1) / 2), ty, label, true)
end

local function pill_outline(x0, y0, x1, y1, label)
  round_rect(x0, y0, x1, y1)
  local ty = y0 + math.floor((y1 - y0 - 15) / 2)
  app.mixc(math.floor((x0 + x1) / 2), ty, label)
end

-- 底部 tab bar: 1px 顶线 + 4 格 (12px 图标 + 12px 小中文)
local function tab_icon(kind, cx, y)
  if kind == 1 then                       -- 导入: 下箭头 + 底座
    app.vline(cx, y + 1, y + 7, BK)
    app.px(cx - 3, y + 4, BK) app.px(cx - 2, y + 5, BK) app.px(cx - 1, y + 6, BK)
    app.px(cx + 3, y + 4, BK) app.px(cx + 2, y + 5, BK) app.px(cx + 1, y + 6, BK)
    app.hline(cx - 4, cx + 4, y + 10, BK)
    app.vline(cx - 4, y + 8, y + 9, BK) app.vline(cx + 4, y + 8, y + 9, BK)
  elseif kind == 2 then                   -- 生成: 骰子
    app.hline(cx - 4, cx + 4, y + 1, BK) app.hline(cx - 4, cx + 4, y + 11, BK)
    app.vline(cx - 5, y + 2, y + 10, BK) app.vline(cx + 5, y + 2, y + 10, BK)
    app.px(cx - 4, y + 1, BK) app.px(cx + 4, y + 1, BK)
    app.px(cx - 4, y + 11, BK) app.px(cx + 4, y + 11, BK)
    app.fill(cx - 3, y + 4, cx - 2, y + 5, BK)
    app.fill(cx + 2, y + 7, cx + 3, y + 8, BK)
    app.fill(cx + 2, y + 3, cx + 3, y + 4, BK)
  elseif kind == 3 then                   -- 预览: 文档
    app.hline(cx - 3, cx + 3, y + 1, BK) app.hline(cx - 3, cx + 3, y + 11, BK)
    app.vline(cx - 4, y + 2, y + 10, BK) app.vline(cx + 4, y + 2, y + 10, BK)
    app.hline(cx - 2, cx + 2, y + 4, BK)
    app.hline(cx - 2, cx + 2, y + 7, BK)
    app.hline(cx - 2, cx + 2, y + 9, BK)
  else                                    -- 操作: ⋯
    app.fill(cx - 6, y + 6, cx - 4, y + 7, BK)
    app.fill(cx - 1, y + 6, cx + 1, y + 7, BK)
    app.fill(cx + 4, y + 6, cx + 6, y + 7, BK)
  end
end

local function draw_tabbar(labels)
  app.hline(0, W - 1, TAB_T, BK)
  for i = 1, 4 do
    local cx = (i - 1) * TAB_W + math.floor(TAB_W / 2)
    tab_icon(i, cx, TAB_T + 10)
    app.zhs(cx - 12, TAB_T + 28, labels[i])
  end
end

-- iOS 式 HUD: 居中黑色胶囊, 2 秒后自动消失 (poll 里重绘)
local function draw_hud(s)
  local w = math.min(tw(s) + 28, W - 40)
  local x0 = math.floor((W - w) / 2)
  fill_round(x0, 130, x0 + w, 156)
  app.mix(x0 + 14, 137, fit(s, w - 28), true)
end

-- 空状态大图标: 38x40 书本 (与 LPK 图标同语义: 一本字典)
local function empty_icon(cx, y0)
  local x0, x1 = cx - 19, cx + 19
  local y1 = y0 + 40
  round_rect_r(x0, y0, x1, y1, 4)
  app.vline(cx, y0 + 5, y1 - 5, BK)                     -- 书脊
  for i = 0, 2 do
    local yy = y0 + 12 + i * 8
    app.hline(x0 + 6, cx - 5, yy, BK)                   -- 左页文字线
    app.hline(cx + 5, x1 - 6, yy, BK)                   -- 右页文字线
  end
end

local function draw_list()
  -- iOS Large Title (本屏最大中文 24px) + 右侧条目数
  app.zh24(CARD_X0 + 6, TITLE_Y, "字典")
  if #G.dicts > 0 then
    draw_right(W - CARD_X0 - 6, TITLE_Y + 4, tostring(#G.dicts) .. " 个")
  end

  -- 卡片高度按实际行数收缩 (iOS grouped list 不会撑满整屏)
  local rows = math.min(#G.dicts, VIS)
  local cbot = math.min(CARD_T + rows * ROW_H, CARD_B)

  if #G.dicts == 0 then
    -- iOS 空状态: 大图标 + 标题 + 一行说明 + 反色主按钮 (不画空卡片)
    empty_icon(200, 62)
    app.zh24(152, 112, "暂无字典")                 -- 4 字 x24 = 96, 居中
    app.mixc(200, 150, "从 TF 卡导入 TXT，或生成随机字典")
    pill(124, 182, 276, 218, "导入 TXT")
    draw_tabbar({ "导入", "生成", "预览", "操作" })
    return
  end

  round_rect(CARD_X0, CARD_T, CARD_X1, cbot)

  -- 分隔线先画 (反色行会盖掉穿过自己的那条), 左端缩进对齐文字
  for i = 1, rows - 1 do
    if G.top + i <= #G.dicts then
      app.hline(30, CARD_X1 - 1, CARD_T + i * ROW_H, BK)
    end
  end

  for i = 0, VIS - 1 do
    local idx = G.top + i
    if idx > #G.dicts then break end
    local y = CARD_T + i * ROW_H
    if y >= cbot then break end                            -- 下边界: 整行在界外不画
    local h = (y + ROW_H > cbot) and (cbot - y) or ROW_H
    local d = G.dicts[idx]
    local sel = (idx == G.sel)
    if sel then
      -- 焦点行反色: 1bpp 没有灰底, 反色是这里唯一稳定的"当前行"表达
      app.fill(CARD_X0 + 1, y + 1, CARD_X1 - 1, y + h - 1, BK)
    end
    if h >= 18 then
      local my = y + 10                                    -- 16px 文字在 37px 行内居中
      local meta = ((d.count == nil) and "..." or (tostring(d.count) .. " 条"))
                   .. " / " .. fmt_size(d.size)
      local mw = tw(meta)
      app.mix(30, my, fit(d.name, CARD_X1 - 22 - mw - 8 - 30), sel)
      draw_right(CARD_X1 - 22, my, meta, sel)
      -- 当前字典: iOS 单选列表的右侧对勾 (不是 chevron —— 这一行没有下一级)
      if d.file == G.cur then checkmark(CARD_X1 - 16, my + 3, sel) end
    end
  end

  draw_tabbar({ "导入", "生成", "预览", "操作" })

  -- 超过一屏时右侧画 iOS 细滚动指示器
  if #G.dicts > VIS then
    local track = cbot - CARD_T
    local h = math.max(18, math.floor(track * VIS / #G.dicts))
    local range = #G.dicts - VIS
    local pos = (range > 0) and math.floor((track - h) * (G.top - 1) / range) or 0
    app.fill(394, CARD_T + pos, 395, CARD_T + pos + h, BK)
  end
end

local function draw_gen()
  nav_bar("生成字典", "返回", "开始")
  section(56, "字符集与参数")
  round_rect(CARD_X0, GEN_T, CARD_X1, GEN_T + #GEN_ROWS * GEN_RH)

  for i, row in ipairs(GEN_ROWS) do
    local y = GEN_T + (i - 1) * GEN_RH
    if i == G.gen_sel then
      -- 焦点行: 卡片内侧 3px 竖条, 与卡片左边框留 3px 空档, 不和边框糊成一块
      app.fill(CARD_X0 + 4, y + 5, CARD_X0 + 6, y + GEN_RH - 6, BK)
    end
    if i > 1 then app.hline(30, CARD_X1 - 1, y, BK) end
    local ty = y + 6
    app.mix(30, ty, row.label)
    if row.kind == "bool" then
      draw_switch(340, y + 3, G.gen[row.k])
    elseif row.kind == "text" then
      local v = G.gen.custom
      draw_right(352, ty, (v == "") and "未设置" or fit(v, 150))
      chevron(CARD_X1 - 18, ty + 8)
    else
      draw_right(325, ty, gen_value(row))
      draw_stepper(331, y + 3)
    end
  end
end

local function draw_preview()
  local d = G.dicts[G.sel]
  local R = G.preview
  local page = R and R.pages[R.page]
  local pno = (R and R.page) or 1
  nav_bar("预览", "返回", tostring(pno) .. "/" .. tostring((R and #R.pages) or 1))

  -- 副标题: 字典名 + 条数 (iOS 导航栏下的说明行)
  local sub = (d and d.name) or ""
  if d and d.count then sub = sub .. "  " .. tostring(d.count) .. " 条" end
  app.mix(CARD_X0 + 6, 59, fit(sub, 360))

  round_rect(CARD_X0, 80, CARD_X1, 248)
  if not page then
    app.mixc(200, 156, "无内容")
  else
    for i, ln in ipairs(page.lines) do
      local y = 82 + (i - 1) * PREV_H
      if y + 16 > 248 then break end                  -- 下边界裁剪
      app.texts(20, y + 4, string.format("%3d", page.first + i - 1))
      app.mix(52, y + 2, fit(show_safe(ln), 324))
    end
  end

  -- 分页: 两个对等的胶囊按钮 (iOS 工具栏不用主次配色, 保持一致)
  pill_outline(CARD_X0, 256, 196, 288, "< 上一页")
  pill_outline(204, 256, CARD_X1, 288, "下一页 >")
end

local function draw_text()
  local T = G.text
  if not T then return end
  nav_bar(T.title, "取消", "完成")

  app.mix(CARD_X0 + 6, 58, fit(T.hint, 360))

  -- iOS 圆角输入框
  round_rect(CARD_X0, 82, CARD_X1, 130)
  local shown = fit(T.buf, 330)
  if T.buf == "" then
    app.mix(CARD_X0 + 26, 98, "点下方键盘输入")     -- 占位提示, 给光标让出左边 1 个字符位
    app.vline(CARD_X0 + 15, 94, 114, BK)
  else
    app.mix(CARD_X0 + 12, 98, shown)
    local cx = CARD_X0 + 13 + tw(shown)
    if cx > CARD_X1 - 10 then cx = CARD_X1 - 10 end
    app.vline(cx, 94, 114, BK)
  end

  app.mix(CARD_X0 + 6, 146, "OK 确定   BACK 取消   点键盘输入")
  app.kbd.draw()
end

local function draw_busy()
  local j = G.job
  nav_bar(G.busy_title or "处理中", nil, nil)

  local pct, line1, line2 = 0, "", ""
  if j and j.kind == "import" then
    local total = app.fs.size(j.src) or 0
    pct = (total > 0) and math.floor(j.off * 100 / total) or 0
    line1 = "已导入 " .. tostring(j.n) .. " 条"
    line2 = "已读取 " .. fmt_size(j.off) .. " / " .. fmt_size(total)
  elseif j and j.kind == "gen" then
    pct = math.floor(j.made * 100 / j.total)
    line1 = tostring(j.made) .. " / " .. tostring(j.total) .. " 条"
    line2 = "目标 " .. j.stem .. ".txt"
  end
  if pct > 100 then pct = 100 end
  if pct < 0 then pct = 0 end

  -- 大号百分比: 8x12 ASCII 整数倍放大 (zh24 只认汉字, 数字会画成空白)
  app.bigc(200, 74, tostring(pct) .. "%", 3)

  -- iOS 进度条: 1px 圆角外框 + 实心填充
  round_rect(60, 130, 340, 148)
  local bw = math.floor(277 * pct / 100)
  if bw > 10 then fill_round(62, 133, 61 + bw, 145) end

  app.mixc(200, 166, line1)
  app.mixc(200, 190, fit(line2, 340))
  app.mixc(200, 214, "每帧处理一批，不阻塞按键")

  pill_outline(110, 252, 290, 288, "取消并删除半成品")
end

local function draw_result()
  local R = G.result or { title = "结果", rows = {} }
  nav_bar(R.title, "返回", "完成")
  -- 卡片按实际行数收缩, 不留一大块空白 (iOS grouped list 的做法)
  local n = math.min(#R.rows, 7)
  local cbot = math.min(74 + n * 28 - 2, 292)
  round_rect(CARD_X0, 60, CARD_X1, cbot)
  for i, s in ipairs(R.rows) do
    local y = 74 + (i - 1) * 28
    if y + 16 > cbot then break end                     -- 下边界裁剪
    app.mix(CARD_X0 + 14, y, fit(s, 340))
  end
end

-- ------------------------------------------------------------------ 契约: render
function render()
  if not G.dirty then return end
  local now = app.millis()
  if (now - G.last_draw) < 32 then return end        -- 两道闸门分开写
  G.last_draw = now
  G.dirty = false
  app.clear(WH)
  -- iOS 顶部状态栏: 只声明"本页要", 时间/电量/信号由 OS 状态栏服务统一刷新
  -- (2026-11-23 统一整改; 原来是"每一帧自己画一次", 与其它应用各画一套)
  if app.statusbar then app.statusbar(true) end
  if G.mode == "list" then draw_list()
  elseif G.mode == "gen" then draw_gen()
  elseif G.mode == "preview" then draw_preview()
  elseif G.mode == "text" then draw_text()
  elseif G.mode == "busy" then draw_busy()
  elseif G.mode == "result" then draw_result()
  end
  -- HUD 最后画, 盖在内容之上
  if G.msg ~= "" and now < G.msg_until then draw_hud(G.msg) end
end

-- ------------------------------------------------------------------ 触摸分发
local function tap(x, y)
  local in_nav = (y >= NAV_T and y <= NAV_B)

  if G.mode == "list" then
    -- 底部 tab bar
    if y >= TAB_T then
      local i = math.floor(x / TAB_W) + 1
      if i == 1 then ask_import()
      elseif i == 2 then G.mode = "gen" dirty()
      elseif i == 3 then open_preview()
      elseif i == 4 then open_menu() end
      return
    end
    -- 卡片行: iOS 单选列表, 点一行 = 选中并立即生效 (不用再点一次"设为当前")
    if y >= CARD_T and y < CARD_B then
      local idx = G.top + math.floor((y - CARD_T) / ROW_H)
      local d = G.dicts[idx]
      if d then
        G.sel = idx
        if G.cur ~= d.file then
          G.cur = d.file
          app.nvs.set_str("wdcur", d.file)
        end
        dirty()
      end
    end

  elseif G.mode == "gen" then
    if in_nav then
      if x >= 280 then begin_gen_from_ui()
      elseif x <= 90 then G.mode = "list" dirty() end
      return
    end
    local ry = y - GEN_T
    if ry >= 0 then
      local i = math.floor(ry / GEN_RH) + 1
      local row = GEN_ROWS[i]
      if row then
        G.gen_sel = i
        if row.kind == "bool" then
          G.gen[row.k] = not G.gen[row.k]
        elseif row.kind == "text" then
          G.text_back = "gen"
          open_text("charset")
          return
        elseif x >= 331 then
          gen_adjust((x >= 356) and 1 or -1)          -- 步进器: 左减右加
        end
        dirty()
      end
    end

  elseif G.mode == "preview" then
    if in_nav then
      if x <= 90 then G.mode = "list" dirty() end
      return
    end
    if y >= 250 then
      if x <= 200 then preview_prev() else preview_next() end
    end

  elseif G.mode == "text" then
    if in_nav then
      if x <= 100 then cancel_text()
      elseif x >= 280 then commit_text() end
      return
    end
    local i = app.kbd.hit(x, y)
    if i and i >= 0 then text_key(i) end

  elseif G.mode == "busy" then
    if y >= 248 then cancel_job() end

  elseif G.mode == "result" then
    if in_nav then G.mode = "list" dirty() end
  end
end

-- ------------------------------------------------------------------ 契约: poll
function poll()
  -- 1) 隔帧发起下一个弹窗 (弹窗回调里直接开弹窗会被框架顶掉)
  if G.next_dlg and not G.dlg then
    local k = G.next_dlg
    G.next_dlg = nil
    if k == "mode" then
      app.dlg.list("导入方式", { "去重 (<=5万条)", "不去重 (不限大小)" }, 1)
      G.dlg = "mode"
      P.on = false
    end
    return
  end

  -- 2) 轮询式弹窗结果; 弹窗期间 poll 不执行, 这一帧不做触摸
  if G.dlg then
    local kind = G.dlg
    local r = app.dlg.result()
    if r == nil then return end
    G.dlg = nil
    handle_dlg(kind, r)
    dirty()
    return
  end

  -- 3) 批处理任务: 每帧推进一步
  if G.job and G.mode == "busy" then
    step_job()
    return
  end
  if G.job and G.job.kind == "count" then step_job() end

  -- 4) 提示过期后重绘一次
  if G.msg ~= "" and app.millis() >= G.msg_until then
    G.msg = ""
    dirty()
  end

  -- 5) 触摸点击状态机
  local tx, ty = app.touch()
  if type(tx) == "number" then
    if not P.on then
      P.on, P.x, P.y, P.t0, P.moved = true, tx, ty, app.millis(), false
    else
      if math.abs(tx - P.x) > 12 or math.abs(ty - P.y) > 12 then P.moved = true end
    end
  elseif P.on then
    P.on = false
    if not P.moved and (app.millis() - P.t0) < 900 then tap(P.x, P.y) end
  end
end

-- ------------------------------------------------------------------ 契约: action
function action(a)
  if a == A_BACK then
    if G.mode == "list" then return false end        -- 交框架弹「退出程序?」
    if G.mode == "busy" then cancel_job()
    elseif G.mode == "text" then cancel_text()
    else G.mode = "list" dirty() end
    return true
  end

  if G.mode == "list" then
    if a == A_UP then move_sel(-1)
    elseif a == A_DOWN then move_sel(1)
    elseif a == A_LEFT then G.top = math.max(1, G.top - VIS) dirty()
    elseif a == A_RIGHT then G.top = math.min(math.max(1, #G.dicts), G.top + VIS) dirty()
    elseif a == A_OK then set_current() end
  elseif G.mode == "gen" then
    if a == A_UP then G.gen_sel = math.max(1, G.gen_sel - 1) dirty()
    elseif a == A_DOWN then G.gen_sel = math.min(#GEN_ROWS, G.gen_sel + 1) dirty()
    elseif a == A_LEFT then gen_adjust(-1)
    elseif a == A_RIGHT then gen_adjust(1)
    elseif a == A_OK then
      if GEN_ROWS[G.gen_sel].kind == "text" then
        G.text_back = "gen"
        open_text("charset")
      else
        begin_gen_from_ui()
      end
    end
  elseif G.mode == "preview" then
    if a == A_UP or a == A_LEFT then preview_prev()
    elseif a == A_DOWN or a == A_RIGHT or a == A_OK then preview_next() end
  elseif G.mode == "text" then
    if a == A_OK then commit_text() end
  elseif G.mode == "busy" then
    -- BACK 已在上面处理; 其余键忽略, 避免误操作
  elseif G.mode == "result" then
    if a == A_OK then G.mode = "list" dirty() end
  end
  return true
end

-- ------------------------------------------------------------------ 契约: 生命周期
function on_enter()
  G.mode = "list"
  G.dirty = true
  G.last_draw = -1000
  if not dir_ensure() then
    toast("TF 卡不可用, 无法创建字典目录", 5000)
    return
  end
  seed_builtin()
  scan_dicts()
  G.cur = app.nvs.get_str("wdcur") or ""
  scan_dicts()                       -- 用持久化值再校正一次显示
  start_count_job()
  dirty()
end

function on_exit()
  -- 任务未完成时退出: 删掉半成品, 不留垃圾文件
  if G.job and G.job.dst and G.job.kind ~= "count" then
    app.fs.remove(G.job.dst)
  end
  G.job = nil
end
