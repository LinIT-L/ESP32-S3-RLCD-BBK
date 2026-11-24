--@lapp v1
--@app name=键盘对比
--@app cat=工具
--@app desc=左右切换看图: 通用文本键盘 5 种 (52键标准/压低/前4排, 42键弹窗/页内)
--@app ver=1
--@app build=202611241800
--
-- ============================================================================
-- 键盘对比 v1 —— 单个 Lua 应用, 一屏看一种键盘, 左右切换 (400x300, 1bpp)
--
-- 目的: 2026-11-24 通用文本键盘精简后, 屏幕上"到底长什么样"需要 linit 自己拍板。
--   本应用**不复制任何键盘绘制代码**, 全部调 C 侧真实模块 (app.kbd.preview):
--     52键 标准/压低/前4排 → ui/kbd_lib/keyboard.c 的 kbd_draw_style (同一张布局表)
--     42键 弹窗/页内       → ui/kbd_lib/kbd_c42.c 的两套几何预设
--   所以看到的像素 = 真机上那些页面里画的像素, 不是"照着重画"的近似品。
--   ⚠ 本应用只是展示器, 不改任何键盘的状态, 也不删任何键盘:
--     preview() 内部用 kbd_std 的**副本**画, 不写全局 caps/shift。
--   ⚠ 只收**通用文本键盘**: 计算器键盘 / 虚拟键鼠(HID) / PIN 九宫格 / 手柄
--     属专用键盘, 不在本展示器范围内 (本轮也不动它们)。
--
-- 操作: 左/右 或 上/下 或 确认 = 翻页; 点屏左 1/3 = 上一个, 其余 = 下一个。
--       BACK = 系统统一"退出程序?"确认 (脚本不接管)。
--
-- 依赖的桥 API (components/apps/lua_bridge/):
--   app.kbd.preview(name, x, y) → 高度   luat_lib_kbd.c  ★ 键盘展示专用
--   app.clear / app.mix / app.mixc / app.mixw / app.hline / app.fill   luat_lib_gfx.c
--   app.touch / app.redraw / app.millis   luat_lib_input.c
-- ============================================================================

local W = 400          -- 屏宽
local TOP = 22         -- 键盘可用区上边界 (顶栏 y=2..18 + 分隔线 y=19 之下)
local BOT = 278        -- 键盘可用区下边界 (底注 y=282..298 之上)

-- 每个条目 = app.kbd.preview() 的一个 name。
-- x/w/h 是**真实几何在 400x300 屏上占的像素**, 与 C 侧算出来的一致 (改 C 侧几何要同步这里):
--   std    : 5 排 × 29 = 145, 满宽 400
--   edit   : 5 排 × 19 = 95 (KBD_EDIT_ROW), 满宽 400
--   rows   : 4 排 × 29 = 116, 满宽 400
--   c42    : 弹窗版满宽 = max(10*34, 9*38, 50+8*30+52, 64+218+60) = 342, 高 5*25 = 125
--   c42page: 页内版满宽 = 60+8*33+49 = 373 (行3 最宽), 高 5*26 = 130
local VIEWS = {
  { id = "std",     x = 0,  w = 400, h = 145,
    name = "52键 标准",
    desc = "全高5排 排高29 总高145 满宽400" },
  { id = "edit",    x = 0,  w = 400, h = 95,
    name = "52键 压低",
    desc = "同一布局表 排高19 总高95 (原kbd_edit)" },
  { id = "rows",    x = 0,  w = 400, h = 116,
    name = "52键 前4排",
    desc = "只画4排 无底排 总高116 (弹窗里用)" },
  { id = "c42",     x = 29, w = 342, h = 125,
    name = "42键 弹窗",
    desc = "342宽 满宽横线 16x24字 排高25 高125" },
  { id = "c42page", x = 13, w = 373, h = 130,
    name = "42键 页内",
    desc = "373宽 每键分割线 16px混排 排高26 高130" },
}

local G = { i = 1, t_down = false }

local function show(n)
  local c = #VIEWS
  if n < 1 then n = c elseif n > c then n = 1 end    -- 首尾循环
  G.i = n
  app.redraw()
end

function on_enter()
  app.statusbar(false)      -- 全屏: 顶栏由本脚本自己画, 不要系统状态栏
  G.i = 1
  G.t_down = false
end

function render()
  app.clear()

  local e = VIEWS[G.i]

  -- 顶栏: 左 = 键盘名, 右 = 序号 + 提示
  app.mix(6, 2, e.name)
  local tip = string.format("%d/%d  < >", G.i, #VIEWS)
  app.mix(W - 6 - app.mixw(tip), 2, tip)
  app.hline(0, W - 1, 19, 0)     -- 第4参是颜色: 0=黑 (ST7305_COLOR_BLACK)

  -- 键盘: 竖向在 [TOP, BOT] 里居中
  -- ⚠ 必须 math.floor: preview 的 y 走 luaL_optinteger, 传小数会报
  --   "number has no integer representation" 而整帧画不出来。
  local y = TOP + math.floor((BOT - TOP - e.h) / 2)
  if y < TOP then y = TOP end
  app.kbd.preview(e.id, e.x, y)

  -- 底注: 这套几何的关键参数 (宽/排高/字高/总高)
  app.mix(6, 282, e.desc)
end

function poll()
  local x = app.touch()
  if x then
    if not G.t_down then                 -- 只在按下那一帧触发, 按住不连翻
      G.t_down = true
      if x < W / 3 then show(G.i - 1) else show(G.i + 1) end
    end
  else
    G.t_down = false
  end
end

function action(a)
  if a == 3 or a == 1 then show(G.i - 1)                -- 左 / 上
  elseif a == 4 or a == 2 or a == 5 then show(G.i + 1)  -- 右 / 下 / 确认
  end
  -- BACK(6) / HOME(7) 不接管: 交给框架走统一"退出程序?"确认
end
