--@lapp v1
--@app name=离线维基
--@app cat=学习
--@app desc=离线维基百科: 拼音搜索 + 长文阅读, 数据在 TF 卡 /sdcard/wiki/
--@app version=1
--@app build=202610311500
--
-- ============================================================================
-- wiki.lua — 离线维基阅读器 (Lua 单文件应用)
--
-- 分工:
--   C 层 (components/services/wiki) 干重活: SD 随机读 / 80B 索引二分 / LZ4 解压 /
--   HTML→纯文本 / 按 384px 折行。本文件只负责"画"和"按键"。
--
-- 数据 (TF 卡, 与应用本体完全分离, 卸载应用不会动它):
--   /sdcard/wiki/<库名>/metadata.json · index.bin · articles_0000.dat · py.bin · pyi.bin
--
-- 为什么搜索只能是拼音:
--   掌机只有三个实体键 + 一块触摸屏, 系统键盘 (app.kbd) 是 **52 键 ASCII**,
--   不能直接打中文。所以 C 层专门提供 py.bin(全拼) / pyi.bin(首字母) 两套索引,
--   输入 "bj" 或 "beijing" 都能命中「北京」。
--
-- 三种界面:
--   list    全部词条, 上下选, 确认进正文
--   search  拉起键盘输入拼音, 实时出结果
--   read    正文, 上下滚行 / 左右翻页
-- ============================================================================

local W, H = app.W, app.H
local BK, WH = app.BLACK, app.WHITE

local ROW_H   = 22          -- 列表行高
local TOP     = 30          -- 标题栏下沿
local FOOT    = 18          -- 底部提示区高
local LIST_N  = 10          -- 一屏几行
local READ_N  = 13          -- 正文一屏几行 (16px 行高 20)

local S = {
    mode   = "list",
    err    = nil,
    name   = "",
    total  = 0,
    items  = {},            -- {{id=,title=}}
    sel    = 1,             -- 选中第几项 (1 基)
    top    = 0,             -- 列表首项在 items 里的下标 (1 基)
    -- 搜索
    q      = "",            -- 当前拼音
    kbx    = 4,
    kby    = 0,
    kidx   = -1,
    kb_on  = false,
    -- 正文
    lines  = 0,
    vtop   = 0,
    id     = -1,
}

-- ---------------------------------------------------------------- 小工具
local function clamp(v, lo, hi) if v < lo then return lo elseif v > hi then return hi else return v end end

local function ellipsis(s, maxpx)
    -- 粗略按"汉字 16px / 其它 8px"截断, 超宽加省略号(用 .. )
    local w, i = 0, 1
    local n = #s
    while i <= n do
        local b = s:byte(i)
        local step, adv
        if b >= 0xE0 then step, adv = 3, 16 else step, adv = 1, 8 end
        if w + adv > maxpx - 16 then
            return s:sub(1, i - 1) .. ".."
        end
        w = w + adv
        i = i + step
    end
    return s
end

-- ---------------------------------------------------------------- 数据
local function load_all()
    S.items = {}
    local from = 0
    while true do
        local rows = app.wiki.titles(from, 32)
        if not rows or #rows == 0 then break end
        for i = 1, #rows do S.items[#S.items + 1] = rows[i] end
        from = from + #rows
        if #S.items >= 2000 then break end        -- 防呆: 不至于把内存吃满
    end
    -- 首项固定放一个「搜索」入口: 掌机只有三键, 没有独立搜索键,
    -- 用列表项进搜索比占用 HOME/CONFIRM 更不容易和系统键冲突。
    table.insert(S.items, 1, { id = -1, title = "[搜索]" })
    S.total = #S.items
    S.sel, S.top = 1, 1
end

