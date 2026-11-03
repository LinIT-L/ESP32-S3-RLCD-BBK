--@lapp v1
--@app name=我的应用 cat=软件 desc=一句话说明 version=1
--@app icon=/////////////////////8AAAAAAAAADwAAAAAAAAAPAAAAAAAAAA8AAAAAAAAADwAAAAAAAAAPAAAAAAAAAA8AAAAAAAAADwAAAAAAAAAPAAAAP///8A8AAAA////wDwAAAD////APAAAAP///8A8AAAA////wDwAAAD////APAAAAP///8A8AAAA////wDwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAD/AAAAPAAAAP8AAAA8AAAA/wAAADwAAAAAAAAAPAAAAAAAAAA8AAAAAAAAADwAAAAAAAAAPAAAAAAAAAA8AAAAAAAAADwAAAAAAAAAPAAAAAAAAAA/////////////////////8=
--
-- ── LinTOS 最小可运行 Lua 应用模板 ────────────────────────────────────────────
-- 用之前请改这几处:
--   1) 上面第二行的 name / cat / desc / version
--   2) render() 里想显示的内容
--   3) action() 里想响应的按键
--
-- 约定(与固件一致, 改错会"界面全白且无报错"):
--   * 契约函数必须是**全局函数**。写成 local function render() 桥侧看不到。
--   * render() 每帧都会被调用, 只画图, 不要在里面读文件/联网/睡等。
--   * action(a) 返回 false = 交回框架处理(比如 BACK 会弹"退出程序?")。
--   * 想让退出不弹确认, 可调用 app.exit_confirm(false)。
--   * 第三方应用的数据目录是 /sdcard/apps/.data/<应用ID>/,
--     **用相对路径就行**: app.fs.write("save.dat", ...) 会自动落到那里。
--     绝对路径只允许指向自己的数据目录, 其它一律 permission denied。
--
-- 第三方应用**不能用**的能力(固件里已强制, 不是靠自觉):
--   app.hid.*          USB 键鼠 / BadUSB
--   app.wiki.open      指定目录读维基库
--   app.rec.start      麦克风录音
--   io.popen / os.execute / loadfile / dofile / package.loadlib
--   require 加载磁盘模块(单文件应用不需要)
--
-- 可用资源上限(第三方): Lua 内存 4 MB, 单次运行 10 秒(死循环会被强制停止)。
-- ────────────────────────────────────────────────────────────────────────────

local S = { t0 = 0, n = 0 }

function on_enter()
  S.t0 = app.millis()
  S.n  = 0
end

function render()
  app.clear(app.WHITE)
  app.mixc(app.W // 2, 40, "我的应用")          -- 16px 中英混排, 水平居中
  app.textc(96,  "按 OK 加一")                  -- 8x16 ASCII, 按屏宽居中
  app.textc(120, "计数: " .. S.n)
  app.textc(160, "已运行 " .. ((app.millis() - S.t0) // 1000) .. " 秒")
end

function action(a)
  if a == app.OK or a == app.CONFIRM then
    S.n = S.n + 1
    return true
  end
  if a == app.BACK then return false end    -- 交回框架: 弹"退出程序?"
  return true
end

function poll()
  -- 每帧尾巴: 只做非阻塞的事(比如查 app.net.fetch_poll)
end

function on_exit()
  -- 退出清理: 停定时器、释放句柄。不写也不会崩, 但建议写。
end
