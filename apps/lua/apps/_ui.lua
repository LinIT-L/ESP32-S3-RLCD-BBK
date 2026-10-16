-- _ui.lua — LinTOS Lua 应用公共 UI 组件库 (LVGL 风格 · 1bpp 版 v1.0)
--
-- 用法 (放在 /sdcard/apps/_ui.lua 或 /store/apps/_ui.lua):
--     local UI = dofile("/sdcard/apps/_ui.lua")      -- 有卡优先
--     若不确定卡在不在, 用下面的两行兜底:
--         local ok, UI = pcall(dofile, "/sdcard/apps/_ui.lua")
--         if not ok then UI = dofile("/store/apps/_ui.lua") end
--
-- 设计约定 (见 docs/Lua应用UI规范.md):
--   * 只画不刷屏 —— 调用方在 render() 末尾统一 app.redraw()
--   * 1bpp: 只有黑/白; 层级靠"线宽/线密/反相", 不靠明度
--   * 圆角填充必须逐行裁角 (禁止用 fill 冒充, 会把圆角盖平)
--   * 中英混排必须分两次调用 (zhs/zhsm/zhsb 遇 ASCII 是空白格但照样占位)
--   * 性能: 折线用 app.poly; render 里只 app.clear() 一次, 不反复刷白

local UI = {}

-- ==== 基本常量 ====
UI.W, UI.H   = app.W or 400, app.H or 300
UI.BK, UI.WH = 0, 1                       -- 0=黑(墨) 1=白(底)
UI.C_BK, UI.C_WH = (app.BLACK or 0), (app.WHITE or 1)

-- 四态 (对标 LVGL state)
UI.ST = { NORMAL = 0, PRESSED = 1, FOCUS = 2, DISABLED = 3 }
UI.SIZE_CONTENT = -1

-- 布局骨架 (照规范第四节)
UI.SBAR_H   = 24      -- 状态栏
UI.BAR_H    = 28      -- 底栏/按键区
UI.PAD      = 8       -- 默认内边距
UI.GAP      = 8       -- 块间距
UI.ROW_H16  = 20      -- 16px 汉字行高
UI.ROW_H12  = 16      -- 12px 汉字行高

-- 字体档 (唯一真相源 = ui_common.c; 只允许这 6 档)
UI.FONT = {
    MIX   = "mix",     -- 16px 中文   app.mix(x,y,s,inv)      主力
    ZH12  = "zhm",     -- 12px 中文   app.zhsm(x,y,s,inv)
    ZH12B = "zhb",     -- 12px 粗     app.zhsb(x,y,s,inv)
    ASC   = "asc",     -- 8x12 ASCII  app.texts(x,y,s,inv)
    ASCB  = "ascb",    -- 8x16 ASCII  app.textsb(x,y,s,inv)
    M     = "textm",   -- 中等 ASCII  app.textm(x,y,s,inv)
}

-- ==== 文本 ====

-- UI.text(x, y, s, font, inv)
function UI.text(x, y, s, font, inv)
    font = font or UI.FONT.MIX
    if     font == UI.FONT.MIX  then app.mix(x, y, s, inv)
    elseif font == UI.FONT.ZH12 then app.zhsm(x, y, s, inv)
    elseif font == UI.FONT.ZH12B then app.zhsb(x, y, s, inv)
    elseif font == UI.FONT.ASC  then app.texts(x, y, s, inv)
    elseif font == UI.FONT.ASCB then app.textsb(x, y, s, inv)
    elseif font == UI.FONT.M    then app.textm(x, y, s, inv)
    else app.mix(x, y, s, inv) end
end

-- 水平居中 (cx = 中心 x)
function UI.textc(cx, y, s, font, inv)
    local w = UI.textw(s, font)
    UI.text(math.floor(cx - w / 2 + 0.5), y, s, font, inv)
end