local function do_search(q)
    if q == nil or q == "" then return end
    local out, seen = {}, {}
    -- 先按首字母(bj), 再按全拼(beijing), 合并去重
    for _, initials in ipairs({ true, false }) do
        local r = app.wiki.search_py(q, initials, 30)
        if r then
            for i = 1, #r do
                if not seen[r[i].id] then
                    seen[r[i].id] = true
                    out[#out + 1] = r[i]
                end
            end
        end
    end
    S.items = out
    S.total = #out
    S.sel, S.top = 1, 1
end

local function open_article(id)
    local n = app.wiki.load(id)
    if not n or n <= 0 then
        S.err = "这一篇打不开"
        return false
    end
    S.lines = n
    S.id = id
    S.vtop = 0
    return true
end

-- 进入搜索: 拉起底部三排键 (52 键 ASCII, 只能打拼音)
local function enter_search()
    S.mode = "search"
    S.q, S.items, S.total, S.sel = "", {}, 0, 1
    S.from_search = false
    S.kb_on = true
    app.kbd.use("rows", 3)
    S.kby = H - 3 * app.kbd.ROW + math.floor(app.kbd.ROW / 2)
    S.kbx = 4
    S.kidx = app.kbd.hit(S.kbx, S.kby)
end

-- 键盘收起: 有结果就用列表展示(复用 list 的选中/滚动), 没结果留在搜索页
local function finish_search()
    S.kb_on = false
    if S.total > 0 then
        S.mode = "list"
        S.from_search = true
        S.sel, S.top = 1, 1
    end
end

-- ---------------------------------------------------------------- 画
local function draw_title()
    app.fill(0, 0, W - 1, TOP - 8, WH)
    local nm = S.name ~= "" and S.name or "离线维基"
    app.mix(8, 4, nm, false)
    local cnt = (S.total > 0) and (S.total - 1) or 0    -- 减掉「[搜索]」那一项
    local t = tostring(cnt) .. " 篇"
    local tw = app.mixw(t)
    app.text(W - 8 - tw, 8, t, false)
    app.hline(0, W - 1, TOP - 6, BK)
end

local function draw_foot(txt)
    app.hline(0, H - FOOT - 2, W - 1, BK)
    app.text(8, H - FOOT + 2, txt, false)
end

local function draw_list()
    app.clear(WH)
    draw_title()

    local y = TOP
    for i = 0, LIST_N - 1 do
        local idx = S.top + i
        local it = S.items[idx]
        if not it then break end
        local sel = (idx == S.sel)
        if sel then
            app.fill(4, y - 2, W - 5, y + ROW_H - 5, BK)
            app.mix(10, y, ellipsis(it.title, W - 30), true)
        else
            app.mix(10, y, ellipsis(it.title, W - 30), false)
        end
        y = y + ROW_H
    end

    if S.total <= 1 then                       -- 只有「[搜索]」一项 = 库是空的
        app.mix(10, TOP + 20, "这里还没有词条", false)
        app.text(10, TOP + 46, "把 wiki 数据放进 /sdcard/wiki/", false)
    end

    local foot = S.from_search and "上下选择  确认阅读  返回重新浏览" or "上下选择  确认阅读  返回退出"
    draw_foot(foot)
end

local function draw_search()
    app.clear(WH)
    app.mix(8, 4, "搜索(拼音)", false)
    app.hline(0, W - 1, 24, BK)

    -- 输入行
    local shown = S.q == "" and "_" or (S.q .. "_")
    app.mix(8, 30, shown, false)

    -- 结果预览 (最多 3 条)
    local y = 54
    for i = 1, math.min(3, S.total) do
        local it = S.items[i]
        if it then
            app.mix(16, y, ellipsis(it.title, W - 40), false)
            y = y + 20
        end
    end
    if S.total == 0 and S.q ~= "" then
        app.text(16, 56, "no match", false)
    end

    if S.kb_on then
        app.kbd.draw(S.kidx)
    end
end

local function draw_read()
    app.clear(WH)
    -- 顶栏: 标题 + 进度
    local title = ""
    for i = 1, #S.items do
        if S.items[i].id == S.id then title = S.items[i].title break end
    end
    app.mix(8, 4, ellipsis(title, W - 90), false)
    local pct = S.lines > 0 and math.floor((S.vtop + READ_N) * 100 / S.lines) or 0
    if pct > 100 then pct = 100 end
    local ps = tostring(pct) .. "%"
    app.text(W - 8 - app.textw(ps), 8, ps, false)
    app.hline(0, W - 1, 24, BK)

    local y = 30
    for i = 0, READ_N - 1 do
        local li = S.vtop + i
        if li >= S.lines then break end
        local s = app.wiki.line(li)
        if s and #s > 0 then
            app.mix(8, y, s, false)
        end
        y = y + 20
    end

    draw_foot("上下滚动  左右翻页  返回列表")
end

-- ---------------------------------------------------------------- 回调
function on_enter()
    S.mode, S.err = "list", nil
    local n, err = app.wiki.open()
    if not n then
        S.mode = "err"
        S.err = err or "没有找到维基库"
        return
    end
    local info = app.wiki.info()
    if info then
        S.name = info.name or ""
    end
    load_all()
    if S.total == 0 then
        S.err = "库里没有词条"
    end
end

function render()
    if S.mode == "err" then
        app.clear(WH)
        app.mix(12, 20, "离线维基", false)
        app.hline(12, W - 13, 46, BK)
        app.mix(12, 60, S.err or "未知错误", false)
        app.text(12, 100, "put wiki data in /sdcard/wiki/<name>/", false)
        app.text(12, 120, "need metadata.json + index.bin", false)
        draw_foot("返回退出")
        return
    end
    if S.mode == "read" then draw_read()
    elseif S.mode == "search" then draw_search()
    else draw_list() end
end

function action(a)
    -- ---- 错误页 ----
    if S.mode == "err" then
        return false
    end

    -- ---- 搜索页: 键盘导航 ----
    if S.mode == "search" then
        if S.kb_on then
            local moved = false
            if a == app.LEFT then
                local nx = S.kbx - 48
                if nx < 0 then nx = W - 6 end
                if app.kbd.hit(nx, S.kby) >= 0 then S.kbx = nx; moved = true end
            elseif a == app.RIGHT then
                local nx = S.kbx + 48
                if nx > W - 1 then nx = 4 end
                if app.kbd.hit(nx, S.kby) >= 0 then S.kbx = nx; moved = true end
            elseif a == app.UP then
                local ny = S.kby - app.kbd.ROW
                if app.kbd.hit(S.kbx, ny) >= 0 then S.kby = ny; moved = true end
            elseif a == app.DOWN then
                local ny = S.kby + app.kbd.ROW
                if app.kbd.hit(S.kbx, ny) >= 0 then S.kby = ny; moved = true end
            end
            if moved then
                S.kidx = app.kbd.hit(S.kbx, S.kby)
                return true
            end
            if a == app.OK or a == app.CONFIRM then
                local i = S.kidx
                if i and i >= 0 then
                    local act = app.kbd.press(i)
                    local ch = app.kbd.char(i)
                    if act == app.kbd.CHAR and ch then
                        if #S.q < 16 then
                            S.q = S.q .. string.lower(ch)
                            do_search(S.q)
                        end
                    elseif act == app.kbd.BKSP then
                        S.q = S.q:sub(1, -2)
                        do_search(S.q)
                    elseif act == app.kbd.ENTER or act == app.kbd.TAB then
                        finish_search()
                    elseif act == app.kbd.ESC then
                        finish_search()
                    end
                end
                return true
            end
            if a == app.BACK then
                finish_search()
                return true
            end
            return true
        end
        -- 键盘已收起但没结果: 只能返回
        if a == app.BACK or a == app.OK or a == app.CONFIRM then
            load_all()
            S.from_search = false
            S.mode = "list"
        end
        return true
    end

    -- ---- 正文 ----
    if S.mode == "read" then
        if a == app.UP then
            S.vtop = math.max(0, S.vtop - 1)
        elseif a == app.DOWN then
            S.vtop = math.min(math.max(0, S.lines - 1), S.vtop + 1)
        elseif a == app.LEFT then
            S.vtop = math.max(0, S.vtop - READ_N)
        elseif a == app.RIGHT then
            S.vtop = math.min(math.max(0, S.lines - 1), S.vtop + READ_N)
        elseif a == app.BACK then
            S.mode = "list"          -- 回到列表(搜索结果仍保留在 S.items 里)
        else
            return true
        end
        return true
    end

    -- ---- 列表 ----
    if a == app.UP then
        S.sel = math.max(1, S.sel - 1)
    elseif a == app.DOWN then
        S.sel = math.min(math.max(1, S.total), S.sel + 1)
    elseif a == app.LEFT then
        S.sel = math.max(1, S.sel - LIST_N)
    elseif a == app.RIGHT then
        S.sel = math.min(math.max(1, S.total), S.sel + LIST_N)
    elseif a == app.OK or a == app.CONFIRM then
        local it = S.items[S.sel]
        if it and it.id == -1 then
            enter_search()
        elseif it and open_article(it.id) then
            S.mode = "read"
        end
    elseif a == app.BACK then
        if S.from_search then
            load_all()                 -- 搜索结果 → 退回全部词条
            S.from_search = false
        else
            return false               -- 已经在全部列表 → 交回框架(退出确认)
        end
    end

    -- 保持选中项在可视区内
    if S.sel < S.top then S.top = S.sel end
    if S.sel > S.top + LIST_N - 1 then S.top = S.sel - LIST_N + 1 end
    if S.top < 1 then S.top = 1 end
    return true
end

function on_exit()
    app.wiki.close()
end
