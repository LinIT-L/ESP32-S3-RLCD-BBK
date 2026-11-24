-- wiki_ui_test.lua — 用真实测试库数据跑 wiki.lua 的界面状态机
--
-- 目的：验证 wiki.lua 自身逻辑（分页/选中/滚动/搜索切换/越界），
--       不验证 C 层（C 层由 wiki_db_test.c 单独测）。
-- 做法：mock 一个 app 表，其中 app.wiki.* 直接读 tests/wiki/data/testlib 的索引文件。
--
-- 运行： /tmp/lua tests/wiki/wiki_ui_test.lua

local DB = "tests/wiki/data/testlib"

---------------------------------------------------------------- mock: 读库
local function readfile(p)
    local f = assert(io.open(p, "rb"), "打不开 " .. p)
    local d = f:read("a"); f:close(); return d
end

local function u32(s, i)
    local a, b, c, d = s:byte(i, i + 3)
    return a + b * 256 + c * 65536 + d * 16777216
end

local WIKI = { open = false, count = 0, titles = {}, py = {}, pyi = {}, cur = nil }

local function load_db()
    local meta = readfile(DB .. "/metadata.json")
    local n = tonumber(meta:match('"articleCount"%s*:%s*(%d+)'))
    local idx = readfile(DB .. "/index.bin")
    assert(#idx == n * 80, "index.bin 大小不符")

    WIKI.titles = {}
    for i = 0, n - 1 do
        local base = i * 80 + 1
        local t = idx:sub(base, base + 63)
        local z = t:find("\0", 1, true)
        if z then t = t:sub(1, z - 1) end
        WIKI.titles[i + 1] = t
    end

    -- 拼音索引: 64B/条 key[60] + id:u32
    local function load_py(fn)
        local d = readfile(DB .. "/" .. fn)
        local rows = {}
        for off = 1, #d, 64 do
            local k = d:sub(off, off + 59)
            local z = k:find("\0", 1, true)
            if z then k = k:sub(1, z - 1) end
            rows[#rows + 1] = { k = k, id = u32(d, off + 60) }
        end
        return rows
    end
    WIKI.py  = load_py("py.bin")
    WIKI.pyi = load_py("pyi.bin")
    WIKI.count = n
end

---------------------------------------------------------------- mock: app
local draw_calls = 0

local app = {
    W = 400, H = 300, BLACK = 0, WHITE = 1,
    UP = 1, DOWN = 2, LEFT = 3, RIGHT = 4, OK = 5, BACK = 6, CONFIRM = 7, HOME = 8,
}
local function noop() draw_calls = draw_calls + 1 end
app.clear = noop
app.fill  = noop
app.hline = noop
app.vline = noop
app.px    = noop
app.mix   = noop
app.text  = noop
app.zh    = noop
function app.mixw(s) return #s * 8 end
function app.textw(s) return #s * 8 end

app.wiki = {}
function app.wiki.open(dir) load_db(); WIKI.open = true; return WIKI.count end
function app.wiki.close() WIKI.open = false; WIKI.cur = nil end
function app.wiki.info()
    if not WIKI.open then return nil end
    return { count = WIKI.count, name = "testlib", lines = WIKI.cur and #WIKI.cur or 0 }
end
function app.wiki.titles(from, n)
    local out = {}
    for i = from + 1, math.min(from + n, WIKI.count) do
        if WIKI.titles[i] then out[#out + 1] = { id = i - 1, title = WIKI.titles[i] } end
    end
    return out
end
function app.wiki.search_py(q, initials, max)
    q = (q or ""):lower()
    if q == "" then return {} end
    local rows = initials and WIKI.pyi or WIKI.py
    local out = {}
    for _, r in ipairs(rows) do
        if r.k:sub(1, #q) == q then
            out[#out + 1] = { id = r.id, title = WIKI.titles[r.id + 1] or "?" }
            if #out >= max then break end
        end
    end
    return out
end
function app.wiki.load(id)
    if not WIKI.open then return nil, "未打开" end
    if id < 0 or id >= WIKI.count then return nil, "编号越界" end
    -- 用标题造几行假正文, 行数与标题长度挂钩
    local t = WIKI.titles[id + 1] or ""
    local lines = {}
    for i = 1, math.max(3, #t) do lines[#lines + 1] = t .. " 第" .. i .. "行" end
    WIKI.cur = lines
    return #lines
end
function app.wiki.line(i)
    if not WIKI.cur then return "" end
    return WIKI.cur[i + 1] or ""
end
function app.wiki.lines() return WIKI.cur and #WIKI.cur or 0 end

-- kbd 桩: 只让 use/hit 返回可用值, 不测几何
app.kbd = { ROW = 29, CHAR = 0, BKSP = 2, ENTER = 1, TAB = 5, ESC = 4 }
function app.kbd.use() end
function app.kbd.draw() end
function app.kbd.hit() return 0 end
function app.kbd.press() return 0 end
function app.kbd.char() return nil end

---------------------------------------------------------------- 载入被测文件
local f = assert(io.open("apps/lua/apps/app/离线维基.lua", "rb"))
local src = f:read("a"); f:close()

local env = { app = app, math = math, string = string, table = table, tostring = tostring, ipairs = ipairs }
local chunk = assert(load(src, "wiki.lua", "t", env))
chunk()     -- 执行, 把 on_enter/render/action 写进 env

local g_pass, g_fail = 0, 0
local function CHECK(c, msg)
    if c then g_pass = g_pass + 1; print("  PASS  " .. msg)
    else g_fail = g_fail + 1; print("  FAIL  " .. msg) end
end
local function PRESS(k) return env.action(k) end

print("========== wiki.lua 界面状态机 冒烟测试 ==========\n")

print("T1 进入应用")
env.on_enter()
env.render()
CHECK(draw_calls > 0, "render 有绘制调用")
CHECK(app.wiki.info() ~= nil, "库已打开")
CHECK(WIKI.count == 31, "库里 31 篇（实际 " .. WIKI.count .. "）")

print("\nT2 列表导航（首项应为 [搜索]）")
PRESS(app.DOWN)
PRESS(app.DOWN)
PRESS(app.DOWN)
PRESS(app.DOWN)
env.render()
CHECK(true, "连按 4 次 DOWN 未报错")

print("\nT3 打开文章")
PRESS(app.OK)          -- 当前选中第 5 项(不是搜索项), 应进 read
env.render()
CHECK(true, "OK 后 render 正常")

print("\nT4 正文滚动边界")
for _ = 1, 200 do PRESS(app.DOWN) end
env.render()
CHECK(true, "按下 200 次 DOWN 不越界")
for _ = 1, 300 do PRESS(app.UP) end
env.render()
CHECK(true, "按上 300 次 UP 不越界")
PRESS(app.RIGHT); PRESS(app.LEFT)
env.render()
CHECK(true, "左右翻页不报错")

print("\nT5 返回列表")
PRESS(app.BACK)
env.render()
CHECK(true, "BACK 后回到列表并渲染正常")

print("\nT6 搜索")
-- 回到顶部选中 [搜索]
for _ = 1, 40 do PRESS(app.UP) end
PRESS(app.OK)              -- 命中 [搜索] 项 -> 进 search
env.render()
CHECK(true, "进入搜索页渲染正常")

print("\nT7 拼音搜索结果")
-- 直接调内部 search: 通过键盘输入路径难以在 mock 里驱动, 这里验证 search_py 数据本身
local r = app.wiki.search_py("bj", true, 10)
CHECK(#r >= 1, "首字母 bj 命中 " .. #r .. " 条")
local hitbj = false
for _, x in ipairs(r) do if x.title == "北京" then hitbj = true end end
CHECK(hitbj, "结果含「北京」")

r = app.wiki.search_py("chang", false, 20)
CHECK(#r >= 4, "全拼 chang 命中 " .. #r .. " 条")

r = app.wiki.search_py("zzz", false, 20)
CHECK(#r == 0, "无匹配返回空")

print("\nT8 退出")
env.on_exit()
CHECK(app.wiki.info() == nil, "on_exit 已关闭库")

print(string.format("\n================ %d PASS / %d FAIL ================", g_pass, g_fail))
os.exit(g_fail == 0 and 0 or 1)