-- 量宽: 中文用 app.mixw; ASCII 按每字 8px
function UI.textw(s, font)
    font = font or UI.FONT.MIX
    if font == UI.FONT.ASC or font == UI.FONT.ASCB or font == UI.FONT.M then
        return #s * 8
    end
    return app.mixw(s) or (#s * 8)
end

-- 中英混排: 分两次画 (ascii_w = 前面用掉的宽度)
function UI.textmix(x, y, zh, asc, inv)
    UI.text(x, y, zh, UI.FONT.MIX, inv)
    UI.text(x + (app.mixw(zh) or #zh * 16), y, asc, UI.FONT.ASC, inv)
end

-- ==== 图元 ====

function UI.px(x, y, c) return app.px(x, y, c or UI.C_BK) end
function UI.hline(x1, x2, y, c) return app.hline(x1, x2, y, c or UI.C_BK) end
function UI.vline(x, y1, y2, c) return app.vline(x, y1, y2, c or UI.C_BK) end
function UI.fill(x1, y1, x2, y2, c) return app.fill(x1, y1, x2, y2, c or UI.C_BK) end

-- 圆角矩形边框 (r>0 时逐行裁角; r>=6 才看得出弧度)
function UI.frame(x, y, w, h, r, c)
    c = c or UI.C_BK
    x, y, w, h = math.floor(x), math.floor(y), math.floor(w), math.floor(h)
    r = math.floor(r or 0)
    if r < 1 then
        app.hline(x, x + w - 1, y, c);  app.hline(x, x + w - 1, y + h - 1, c)
        app.vline(x, y + 1, y + h - 2, c); app.vline(x + w - 1, y + 1, y + h - 2, c)
        return
    end
    if r > w / 2 then r = math.floor(w / 2) end
    if r > h / 2 then r = math.floor(h / 2) end
    app.hline(x + r, x + w - 1 - r, y, c)
    app.hline(x + r, x + w - 1 - r, y + h - 1, c)
    app.vline(x, y + r, y + h - 1 - r, c)
    app.vline(x + w - 1, y + r, y + h - 1 - r, c)
    -- 四角圆弧 (逐行取 inset, 与技能里的 fill_round 同算法)
    for dy = 0, r - 1 do
        local d = r - dy
        local ins = r - math.floor(math.sqrt(r * r - d * d) + 0.5)
        app.px(x + ins,     y + dy, c);         app.px(x + w - 1 - ins, y + dy, c)
        app.px(x + ins,     y + h - 1 - dy, c); app.px(x + w - 1 - ins, y + h - 1 - dy, c)
    end
end

-- ★ 圆角填充: 逐行按 inset 裁掉左右两端 (禁止用 UI.fill 直接盖, 会把圆角盖平)
function UI.fill_round(x, y, w, h, r, c)
    c = c or UI.C_BK
    x, y, w, h = math.floor(x), math.floor(y), math.floor(w), math.floor(h)
    r = math.floor(r or 0)
    if r < 1 then return UI.fill(x, y, x + w - 1, y + h - 1, c) end
    if r > w / 2 then r = math.floor(w / 2) end
    if r > h / 2 then r = math.floor(h / 2) end
    for dy = 0, h - 1 do
        local ins = 0
        if dy < r then
            local d = r - dy
            ins = r - math.floor(math.sqrt(r * r - d * d) + 0.5)
        elseif dy >= h - r then
            local d = r - (h - 1 - dy)
            ins = r - math.floor(math.sqrt(r * r - d * d) + 0.5)
        end
        app.hline(x + ins, x + w - 1 - ins, y + dy, c)
    end
end

-- 虚线水平线 (DISABLED 态用; 靠"线密"表达, 不用颜色)
function UI.hline_dot(x1, x2, y, c)
    c = c or UI.C_BK
    local x = x1
    while x <= x2 do app.px(x, y, c); x = x + 2 end
end

-- ==== 布局 ====

-- 把总宽 total 按比例表 ratios 分成 n 列, 返回各列左边界 x 与宽度
function UI.row(x, y, total, ratios)
    local n, sum = #ratios, 0
    for i = 1, n do sum = sum + ratios[i] end
    local res, cx, used = {}, x, 0
    for i = 1, n do
        local w = math.floor(total * ratios[i] / sum)
        res[i] = { x = cx, w = w, y = y }
        cx = cx + w; used = used + w
    end
    res[n].w = res[n].w + (total - used)      -- 余数补给最后一列
    return res
end

-- 纵向分行 (等分或给定高度表)
function UI.col(x, y, w, heights)
    local res, cy = {}, y
    for i = 1, #heights do
        res[i] = { x = x, y = cy, w = w, h = heights[i] }
        cy = cy + heights[i]
    end
    return res
end

function UI.center(w, h)
    return { x = math.floor((UI.W - w) / 2), y = math.floor((UI.H - h) / 2), w = w, h = h }
end

-- 触摸/按键命中测试
function UI.hit(px, py, x, y, w, h)
    return px >= x and px < x + w and py >= y and py < y + h
end

-- ==== 组件 ====

-- 按钮: 三态表现 —— NORMAL=描边 / PRESSED=整块反相 / DISABLED=虚线
function UI.button(x, y, w, h, txt, st, opt)
    st = st or UI.ST.NORMAL
    opt = opt or {}
    local r = opt.r or 4
    local font = opt.font or UI.FONT.MIX
    if st == UI.ST.PRESSED then
        UI.fill_round(x, y, w, h, r, UI.C_BK)
        UI.textc(x + w / 2, y + math.floor((h - 16) / 2), txt, font, true)
        return { x = x, y = y, w = w, h = h }
    end
    if st == UI.ST.DISABLED then
        UI.hline_dot(x + r, x + w - 1 - r, y, UI.C_BK)
        UI.hline_dot(x + r, x + w - 1 - r, y + h - 1, UI.C_BK)
        UI.textc(x + w / 2, y + math.floor((h - 16) / 2), txt, font, false)
        return { x = x, y = y, w = w, h = h }
    end
    UI.frame(x, y, w, h, r, UI.C_BK)
    if st == UI.ST.FOCUS then                    -- 焦点: 左侧加粗 2px
        UI.fill(x, y + r, x + 1, y + h - 1 - r, UI.C_BK)
    end
    UI.textc(x + w / 2, y + math.floor((h - 16) / 2), txt, font, false)
    return { x = x, y = y, w = w, h = h }
end

-- 列表 (滚动 + 焦点): items={s1,s2,...}; 返回可见区间与内容总高
function UI.list(x, y, w, h, items, sel, top, opt)
    opt = opt or {}
    local rh = opt.row_h or UI.ROW_H16
    local font = opt.font or UI.FONT.MIX
    local vis = math.floor(h / rh)
    top = top or 0
    if sel and sel <= top then top = sel - 1 end
    if sel and sel > top + vis then top = sel - vis end
    if top < 0 then top = 0 end
    if top > #items - vis then top = math.max(0, #items - vis) end
    UI.frame(x, y, w, h, opt.r or 4, UI.C_BK)
    for i = 1, vis do
        local k = top + i
        if k > #items then break end
        local ry = y + (i - 1) * rh
        if k == sel then
            UI.fill(x + 2, ry + 1, x + w - 3, ry + rh - 2, UI.C_BK)   -- 反相选中
            UI.text(x + 6, ry + 2, items[k], font, true)
        else
            UI.text(x + 6, ry + 2, items[k], font, false)
        end
    end
    if #items > vis then                                              -- 右侧滚动条
        local bh = math.max(6, math.floor(h * vis / #items))
        local by = y + math.floor((h - bh) * (top / math.max(1, #items - vis)))
        UI.fill(x + w - 4, by, x + w - 3, by + bh - 1, UI.C_BK)
    end
    return { top = top, vis = vis, total_h = #items * rh }
end

-- 进度/电量条: pct = 0..100
function UI.bar(x, y, w, h, pct, inv)
    pct = math.max(0, math.min(100, pct or 0))
    UI.frame(x, y, w, h, 2, UI.C_BK)
    local iw = math.floor((w - 4) * pct / 100)
    if iw > 0 then
        if inv then UI.fill(x + 2, y + 2, x + 2 + iw - 1, y + h - 3, UI.C_WH)
        else        UI.fill(x + 2, y + 2, x + 2 + iw - 1, y + h - 3, UI.C_BK) end
    end
    return { x = x, y = y, w = w, h = h }
end

-- 开关: 反相 = ON (不用颜色区分)
function UI.switch(x, y, on, w, h)
    w, h = w or 40, h or 18
    if on then
        UI.fill_round(x, y, w, h, h / 2, UI.C_BK)
        UI.fill(x + w - h + 3, y + 3, x + w - 4, y + h - 4, UI.C_WH)   -- 圆钮=白
    else
        UI.frame(x, y, w, h, h / 2, UI.C_BK)
        UI.fill(x + 3, y + 3, x + h - 4, y + h - 4, UI.C_BK)
    end
    return { x = x, y = y, w = w, h = h }
end

-- 勾选框
function UI.check(x, y, on, sz)
    sz = sz or 14
    UI.frame(x, y, sz, sz, 2, UI.C_BK)
    if on then                                       -- 打勾 (两段直线)
        app.hline(x + 3, x + 5, y + sz - 5, UI.C_BK)
        app.hline(x + 6, x + 7, y + sz - 6, UI.C_BK)
        app.hline(x + 8, x + 11, y + 4, UI.C_BK)
        app.hline(x + 8, x + 11, y + 5, UI.C_BK)
    end
    return { x = x, y = y, w = sz, h = sz }
end

-- 面板/浮层: 刷白 + 圆角框 (只有浮层面板才需要刷白)
function UI.panel(x, y, w, h, r)
    UI.fill_round(x, y, w, h, r or 6, UI.C_WH)
    UI.frame(x, y, w, h, r or 6, UI.C_BK)
end

-- ==== 弹窗 (一律走系统, 不自绘) ====
UI.dlg = {
    confirm = function(msg) return app.dlg.confirm(msg) end,
    list    = function(title, items, sel) return app.dlg.list(title, items, sel) end,
    toast   = function(msg, ms) return app.dlg.toast(msg, ms or 1000) end,
    result  = function() return app.dlg.result() end,
}

return UI
